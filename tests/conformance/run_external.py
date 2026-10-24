#!/usr/bin/env python3
"""Run the official MCP conformance server suite against the fixture server."""

import argparse
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional



REQUIRED_SERVER_SCENARIOS = {
    "2026-07-28": {
        "server-stateless", "completion-complete", "tools-list",
        "tools-call-simple-text", "tools-call-image", "tools-call-audio",
        "tools-call-embedded-resource", "tools-call-mixed-content",
        "tools-call-error", "tools-call-with-progress",
        "server-sse-multiple-streams", "resources-list",
        "resources-read-text", "resources-read-binary",
        "resources-templates-read", "sep-2164-resource-not-found",
        "prompts-list", "prompts-get-simple", "prompts-get-with-args",
        "prompts-get-embedded-resource", "prompts-get-with-image",
        "dns-rebinding-protection", "caching",
        "input-required-result-basic-elicitation",
        "input-required-result-basic-sampling",
        "input-required-result-basic-list-roots",
        "input-required-result-request-state",
        "input-required-result-multiple-input-requests",
        "input-required-result-multi-round",
        "input-required-result-missing-input-response",
        "input-required-result-non-tool-request",
        "input-required-result-result-type",
        "input-required-result-unsupported-methods",
        "input-required-result-tampered-state",
        "input-required-result-capability-check",
        "input-required-result-ignore-extra-params",
        "input-required-result-validate-input",
    },
    "2025-11-25": {
        "server-initialize", "logging-set-level", "ping",
        "completion-complete", "tools-list", "tools-call-simple-text",
        "tools-call-image", "tools-call-audio",
        "tools-call-embedded-resource", "tools-call-mixed-content",
        "tools-call-with-logging", "tools-call-error",
        "tools-call-with-progress", "tools-call-sampling",
        "tools-call-elicitation", "elicitation-sep1034-defaults",
        "server-sse-multiple-streams", "elicitation-sep1330-enums",
        "resources-list", "resources-read-text", "resources-read-binary",
        "resources-templates-read", "resources-subscribe",
        "resources-unsubscribe", "prompts-list", "prompts-get-simple",
        "prompts-get-with-args", "prompts-get-embedded-resource",
        "prompts-get-with-image", "dns-rebinding-protection",
    },
}


def scenario_from_directory(name: str) -> str:
    match = re.match(r"^server-(.*?)-\d{4}-\d{2}-\d{2}T", name)
    return match.group(1) if match else name

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-server", required=True)
    parser.add_argument("--revision", default="2026-07-28")
    parser.add_argument("--package",
                        default="@modelcontextprotocol/conformance@0.2.0-alpha.12")
    parser.add_argument("--out", required=True)
    parser.add_argument("--expected-failures")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    return parser.parse_args()


def read_expected(path: Optional[str]) -> set[str]:
    if not path:
        return set()
    expected: set[str] = set()
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.endswith(":"):
            continue
        if line.startswith("-"):
            value = line[1:].strip().strip("'\"")
            if value:
                expected.add(value)
    return expected


