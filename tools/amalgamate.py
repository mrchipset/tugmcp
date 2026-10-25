#!/usr/bin/env python3
"""Generate the tugmcp single-header release artifact.

The split headers under ``include/`` and the implementation files under ``src/``
remain the source of truth. This script only concatenates them:

* public headers for the selected profile (dependency-sorted, de-duplicated);
* the profile's internal implementation header;
* the profile's source files, wrapped in ``#ifdef TUGMCP_IMPLEMENTATION``.

Project-local includes are removed after being resolved; system/third-party
includes are hoisted to the top and de-duplicated. The output is deterministic:
there are no timestamps and every list is processed in a stable order.

Usage:
    python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
    python tools/amalgamate.py --profile core --check build/amalgamation/tugmcp.hpp
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_ROOT = ROOT / "include"
PUBLIC_ROOT = INCLUDE_ROOT / "tugmcp"

PROFILES: Dict[str, Dict[str, Sequence[str]]] = {
    "core": {
        "public_headers": [
            "version.hpp",
            "error.hpp",
            "message.hpp",
            "operation.hpp",
            "resource.hpp",
            "prompt.hpp",
            "security.hpp",
            "tool.hpp",
            "request_context.hpp",
            "time_source.hpp",
            "transport.hpp",
            "transport/stdio.hpp",
            "executor.hpp",
            "server.hpp",
            "client.hpp",
            "canonical.hpp",
            "schema.hpp",
            "core.hpp",
        ],
        "internal_headers": ["src/core/dispatcher.hpp"],
        "sources": [
            "src/core/version.cpp",
            "src/core/error.cpp",
            "src/core/message.cpp",
            "src/core/codec.cpp",
            "src/core/canonical.cpp",
            "src/core/registry.cpp",
            "src/core/dispatcher.cpp",
            "src/core/server.cpp",
            "src/core/executor.cpp",
            "src/core/client.cpp",
            "src/core/operation.cpp",
            "src/transport/stdio.cpp",
        ],
    },
}

HEADER_GUARD = "TUGMCP_SINGLE_HEADER_HPP"
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]\s*$")
PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\s*$")
COND_START_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
COND_END_RE = re.compile(r"^\s*#\s*endif\b")


class GenerationError(RuntimeError):
    pass


def normalize_lines(text: str) -> List[str]:
    """Normalize newlines and trailing whitespace for byte-reproducibility."""
    if text.startswith("\ufeff"):
        text = text[1:]
    return [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]


def resolve_project_include(name: str, including: Path) -> Path | None:
    """Return the repo-local file for a project include, else ``None``.

    Quoted includes are always project includes; a quoted include that cannot be
    resolved is an error (the caller reports it). Angle-bracket includes are
    project includes only when they name a ``tugmcp/`` header.
    """
    candidates: List[Path] = []
    if name.startswith("tugmcp/"):
        candidates.append(INCLUDE_ROOT / name)
    else:
        candidates.append(including.parent / name)
        candidates.append(INCLUDE_ROOT / name)
        candidates.append(ROOT / name)

    for candidate in candidates:
        if candidate.is_file():
            resolved = candidate.resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                continue
            return resolved
    return None


def find_matching_endif(lines: Sequence[str], start: int) -> int:
    depth = 0
    for index in range(start, len(lines)):
        line = lines[index]
        if COND_START_RE.match(line):
            depth += 1
        elif COND_END_RE.match(line):
            depth -= 1
            if depth == 0:
                return index
    raise GenerationError(
        f"unterminated preprocessor conditional starting at line {start + 1}"
    )


def is_directive_only(block: Sequence[str]) -> bool:
    continuation = False
    for line in block:
        stripped = line.strip()
        if continuation:
            # Continuation line of a preprocessor directive: it may not start
            # with '#'.
            continuation = stripped.endswith("\\")
            continue
        if not stripped:
            continue
        if stripped.startswith("#"):
            continuation = stripped.endswith("\\")
            continue
        if stripped.startswith("//") or stripped.startswith("/*"):
            continue
        if stripped.startswith("*") or stripped.endswith("*/"):
            continue
        return False
    return True


def trim_blank_edges(lines: List[str]) -> List[str]:
    start = 0
    while start < len(lines) and not lines[start].strip():
        start += 1
    end = len(lines)
    while end > start and not lines[end - 1].strip():
        end -= 1
    return lines[start:end]


class Prelude:
    """Collects hoisted includes while keeping deterministic first-seen order."""

    def __init__(self) -> None:
        self.plain: List[str] = []
        self.blocks: List[List[str]] = []
        self._seen: set[str] = set()

    def add_include(self, line: str) -> None:
        canonical = " ".join(line.split())
        if canonical in self._seen:
            return
        self._seen.add(canonical)
        self.plain.append(canonical)

    def add_block(self, lines: List[str], path: Path) -> None:
        """Add a directive-only conditional block, filtering project includes."""
        kept: List[str] = []
        for line in lines:
            include = INCLUDE_RE.match(line)
            if include:
                name = include.group(2)
                resolved = resolve_project_include(name, path)
                if resolved is not None:
                    raise GenerationError(
                        f"{path}: project include inside a hoisted conditional "
                        f"block is not supported: {line.strip()}"
                    )
                if include.group(1) == '"' or name.startswith("tugmcp/"):
                    raise GenerationError(
                        f"{path}: unresolved project include {line.strip()}"
                    )
                canonical = " ".join(line.split())
                if canonical in self._seen:
                    continue
                self._seen.add(canonical)
            kept.append(line)
        if any(line.strip() for line in kept):
            self.blocks.append(kept)

    def render(self) -> List[str]:
        lines: List[str] = []
        lines.extend(self.plain)
        for block in self.blocks:
            if lines:
                lines.append("")
            lines.extend(block)
        return lines


def process_file(
    path: Path, prelude: Prelude
) -> Tuple[List[str], List[Path]]:
    """Remove project includes/``#pragma once`` and return repo dependencies."""
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    lines = normalize_lines(path.read_text(encoding="utf-8"))
    body: List[str] = []
    deps: List[Path] = []
    depth = 0
    index = 0

    while index < len(lines):
        line = lines[index]

        if PRAGMA_ONCE_RE.match(line):
            index += 1
            continue

        if depth == 0 and COND_START_RE.match(line):
            end = find_matching_endif(lines, index)
            block = lines[index : end + 1]
            if is_directive_only(block) and any(
                INCLUDE_RE.match(line) for line in block
            ):
                prelude.add_block(block, path)
                index = end + 1
                continue
            body.append(line)
            depth += 1
            index += 1
            continue

        include = INCLUDE_RE.match(line)
        if include:
            name = include.group(2)
            resolved = resolve_project_include(name, path)
            if resolved is not None:
                deps.append(resolved)
                index += 1
                continue
            if include.group(1) == '"' or name.startswith("tugmcp/"):
                raise GenerationError(
                    f"{path}: unresolved project include {line.strip()}"
                )
            if depth == 0:
                prelude.add_include(line.strip())
                index += 1
                continue
            raise GenerationError(
                f"{path}: system include inside a conditional block cannot be "
                f"hoisted safely: {line.strip()}"
            )

        if COND_END_RE.match(line) and depth > 0:
            depth -= 1
        body.append(line)
        index += 1

    if depth != 0:
        raise GenerationError(f"{path}: unbalanced preprocessor conditional")
    return trim_blank_edges(body), deps


