#!/usr/bin/env python3
"""Generate and verify the tugmcp single-header release artifact.

The script:
  1. generates the core+stdio header twice and asserts identical SHA256;
  2. verifies the committed/generated file with ``--check``;
  3. configures, builds and runs the standalone two-TU consumer project.

This is intentionally not part of the default ctest suite (H3 may wire it).
Run it directly:

    python tests/amalgamation/run_tests.py
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools" / "amalgamate.py"
CONSUMER_PROJECT = ROOT / "tests" / "amalgamation"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], cwd: Path | None = None) -> None:
    printable = " ".join(str(part) for part in command)
    print(f"+ {printable}", flush=True)
    subprocess.run(command, cwd=cwd or ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "build" / "amalgamation" / "tugmcp.hpp",
        help="generated header path (default: build/amalgamation/tugmcp.hpp)",
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
    json_include = args.json_include.resolve()
    if not (json_include / "nlohmann" / "json.hpp").is_file():
        print(
            f"error: nlohmann/json.hpp not found under {json_include}",
            file=sys.stderr,
        )
        return 2

    with tempfile.TemporaryDirectory(prefix="tugmcp-amalgamation-") as temp_name:
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
            print(
                "error: generator is not deterministic:\n"
                f"  {out}: {first_hash}\n"
                f"  {second}: {second_hash}",
                file=sys.stderr,
            )
            return 1

        # 3. Verify the checked-in/generated artifact via --check.
        run(
            [
                sys.executable,
                str(GENERATOR),
                "--profile",
                args.profile,
                "--check",
                str(out),
            ]
        )

        # 4. Build and run the standalone two-TU consumer.
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

    size = out.stat().st_size
    print()
    print(f"generated: {out}")
    print(f"size:      {size} bytes")
    print(f"sha256:    {first_hash}")
    print("result:    deterministic generation, --check, build and smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
