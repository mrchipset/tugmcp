#!/usr/bin/env python3
"""Generate and verify the tugmcp single-header release artifact.

The script:
  1. generates the core+stdio header twice and asserts identical SHA256;
  2. verifies the artifact against a recorded SHA256 and its own bytes;
  3. runs cheap negative tests for include validation, anonymous-namespace
     collision detection and ``--check`` mismatch;
  4. configures, builds and runs the standalone two-TU consumer project,
     including the stdio framing and McpClient API smoke checks.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools" / "amalgamate.py"
CONSUMER_PROJECT = ROOT / "tests" / "amalgamation"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recorded_hash(path: Path) -> str:
    tokens = path.read_text(encoding="utf-8").split()
    if not tokens:
        raise RuntimeError(f"empty recorded hash file: {path}")
    return tokens[0].lower()


def run(command: list[str], cwd: Path | None = None) -> None:
    printable = " ".join(str(part) for part in command)
    print(f"+ {printable}", flush=True)
    completed = subprocess.run(command, cwd=cwd or ROOT)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {completed.returncode}: {printable}"
        )


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "tugmcp_amalgamate", GENERATOR
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load generator: {GENERATOR}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_generated_structure(generated: Path) -> None:
    text = generated.read_text(encoding="utf-8")
    marker = "#ifdef TUGMCP_IMPLEMENTATION"
    if text.count(marker) != 1:
        raise RuntimeError("generated header has no unique implementation marker")
    prelude, implementation = text.split(marker, 1)
    if "#ifndef TUGMCP_SINGLE_HEADER_CORE_HPP" not in prelude:
        raise RuntimeError("core artifact does not use the core-specific guard")
    # These are unconditional implementation-only includes on every platform.
    for implementation_only_include in ("<system_error>", "<thread>"):
        if implementation_only_include in prelude:
            raise RuntimeError(
                f"implementation include {implementation_only_include} was "
                "hoisted into the declaration prelude"
            )
        if implementation_only_include not in implementation:
            raise RuntimeError(
                f"implementation include {implementation_only_include} is "
                "missing from the implementation section"
            )


def run_negative_tests(temp: Path, generated: Path) -> None:
    module = load_generator()

    missing_source = temp / "missing_include.cpp"
    missing_source.write_text(
        '#include "definitely_missing_project_header.hpp"\n', encoding="utf-8"
    )
    try:
        module.process_implementation_file(missing_source, set(), set())
    except module.GenerationError:
        pass
    else:
        raise RuntimeError("unresolved implementation include was not rejected")

    excluded_source = temp / "excluded_include.cpp"
    excluded_source.write_text(
        '#include "tugmcp/transport/http.hpp"\n', encoding="utf-8"
    )
    core_input_paths, _ = module.profile_input_paths(module.PROFILES["core"])
    try:
        module.process_implementation_file(excluded_source, core_input_paths, set())
    except module.GenerationError:
        pass
    else:
        raise RuntimeError(
            "implementation include outside the profile was not rejected"
        )

    first = temp / "anon_a.cpp"
    second = temp / "anon_b.cpp"
    body = "namespace demo { namespace { int duplicateHelper = 1; } }\n"
    first.write_text(body, encoding="utf-8")
    second.write_text(body, encoding="utf-8")
    try:
        module.check_anonymous_collisions(
            [("anon_a.cpp", first), ("anon_b.cpp", second)]
        )
    except module.GenerationError:
        pass
    else:
        raise RuntimeError("duplicate anonymous-namespace name was not rejected")

    mismatch = temp / "mismatch.hpp"
    shutil.copyfile(generated, mismatch)
    with mismatch.open("ab") as handle:
        handle.write(b"\n// stale\n")
    completed = subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--profile",
            "core",
            "--check",
            str(mismatch),
        ],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if completed.returncode != 1:
        raise RuntimeError("--check did not reject a mismatched artifact")

    print(
        "negative tests: unresolved include, include outside profile, "
        "duplicate anon name, --check mismatch"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "build" / "amalgamation" / "tugmcp.hpp",
        help="generated header path (default: build/amalgamation/tugmcp.hpp)",
    )
    parser.add_argument(
        "--hash-file",
        type=Path,
        default=CONSUMER_PROJECT / "core.sha256",
        help="recorded SHA256 file (default: tests/amalgamation/core.sha256)",
    )
    parser.add_argument(
        "--json-include",
        type=Path,
        default=ROOT / "3rd" / "nlohmann_json",
        help="directory containing nlohmann/json.hpp",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="standalone consumer build directory (default: temporary)",
    )
    parser.add_argument("--profile", default="core", help="artifact profile")
    args = parser.parse_args()

    out = args.out.resolve()
    hash_file = args.hash_file.resolve()
    json_include = args.json_include.resolve()
    if not (json_include / "nlohmann" / "json.hpp").is_file():
        print(
            f"error: nlohmann/json.hpp not found under {json_include}",
            file=sys.stderr,
        )
        return 2
    if not hash_file.is_file():
        print(f"error: recorded hash file not found: {hash_file}", file=sys.stderr)
        return 2

    try:
        with tempfile.TemporaryDirectory(
            prefix="tugmcp-amalgamation-"
        ) as temp_name:
            temp = Path(temp_name)
            second = temp / "tugmcp-second.hpp"
            build_dir = (
                args.build_dir.resolve() if args.build_dir else temp / "build"
            )

            # 1. Generate the publishable artifact.
            run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--profile",
                    args.profile,
                    "--out",
                    str(out),
                ]
            )

            # 2. Determinism: generate again and compare hashes.
            run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--profile",
                    args.profile,
                    "--out",
                    str(second),
                ]
            )
            first_hash = sha256(out)
            second_hash = sha256(second)
            if first_hash != second_hash:
                raise RuntimeError(
                    "generator is not deterministic:\n"
                    f"  {out}: {first_hash}\n"
                    f"  {second}: {second_hash}"
                )
            expected_hash = recorded_hash(hash_file)
            if first_hash != expected_hash:
                raise RuntimeError(
                    "artifact does not match the recorded hash:\n"
                    f"  recorded: {expected_hash}\n"
                    f"  generated: {first_hash}"
                )
            check_generated_structure(out)

            # 3. Verify the artifact against the recorded hash and its bytes.
            run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--profile",
                    args.profile,
                    "--check",
                    str(out),
                    "--hash-file",
                    str(hash_file),
                ]
            )

            # 4. Cheap negative tests.
            run_negative_tests(temp, out)

            # 5. Build and run the standalone two-TU consumer.
            run(
                [
                    "cmake",
                    "-S",
                    str(CONSUMER_PROJECT),
                    "-B",
                    str(build_dir),
                    f"-DTUGMCP_AMALGAMATION_HEADER={out}",
                    f"-DTUGMCP_JSON_INCLUDE={json_include}",
                    "-DCMAKE_BUILD_TYPE=Debug",
                ]
            )
            run(
                [
                    "cmake",
                    "--build",
                    str(build_dir),
                    "--config",
                    "Debug",
                    "--parallel",
                ]
            )
            run(
                [
                    "ctest",
                    "--test-dir",
                    str(build_dir),
                    "-C",
                    "Debug",
                    "--output-on-failure",
                ]
            )
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    size = out.stat().st_size
    print()
    print(f"generated: {out}")
    print(f"size:      {size} bytes")
    print(f"sha256:    {first_hash}")
    print(f"recorded:  {expected_hash} ({hash_file})")
    print("result:    deterministic generation, recorded-hash check, "
          "negative tests, build and smoke tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