def start_fixture(executable: str):
    process = subprocess.Popen(
        [executable, "--http", "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    lines: queue.Queue[str] = queue.Queue()
    stderr_lines: list[str] = []

    def pump():
        assert process.stderr is not None
        for line in process.stderr:
            stderr_lines.append(line)
            lines.put(line)

    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    port = None
    try:
        while True:
            line = lines.get(timeout=10)
            match = re.search(r"listening on http://[^:]+:(\d+)/mcp", line)
            if match:
                port = int(match.group(1))
                break
    except queue.Empty:
        process.terminate()
        process.wait(timeout=5)
        raise RuntimeError("fixture server did not report a bound port:\n" +
                           "".join(stderr_lines))
    if process.poll() is not None:
        raise RuntimeError("fixture server exited during startup:\n" +
                           "".join(stderr_lines))
    return process, port


def walk_checks(root: Path):
    checks = []
    for path in root.rglob("checks.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as error:  # noqa: BLE001
            raise RuntimeError(f"failed to read {path}: {error}")
        if isinstance(data, list):
            items = data
        elif isinstance(data, dict):
            items = data.get("checks") or data.get("results") or []
        else:
            items = []
        for item in items:
            if isinstance(item, dict):
                checks.append((scenario_from_directory(path.parent.name),
                               item))
    return checks


def main() -> int:
    args = parse_args()
    fixture = str(Path(args.fixture_server).resolve())
    if not Path(fixture).is_file():
        print(f"error: fixture server not found: {fixture}", file=sys.stderr)
        return 2
    if not shutil.which("npx") and not shutil.which("npx.cmd"):
        print("error: npx was not found on PATH", file=sys.stderr)
        return 2

    out_dir = Path(args.out).resolve()
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    expected = read_expected(args.expected_failures)

    process = None
    try:
        process, port = start_fixture(fixture)
        command = [
            "npx", "-y", args.package, "server",
            "--url", f"http://127.0.0.1:{port}/mcp",
            "--requirements", args.revision,
            "--timeout", str(args.timeout_ms),
            "-o", str(out_dir),
        ]
        print("running:", " ".join(command), flush=True)
        npx = shutil.which("npx") or shutil.which("npx.cmd")
        completed = subprocess.run(
            ["cmd", "/c", npx, *command[1:]],
            cwd=str(Path.cwd()),
        )
        if completed.returncode != 0:
            print(f"warning: conformance runner exited {completed.returncode}",
                  file=sys.stderr)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    checks = walk_checks(out_dir)
    required = REQUIRED_SERVER_SCENARIOS.get(args.revision)
    if required is None:
        print(f"error: no required-scenario list for revision {args.revision}",
              file=sys.stderr)
        return 2
    checks = [(scenario, item) for scenario, item in walk_checks(out_dir)
              if scenario in required]
    if not checks:
        print(f"error: no checks.json results found under {out_dir}",
              file=sys.stderr)
        return 2

    failures = []
    unexpected_skips = []
    passed_expected = []
    by_scenario: dict[str, dict[str, int]] = {}
    for scenario, check in checks:
        status = str(check.get("status", "")).upper()
        check_id = str(check.get("id", ""))
        counts = by_scenario.setdefault(
            scenario, {"SUCCESS": 0, "FAILURE": 0, "WARNING": 0,
                       "SKIPPED": 0, "INFO": 0})
        counts[status] = counts.get(status, 0) + 1
        if status == "FAILURE":
            if check_id in expected:
                passed_expected.append(("failure", check_id))
            else:
                failures.append((scenario, check_id,
                                 check.get("errorMessage", "")))
        elif status == "SKIPPED":
            if check_id in expected:
                passed_expected.append(("skipped", check_id))
            else:
                unexpected_skips.append((scenario, check_id,
                                         check.get("errorMessage", "")))
        elif status == "SUCCESS" and check_id in expected:
            passed_expected.append(("unexpected-pass", check_id))

    print("\nPer-scenario summary:", flush=True)
    for scenario in sorted(by_scenario):
        counts = by_scenario[scenario]
        print(f"  {scenario}: " + ", ".join(
            f"{key}={value}" for key, value in counts.items() if value),
            flush=True)

    total = len(checks)
    failed = len(failures) + len(unexpected_skips)
    print(f"\nTotal checks: {total}, failures: {len(failures)}, "
          f"unexpected skipped: {len(unexpected_skips)}, "
          f"expected-failure mismatches: "
          f"{sum(1 for kind, _ in passed_expected if kind == 'unexpected-pass')}",
          flush=True)

    for scenario, check_id, message in failures:
        print(f"FAIL {scenario}/{check_id}: {message}", flush=True)
    for scenario, check_id, message in unexpected_skips:
        print(f"SKIPPED {scenario}/{check_id}: {message}", flush=True)
    for kind, check_id in passed_expected:
        if kind == "unexpected-pass":
            print(f"EXPECTED-FAILURE NOW PASSING: {check_id}", flush=True)

    return 0 if failed == 0 and not any(
        kind == "unexpected-pass" for kind, _ in passed_expected) else 1


if __name__ == "__main__":
    sys.exit(main())
