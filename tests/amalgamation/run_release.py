#!/usr/bin/env python3
"""Verify the packaged single-header release directory.

Checks:
  * the three release headers and their recorded SHA256 values;
  * SHA256SUMS is complete and every listed file matches;
  * the project LICENSE (MIT), bundled third-party licenses, NOTICE and README
    are present and match the source tree;
  * README/NOTICE record the project version (derived from CMakeLists.txt) and
    dependency versions (cpp-httplib derived from httplib.h);
  * guards, TUGMCP_IMPLEMENTATION guidance and link requirements are present;
  * re-running the packager produces byte-identical output (no timestamps).

This test only needs Python 3; it never invokes npx or the network.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parent))
from process_util import run_bounded  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_amalgamation.py"
RECORDED = ROOT / "tests" / "amalgamation"
PROJECT_LICENSE = ROOT / "LICENSE"
CMAKELISTS = ROOT / "CMakeLists.txt"
HTTPLIB_HEADER = ROOT / "3rd" / "cpp-httplib" / "httplib.h"
EXPECTED_HTTPLIB_VERSION = "0.63.0"

HEADERS = (
    ("tugmcp.hpp", "TUGMCP_SINGLE_HEADER_CORE_HPP", "core.sha256"),
    ("tugmcp_http.hpp", "TUGMCP_SINGLE_HEADER_HTTP_HPP", "http.sha256"),
    ("tugmcp_http_tls.hpp", "TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP",
     "http_tls.sha256"),
)
LICENSES = ("nlohmann-json-MIT.txt", "cpp-httplib-MIT.txt")


class ReleaseError(RuntimeError):
    pass


def sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def recorded_hash(name: str) -> str:
    tokens = (RECORDED / name).read_text(encoding="utf-8").split()
    if not tokens:
        raise ReleaseError(f"empty recorded hash file: {name}")
    return tokens[0].lower()


def project_version() -> str:
    text = CMAKELISTS.read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\(\s*tugmcp\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        text,
    )
    if not match:
        raise ReleaseError("project version not found in CMakeLists.txt")
    return match.group(1)


def httplib_version() -> str:
    text = HTTPLIB_HEADER.read_text(encoding="utf-8")
    match = re.search(r'#define\s+CPPHTTPLIB_VERSION\s+"([^"]+)"', text)
    if not match:
        raise ReleaseError("CPPHTTPLIB_VERSION not found in httplib.h")
    if match.group(1) != EXPECTED_HTTPLIB_VERSION:
        raise ReleaseError(
            f"cpp-httplib version {match.group(1)} != packaged "
            f"{EXPECTED_HTTPLIB_VERSION}"
        )
    return match.group(1)


def run(command: List[str], timeout: int = 300) -> None:
    printable = " ".join(str(part) for part in command)
    print(f"+ {printable}", flush=True)
    try:
        completed = run_bounded(command, cwd=ROOT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise ReleaseError(
            f"command timed out after {timeout}s: {printable}"
        ) from error
    if completed.returncode != 0:
        raise ReleaseError(
            f"command failed with exit code {completed.returncode}: {printable}"
        )


def package(release_dir: Path) -> None:
    if release_dir.exists():
        shutil.rmtree(release_dir)
    run([sys.executable, str(PACKAGER), "--release-dir", str(release_dir)])


def parse_sums(release_dir: Path) -> Dict[str, str]:
    path = release_dir / "SHA256SUMS"
    if not path.is_file():
        raise ReleaseError("SHA256SUMS is missing")
    entries: Dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise ReleaseError(f"malformed SHA256SUMS line: {line!r}")
        digest, name = match.groups()
        if name in entries:
            raise ReleaseError(f"duplicate SHA256SUMS entry: {name}")
        entries[name] = digest
    return entries


def verify_required_files(release_dir: Path) -> Dict[str, str]:
    sums = parse_sums(release_dir)
    required = ["LICENSE"]
    required += [name for name, _guard, _hash in HEADERS]
    required += [f"LICENSES/{name}" for name in LICENSES]
    for name in required:
        path = release_dir / name
        if not path.is_file():
            raise ReleaseError(f"required release file is missing: {name}")
        if name not in sums:
            raise ReleaseError(f"{name} is not listed in SHA256SUMS")
        actual = sha256(path.read_bytes())
        if actual != sums[name]:
            raise ReleaseError(
                f"SHA256SUMS mismatch for {name}: "
                f"recorded {sums[name]}, actual {actual}"
            )
    packaged_license = (release_dir / "LICENSE").read_bytes()
    if packaged_license != PROJECT_LICENSE.read_bytes():
        raise ReleaseError("packaged LICENSE differs from the root LICENSE")
    for extra in ("NOTICE", "README.md"):
        if not (release_dir / extra).is_file():
            raise ReleaseError(f"required release file is missing: {extra}")
    return sums


def verify_headers(release_dir: Path, sums: Dict[str, str]) -> None:
    for name, guard, hash_name in HEADERS:
        path = release_dir / name
        text = path.read_text(encoding="utf-8")
        if f"#ifndef {guard}" not in text or f"#define {guard}" not in text:
            raise ReleaseError(f"{name} does not use guard {guard}")
        if "#ifdef TUGMCP_IMPLEMENTATION" not in text:
            raise ReleaseError(f"{name} is missing the implementation section")
        expected = recorded_hash(hash_name)
        if sums[name] != expected or sha256(path.read_bytes()) != expected:
            raise ReleaseError(
                f"{name} does not match tests/amalgamation/{hash_name}: "
                f"expected {expected}, packaged {sums[name]}"
            )


def verify_metadata(release_dir: Path) -> None:
    readme = (release_dir / "README.md").read_text(encoding="utf-8")
    notice = (release_dir / "NOTICE").read_text(encoding="utf-8")
    combined = readme + "\n" + notice

    for token in (
        project_version(),
        "3.12.0",
        httplib_version(),
    ):
        if token not in combined:
            raise ReleaseError(f"release metadata is missing version {token}")

    if "See the project repository" in combined:
        raise ReleaseError(
            "release metadata must point at the bundled LICENSE, not the "
            "project repository"
        )
    for token in ("LICENSE", "MIT License"):
        if token not in combined:
            raise ReleaseError(f"release metadata is missing {token!r}")

    readme_tokens = (
        "TUGMCP_SINGLE_HEADER_CORE_HPP",
        "TUGMCP_SINGLE_HEADER_HTTP_HPP",
        "TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP",
        "TUGMCP_IMPLEMENTATION",
        "Threads::Threads",
        "ws2_32",
        "crypt32",
        "OpenSSL",
        "TUGMCP_TLS_ENABLED",
        "CPPHTTPLIB_OPENSSL_SUPPORT",
        "httplib.h",
        "nlohmann/json.hpp",
        "schema",
        "legacy",
        "excluded",
        "package_amalgamation.py",
        "amalgamate-release",
        "sha256sum -c SHA256SUMS",
        "openssl dgst",
    )
    for token in readme_tokens:
        if token not in readme:
            raise ReleaseError(f"release README is missing {token!r}")

    for license_name in LICENSES:
        source = (ROOT / "LICENSES" / license_name).read_bytes()
        packaged = (release_dir / "LICENSES" / license_name).read_bytes()
        if source != packaged:
            raise ReleaseError(
                f"packaged license differs from LICENSES/{license_name}"
            )


def tree_snapshot(root: Path) -> Dict[str, bytes]:
    snapshot: Dict[str, bytes] = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            snapshot[path.relative_to(root).as_posix()] = path.read_bytes()
    return snapshot


def verify_deterministic(release_dir: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="tugmcp-release-") as temp_name:
        second = Path(temp_name) / "release"
        run([sys.executable, str(PACKAGER), "--release-dir", str(second)])
        first = tree_snapshot(release_dir)
        again = tree_snapshot(second)
        if set(first) != set(again):
            raise ReleaseError(
                "release directory is not reproducible; file sets differ:\n"
                f"  first:  {sorted(first)}\n"
                f"  second: {sorted(again)}"
            )
        for name in first:
            if first[name] != again[name]:
                raise ReleaseError(
                    f"release file is not byte-identical across runs: {name}"
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--release-dir",
        type=Path,
        default=ROOT / "build" / "amalgamation" / "release",
        help="release directory to create and verify",
    )
    args = parser.parse_args()
    release_dir = args.release_dir.resolve()

    try:
        package(release_dir)
        sums = verify_required_files(release_dir)
        verify_headers(release_dir, sums)
        verify_metadata(release_dir)
        verify_deterministic(release_dir)
    except ReleaseError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print()
    print(f"release directory: {release_dir}")
    for name, _guard, _hash in HEADERS:
        size = (release_dir / name).stat().st_size
        print(f"  {name}: {size} bytes, sha256 {sums[name]}")
    print("result: files, hashes, licenses, metadata and deterministic "
          "regeneration verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
