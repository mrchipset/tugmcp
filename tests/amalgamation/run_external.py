#!/usr/bin/env python3
"""Validate the HTTP single-header artifact with external MCP protocol tools.

Steps:
  1. generate ``tugmcp.hpp`` and ``tugmcp_http.hpp`` and verify their recorded
     SHA256 values;
  2. configure/build the standalone consumers (core stdio server, HTTP server
     and the conformance fixture server) from those generated headers only;
  3. run MCP Inspector 2.12 CLI against the stdio server for
     ``initialize``/``tools/list``/``tools/call``;
  4. run the same Inspector CLI against the Streamable HTTP server for
     ``tools/list``/``tools/call``;
  5. run the official conformance suite against the single-header fixture server
     for 2026-07-28 and 2025-11-25.

The script fails loudly when Node/npx is missing: external verification is a
required part of this test, never a silent skip.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional, Sequence

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools" / "amalgamate.py"
CONSUMER_PROJECT = ROOT / "tests" / "amalgamation"
CONFORMANCE_RUNNER = ROOT / "tests" / "conformance" / "run_external.py"
EXPECTED_FAILURES = ROOT / "tests" / "conformance" / "expected_failures.yaml"
INSPECTOR_PACKAGE = "@modelcontextprotocol/inspector@2.12.0"
SERVERS = ("tugmcp_amalgamation_stdio_server",
           "tugmcp_amalgamation_http_server",
           "tugmcp_amalgamation_fixture_server")
BUILD_CONFIG = "Debug"


class VerificationError(RuntimeError):
    pass


def printable(command: Sequence[str]) -> str:
    return " ".join(str(part) for part in command)


def run(command: Sequence[str], cwd: Path | None = None,
        timeout: Optional[int] = None) -> None:
    command = [str(part) for part in command]
    print(f"+ {printable(command)}", flush=True)
    completed = subprocess.run(command, cwd=str(cwd or ROOT), timeout=timeout)
    if completed.returncode != 0:
        raise VerificationError(
            f"command failed with exit code {completed.returncode}: "
            f"{printable(command)}"
        )


def npx_command() -> list[str]:
    npx_name = "npx.cmd" if os.name == "nt" else "npx"
    npx = shutil.which(npx_name)
    node = shutil.which("node.exe" if os.name == "nt" else "node")
    if not npx:
        raise VerificationError(f"{npx_name} was not found on PATH")
    if not node:
        raise VerificationError("node was not found on PATH")
    return [npx]


def inspector_command(prefix: Sequence[str], args: Sequence[str]) -> list[str]:
    command = [*prefix, "-y", INSPECTOR_PACKAGE, "--cli", "--format", "json",
               *args]
    if os.name == "nt":
        return ["cmd", "/c", *command]
    return command


def run_inspector(prefix: Sequence[str], args: Sequence[str],
                  timeout: int = 180) -> dict:
    command = inspector_command(prefix, args)
    print(f"+ {printable(command)}", flush=True)
    completed = subprocess.run(
        command, cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=timeout)
    if completed.returncode != 0:
        raise VerificationError(
            "MCP Inspector failed "
            f"(exit {completed.returncode}): {printable(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise VerificationError(
            "MCP Inspector did not emit a JSON envelope: "
            f"{error}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        ) from error
    if isinstance(payload, dict) and "error" in payload:
        raise VerificationError(
            f"MCP Inspector returned an error: {payload['error']}\n"
            f"stderr:\n{completed.stderr}"
        )
    return payload


def generate(profile: str, out: Path, hash_file: Path) -> None:
    out = out.resolve()
    hash_file = hash_file.resolve()
    if not hash_file.is_file():
        raise VerificationError(f"recorded hash file not found: {hash_file}")
    run([sys.executable, str(GENERATOR), "--profile", profile, "--out", str(out)])
    run([sys.executable, str(GENERATOR), "--profile", profile, "--check",
         str(out), "--hash-file", str(hash_file)])
    size = out.stat().st_size
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"generated {profile}: {out} ({size} bytes, sha256 {digest})",
          flush=True)


def check_http_artifact(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = "#ifdef TUGMCP_IMPLEMENTATION"
    if text.count(marker) != 1:
        raise VerificationError("HTTP artifact has no unique implementation marker")
    prelude, implementation = text.split(marker, 1)
    if "#ifndef TUGMCP_SINGLE_HEADER_HTTP_HPP" not in prelude:
        raise VerificationError("HTTP artifact does not use its own guard")
    if ("merged from: include/tugmcp/detail/http_httplib.hpp" in prelude or
            "#include <httplib.h>" in prelude):
        raise VerificationError(
            "detail/http_httplib.hpp leaked into the declaration prelude")
    if implementation.count("#include <httplib.h>") != 1:
        raise VerificationError(
            "cpp-httplib must be included exactly once in the implementation")
    if "CPPHTTPLIB_OPENSSL_SUPPORT" not in implementation:
        raise VerificationError(
            "cpp-httplib TLS macro handling is missing from the implementation")
    if "#define TUGMCP_TLS_ENABLED 0" not in implementation:
        raise VerificationError(
            "non-TLS TUGMCP_TLS_ENABLED default is missing")
    if "transport/legacy_sse.hpp" in text:
        raise VerificationError("legacy SSE must not enter the HTTP artifact")


def configure_and_build(args: argparse.Namespace) -> Path:
    build_dir = args.build_dir.resolve()
    run([
        "cmake", "-S", str(CONSUMER_PROJECT), "-B", str(build_dir),
        f"-DTUGMCP_AMALGAMATION_HEADER={args.core_out.resolve()}",
        f"-DTUGMCP_AMALGAMATION_HTTP_HEADER={args.http_out.resolve()}",
        f"-DTUGMCP_JSON_INCLUDE={args.json_include.resolve()}",
        f"-DTUGMCP_HTTPLIB_INCLUDE={args.httplib_include.resolve()}",
        f"-DCMAKE_BUILD_TYPE={BUILD_CONFIG}",
    ])
    run([
        "cmake", "--build", str(build_dir), "--config", BUILD_CONFIG,
        "--parallel",
    ])
    for name in SERVERS:
        locate_executable(build_dir, name)
    return build_dir


def locate_executable(build_dir: Path, name: str) -> Path:
    suffixes = [".exe", ""] if os.name == "nt" else [""]
    candidates: list[Path] = []
    for directory in (build_dir / BUILD_CONFIG, build_dir):
        for suffix in suffixes:
            candidates.append(directory / f"{name}{suffix}")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    matches = sorted(build_dir.rglob(f"{name}*"))
    for match in matches:
        if match.is_file() and match.suffix in (".exe", ""):
            return match
    raise VerificationError(f"built executable not found: {name} in {build_dir}")


def start_server(executable: Path, arguments: Sequence[str]) -> tuple[
        subprocess.Popen, int, list[str]]:
    command = [str(executable), *arguments]
    print(f"+ start {printable(command)}", flush=True)
    process = subprocess.Popen(
        command, cwd=str(ROOT), stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, text=True, encoding="utf-8",
        errors="replace", bufsize=1)
    lines: "queue.Queue[str]" = queue.Queue()
    stderr_lines: list[str] = []

    def pump() -> None:
        assert process.stderr is not None
        for line in process.stderr:
            stderr_lines.append(line)
            lines.put(line)

    threading.Thread(target=pump, daemon=True).start()

    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise VerificationError(
                "server exited during startup:\n" + "".join(stderr_lines))
        try:
            line = lines.get(timeout=0.5)
        except queue.Empty:
            continue
        match = re.search(r"listening on http://[^:]+:(\d+)/mcp", line)
        if match:
            return process, int(match.group(1)), stderr_lines
    stop_process(process)
    raise VerificationError(
        "server did not report a bound port within 20s:\n"
        + "".join(stderr_lines))


def stop_process(process: Optional[subprocess.Popen]) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def has_echo_tool(payload: dict) -> bool:
    result = payload.get("result")
    if not isinstance(result, dict):
        return False
    tools = result.get("tools")
    if not isinstance(tools, list):
        return False
    return any(isinstance(tool, dict) and tool.get("name") == "echo"
               for tool in tools)


def call_text(payload: dict) -> str:
    result = payload.get("result")
    if not isinstance(result, dict):
        raise VerificationError(f"tools/call has no result: {payload}")
    blocks = result.get("content")
    if not isinstance(blocks, list):
        raise VerificationError(f"tools/call has no content: {payload}")
    texts = [block.get("text", "") for block in blocks
             if isinstance(block, dict) and block.get("type") == "text"]
    return "\n".join(texts)


def inspector_stdio(npx: Sequence[str], executable: Path) -> None:
    initialize = run_inspector(npx, ["--method", "initialize", str(executable)])
    server_info = initialize.get("result", {}).get("serverInfo", {})
    if server_info.get("name") != "amalgamation-echo":
        raise VerificationError(
            f"unexpected initialize result: {initialize}")

    tools = run_inspector(npx, ["--method", "tools/list", str(executable)])
    if not has_echo_tool(tools):
        raise VerificationError(f"echo tool missing from tools/list: {tools}")

    call = run_inspector(npx, [
        "--method", "tools/call", "--tool-name", "echo",
        "--tool-args-json", '{"text":"stdio-amalgamation"}', str(executable)])
    text = call_text(call)
    if "stdio-amalgamation" not in text:
        raise VerificationError(
            f"echo tool did not return the expected text: {call}")
    print("inspector stdio: initialize/tools-list/tools-call passed", flush=True)


def inspector_http(npx: Sequence[str], url: str) -> None:
    tools = run_inspector(npx, [
        "--method", "tools/list", "--server-url", url,
        "--transport", "http"])
    if not has_echo_tool(tools):
        raise VerificationError(f"echo tool missing from HTTP tools/list: {tools}")

    call = run_inspector(npx, [
        "--method", "tools/call", "--server-url", url, "--transport", "http",
        "--tool-name", "echo", "--tool-args-json",
        '{"text":"http-amalgamation"}'])
    text = call_text(call)
    if "http-amalgamation" not in text:
        raise VerificationError(
            f"HTTP echo tool did not return the expected text: {call}")
    print("inspector http: tools-list/tools-call passed", flush=True)


def run_conformance(fixture: Path, revision: str, out_dir: Path) -> None:
    out_dir = out_dir.resolve()
    run([
        sys.executable, str(CONFORMANCE_RUNNER),
        "--fixture-server", str(fixture),
        "--revision", revision,
        "--out", str(out_dir),
        "--expected-failures", str(EXPECTED_FAILURES),
    ], timeout=1200)
    print(f"conformance {revision}: passed", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--core-out", type=Path,
        default=ROOT / "build" / "amalgamation" / "tugmcp.hpp")
    parser.add_argument(
        "--http-out", type=Path,
        default=ROOT / "build" / "amalgamation" / "tugmcp_http.hpp")
    parser.add_argument(
        "--core-hash", type=Path,
        default=CONSUMER_PROJECT / "core.sha256")
    parser.add_argument(
        "--http-hash", type=Path,
        default=CONSUMER_PROJECT / "http.sha256")
    parser.add_argument(
        "--json-include", type=Path,
        default=ROOT / "3rd" / "nlohmann_json")
    parser.add_argument(
        "--httplib-include", type=Path,
        default=ROOT / "3rd" / "cpp-httplib")
    parser.add_argument(
        "--build-dir", type=Path,
        default=ROOT / "build" / "amalgamation" / "external-build")
    parser.add_argument(
        "--conformance-out", type=Path,
        default=ROOT / "build" / "amalgamation" / "conformance")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server: Optional[subprocess.Popen] = None
    try:
        npx = npx_command()
        if not (args.json_include / "nlohmann" / "json.hpp").is_file():
            raise VerificationError(
                f"nlohmann/json.hpp not found under {args.json_include}")
        if not (args.httplib_include / "httplib.h").is_file():
            raise VerificationError(
                f"httplib.h not found under {args.httplib_include}")

        generate("core", args.core_out, args.core_hash)
        generate("http", args.http_out, args.http_hash)
        check_http_artifact(args.http_out.resolve())

        build_dir = configure_and_build(args)
        stdio = locate_executable(
            build_dir, "tugmcp_amalgamation_stdio_server")
        http = locate_executable(build_dir, "tugmcp_amalgamation_http_server")
        fixture = locate_executable(
            build_dir, "tugmcp_amalgamation_fixture_server")

        inspector_stdio(npx, stdio)

        http_server, port, _ = start_server(http, ["--http", "--port", "0"])
        server = http_server
        try:
            inspector_http(npx, f"http://127.0.0.1:{port}/mcp")
        finally:
            stop_process(server)
            server = None

        for revision in ("2026-07-28", "2025-11-25"):
            run_conformance(
                fixture, revision, args.conformance_out / revision)
    except (VerificationError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(server)

    print()
    print("result: HTTP single-header Inspector + conformance checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