def public_header_order(profile: Dict[str, Sequence[str]]) -> List[Path]:
    names = list(profile["public_headers"])
    paths = {name: PUBLIC_ROOT / name for name in names}
    for name, path in paths.items():
        if not path.is_file():
            raise GenerationError(f"public header not found: {path}")

    dependencies: Dict[str, List[str]] = {}
    for name, path in paths.items():
        _, deps = process_file(path, Prelude())
        local_deps: List[str] = []
        for dep in deps:
            try:
                relative = dep.relative_to(PUBLIC_ROOT).as_posix()
            except ValueError:
                raise GenerationError(
                    f"public header {path} includes non-public file {dep}"
                ) from None
            if relative not in paths:
                raise GenerationError(
                    f"public header {path} includes {relative}, which is not in "
                    f"the profile header set"
                )
            local_deps.append(relative)
        dependencies[name] = local_deps

    result: List[str] = []
    visiting: set[str] = set()
    done: set[str] = set()

    def visit(name: str) -> None:
        if name in done:
            return
        if name in visiting:
            raise GenerationError(f"public header include cycle at {name}")
        visiting.add(name)
        for dependency in dependencies[name]:
            visit(dependency)
        visiting.remove(name)
        done.add(name)
        result.append(name)

    for name in names:
        visit(name)
    return [paths[name] for name in result]


def read_text(path: Path) -> str:
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    return path.read_text(encoding="utf-8")


def generate(profile_name: str) -> str:
    if profile_name not in PROFILES:
        if profile_name == "http":
            raise GenerationError(
                "--profile http is planned for the H2 release artifact and is "
                "not implemented yet"
            )
        known = ", ".join(sorted(PROFILES))
        raise GenerationError(
            f"unknown profile '{profile_name}' (available: {known})"
        )
    profile = PROFILES[profile_name]
    prelude = Prelude()
    declarations: List[str] = []
    implementation: List[str] = []

    for path in public_header_order(profile):
        body, _ = process_file(path, prelude)
        if body:
            if declarations:
                declarations.append("")
            declarations.extend(body)

    for relative in profile["internal_headers"]:
        path = ROOT / relative
        body, _ = process_file(path, prelude)
        if body:
            if implementation:
                implementation.append("")
            implementation.extend(body)

    for relative in profile["sources"]:
        path = ROOT / relative
        body, _ = process_file(path, prelude)
        if body:
            if implementation:
                implementation.append("")
            implementation.extend(body)

    banner = [
        "// Generated by tools/amalgamate.py; do not edit.",
        "//",
        "// tugmcp single-header release artifact: core + stdio.",
        "//",
        "// This header is a build artifact generated from the split headers in",
        "// include/ and the implementation in src/. The split sources remain the",
        "// source of truth; regenerate instead of editing this file.",
        "//",
        "// Usage:",
        "//   * Define TUGMCP_IMPLEMENTATION in exactly one translation unit before",
        "//     including this header. Include it without the macro everywhere else.",
        "//   * Link Threads::Threads (std::thread, std::mutex, ...).",
        "//   * nlohmann/json is an external dependency: put <nlohmann/json.hpp> on",
        "//     the include path. It is not embedded in this artifact.",
        "//   * This artifact intentionally excludes HTTP/SSE, legacy transports and",
        "//     schema validator backends.",
        "//",
        f"// Profile: {profile_name}",
        "",
        f"#ifndef {HEADER_GUARD}",
        f"#define {HEADER_GUARD}",
        "",
    ]

    output = list(banner)
    rendered_prelude = prelude.render()
    if rendered_prelude:
        output.extend(rendered_prelude)
        output.append("")
    if declarations:
        output.extend(declarations)
        output.append("")
    output.extend(
        [
            "#ifdef TUGMCP_IMPLEMENTATION",
            "",
        ]
    )
    output.extend(implementation)
    output.extend(
        [
            "",
            "#endif // TUGMCP_IMPLEMENTATION",
            "",
            f"#endif // {HEADER_GUARD}",
            "",
        ]
    )
    return "\n".join(output)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the tugmcp single-header release artifact."
    )
    parser.add_argument("--profile", required=True, help="artifact profile")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", type=Path, help="write the header to this path")
    group.add_argument(
        "--check",
        type=Path,
        metavar="PATH",
        help="regenerate and compare byte-for-byte with PATH",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        generated = generate(args.profile)
        payload = generated.encode("utf-8")
    except GenerationError as error:
        print(f"amalgamate: error: {error}", file=sys.stderr)
        return 2

    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(payload)
        print(
            f"amalgamate: wrote {args.out} ({len(payload)} bytes, "
            f"sha256 {sha256(payload)})"
        )
        return 0

    assert args.check is not None
    if not args.check.is_file():
        print(f"amalgamate: error: check target not found: {args.check}",
              file=sys.stderr)
        return 1
    existing = args.check.read_bytes()
    if existing != payload:
        print(
            "amalgamate: error: generated header is not up to date\n"
            f"  existing: {args.check} ({len(existing)} bytes, "
            f"sha256 {sha256(existing)})\n"
            f"  expected: {len(payload)} bytes, sha256 {sha256(payload)}",
            file=sys.stderr,
        )
        return 1
    print(
        f"amalgamate: {args.check} is up to date "
        f"({len(payload)} bytes, sha256 {sha256(payload)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
