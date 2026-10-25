#!/usr/bin/env python3
"""Generate the tugmcp single-header release artifact.

The split headers under ``include/`` and the implementation files under ``src/``
remain the source of truth. This script only concatenates them:

* public headers for the selected profile (dependency-sorted, de-duplicated);
* the profile's internal implementation headers;
* the profile's source files, wrapped in ``#ifdef TUGMCP_IMPLEMENTATION``.

Includes in declaration-section headers are hoisted/deduplicated. Includes in
implementation-section files are emitted verbatim in place, so HTTP/TLS headers
are never hoisted into the declaration prelude.

The output is deterministic: there are no timestamps and every list is processed
in a stable order.

Usage:
    python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
    python tools/amalgamate.py --profile core --check build/amalgamation/tugmcp.hpp \
        --hash-file tests/amalgamation/core.sha256
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Set, Tuple, TypedDict

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_ROOT = ROOT / "include"
PUBLIC_ROOT = INCLUDE_ROOT / "tugmcp"

# The exact nlohmann/json pin used by the split build (AGENTS.md).
NLOHMANN_JSON_VERSION = "3.12.0"

# Explicit allowlist for quoted third-party includes. Quoted includes that do
# not resolve to a repo file must appear here, otherwise generation fails.
QUOTED_INCLUDE_ALLOWLIST: Dict[str, str] = {
    "nlohmann/json.hpp": "vendored nlohmann/json header",
    "httplib.h": "vendored cpp-httplib header (implementation sections only)",
}

# Repo-relative implementation includes that are intentionally not part of a
# profile's input set. Keep this empty unless a generated (non-merged) header is
# deliberately consumed by an implementation file.
IMPL_INCLUDE_IGNORE: Dict[str, str] = {}

class Profile(TypedDict):
    guard: str
    display_name: str
    public_headers: List[str]
    internal_headers: List[str]
    sources: List[str]
    exclusions: Dict[str, str]
    implementation_prelude: List[str]
    banner_notes: List[str]


SCHEMA_EXCLUSIONS: Dict[str, str] = {
    "include/tugmcp/schema/jsoncons.hpp":
        "optional jsoncons schema validator; not part of single-header artifacts",
    "include/tugmcp/schema/null.hpp":
        "test-only schema validator; not part of single-header artifacts",
    "include/tugmcp/schema/valijson.hpp":
        "optional Valijson schema validator; not part of single-header artifacts",
    "src/schema/jsoncons_validator.cpp":
        "optional jsoncons schema validator; not part of single-header artifacts",
    "src/schema/valijson_validator.cpp":
        "optional Valijson schema validator; not part of single-header artifacts",
}

LEGACY_EXCLUSIONS: Dict[str, str] = {
    "include/tugmcp/transport/legacy_sse.hpp":
        "legacy HTTP+SSE transport; excluded from single-header artifacts",
    "src/transport/legacy_sse.cpp":
        "legacy HTTP+SSE transport; excluded from single-header artifacts",
}

HTTP_EXCLUSIONS: Dict[str, str] = {
    "include/tugmcp/detail/http_httplib.hpp":
        "HTTP implementation header; belongs to the H2 HTTP artifact",
    "include/tugmcp/transport/http.hpp":
        "Streamable HTTP transport; belongs to the H2 HTTP artifact",
    "src/transport/http_security.hpp":
        "HTTP security helpers; belong to the H2 HTTP artifact",
    "src/transport/http_server.cpp":
        "Streamable HTTP server; belongs to the H2 HTTP artifact",
    "src/transport/http_client.cpp":
        "Streamable HTTP client; belongs to the H2 HTTP artifact",
}

# Emitted before detail/http_httplib.hpp and <httplib.h>. Keeping these in the
# implementation prelude guarantees they precede the third-party include.
PLATFORM_DEFINES: List[str] = [
    "// Platform feature macros must precede <httplib.h> and the OS headers.",
    "#ifndef NOMINMAX",
    "#define NOMINMAX",
    "#endif",
    "#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)",
    "#define WIN32_LEAN_AND_MEAN",
    "#endif",
]

HTTP_NONTLS_PRELUDE: List[str] = [
    *PLATFORM_DEFINES,
    "// Non-TLS default for the HTTP artifact.",
    "#ifndef TUGMCP_TLS_ENABLED",
    "#define TUGMCP_TLS_ENABLED 0",
    "#endif",
]

HTTP_TLS_PRELUDE: List[str] = [
    *PLATFORM_DEFINES,
    "// TLS variant: OpenSSL >= 3.0 is required at compile and link time.",
    "#ifndef TUGMCP_TLS_ENABLED",
    "#define TUGMCP_TLS_ENABLED 1",
    "#endif",
    "// Must be defined before <httplib.h>; detail/http_httplib.hpp relies on it.",
    "#ifndef CPPHTTPLIB_OPENSSL_SUPPORT",
    "#define CPPHTTPLIB_OPENSSL_SUPPORT",
    "#endif",
]

EXCLUSIONS: Dict[str, Dict[str, str]] = {
    "core": {
        **HTTP_EXCLUSIONS,
        **SCHEMA_EXCLUSIONS,
        **LEGACY_EXCLUSIONS,
    },
    "http": {
        **SCHEMA_EXCLUSIONS,
        **LEGACY_EXCLUSIONS,
    },
}

PROFILES: Dict[str, Profile] = {
    "core": {
        "guard": "TUGMCP_SINGLE_HEADER_CORE_HPP",
        "display_name": "core + stdio",
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
        "exclusions": EXCLUSIONS["core"],
        "implementation_prelude": [],
        "banner_notes": [
            "//   * This artifact intentionally excludes HTTP/SSE, legacy transports and",
            "//     schema validator backends.",
        ],
    },
    "http": {
        "guard": "TUGMCP_SINGLE_HEADER_HTTP_HPP",
        "display_name": "core + stdio + Streamable HTTP/SSE",
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
            "transport/http.hpp",
            "executor.hpp",
            "server.hpp",
            "client.hpp",
            "canonical.hpp",
            "schema.hpp",
            "core.hpp",
        ],
        # detail/http_httplib.hpp is emitted once at the start of the
        # implementation section (never hoisted into the declaration prelude).
        "internal_headers": [
            "include/tugmcp/detail/http_httplib.hpp",
            "src/transport/http_security.hpp",
            "src/core/dispatcher.hpp",
        ],
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
            "src/transport/http_server.cpp",
            "src/transport/http_client.cpp",
        ],
        "exclusions": EXCLUSIONS["http"],
        "implementation_prelude": HTTP_NONTLS_PRELUDE,
        "banner_notes": [
            "//   * Link Threads::Threads and, on Windows, ws2_32. cpp-httplib is an",
            "//     external dependency: put <httplib.h> on the include path.",
            "//   * TUGMCP_TLS_ENABLED is 0 in this artifact; use --tls for the",
            "//     OpenSSL >= 3.0 variant (also link crypt32 on Windows).",
            "//   * This artifact intentionally excludes legacy HTTP+SSE and schema",
            "//     validator backends.",
        ],
    },
}

HTTP_TLS_GUARD = "TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP"

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]\s*$")
PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\s*$")
COND_START_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
COND_END_RE = re.compile(r"^\s*#\s*endif\b")
ANON_NAMESPACE_RE = re.compile(r"\bnamespace\s*\{")
CONTROL_KEYWORDS = {
    "alignof",
    "catch",
    "const_cast",
    "decltype",
    "delete",
    "dynamic_cast",
    "for",
    "if",
    "new",
    "noexcept",
    "operator",
    "reinterpret_cast",
    "return",
    "sizeof",
    "static_cast",
    "switch",
    "throw",
    "typeid",
    "while",
}


class GenerationError(RuntimeError):
    pass


def repo_relative(path: Path) -> str:
    return path.resolve().relative_to(ROOT).as_posix()


def normalize_lines(text: str) -> List[str]:
    """Normalize newlines and trailing whitespace for byte-reproducibility."""
    if text.startswith("\ufeff"):
        text = text[1:]
    return [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]


def normalized_file_sha256(path: Path) -> str:
    lines = normalize_lines(path.read_text(encoding="utf-8"))
    payload = "\n".join(lines).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def provenance_marker(path: Path) -> List[str]:
    return [
        "// ===========================================================================",
        f"// merged from: {repo_relative(path)}",
        f"// normalized sha256: {normalized_file_sha256(path)}",
        "// ===========================================================================",
    ]


def resolve_project_include(name: str, including: Path) -> Path | None:
    """Return the repo-local file for a project include, else ``None``."""
    candidates: List[Path] = []
    if name.startswith("tugmcp/"):
        candidates.append(INCLUDE_ROOT / name)
    else:
        candidates.append(including.parent / name)
        candidates.append(INCLUDE_ROOT / name)
        candidates.append(ROOT / name)

    matches: List[Path] = []
    for candidate in candidates:
        if candidate.is_file():
            resolved = candidate.resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                continue
            if resolved not in matches:
                matches.append(resolved)
    if len(matches) > 1:
        raise GenerationError(
            f"ambiguous project include {name!r} in {including}: "
            + ", ".join(repo_relative(path) for path in matches)
        )
    return matches[0] if matches else None


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
    """Collects hoisted declaration includes in deterministic first-seen order."""

    def __init__(self) -> None:
        self.plain: List[str] = []
        self.blocks: List[List[str]] = []
        self._seen: Set[str] = set()

    def add_include(self, line: str) -> None:
        canonical = " ".join(line.split())
        if canonical in self._seen:
            return
        self._seen.add(canonical)
        self.plain.append(canonical)

    def add_block(self, lines: List[str], path: Path) -> None:
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
                if include.group(1) == '"':
                    if name not in QUOTED_INCLUDE_ALLOWLIST:
                        raise GenerationError(
                            f"{path}: unresolved quoted include {line.strip()}; "
                            "add it to QUOTED_INCLUDE_ALLOWLIST if it is external"
                        )
                elif name.startswith("tugmcp/"):
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


def process_declaration_file(
    path: Path, prelude: Prelude
) -> Tuple[List[str], List[Path]]:
    """Strip project includes/``#pragma once`` and hoist external includes."""
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
            if include.group(1) == '"':
                if name not in QUOTED_INCLUDE_ALLOWLIST:
                    raise GenerationError(
                        f"{path}: unresolved quoted include {line.strip()}; "
                        "add it to QUOTED_INCLUDE_ALLOWLIST if it is external"
                    )
                prelude.add_include(line.strip())
                index += 1
                continue
            if name.startswith("tugmcp/"):
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


def process_implementation_file(
    path: Path,
    input_paths: Set[Path],
    ignored_relative_paths: Set[str],
) -> Tuple[List[str], List[Path]]:
    """Validate implementation includes and keep external includes in place."""
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    lines = normalize_lines(path.read_text(encoding="utf-8"))
    body: List[str] = []
    deps: List[Path] = []

    for line in lines:
        if PRAGMA_ONCE_RE.match(line):
            continue

        include = INCLUDE_RE.match(line)
        if include:
            name = include.group(2)
            resolved = resolve_project_include(name, path)
            if resolved is not None:
                relative = repo_relative(resolved)
                if resolved not in input_paths and relative not in ignored_relative_paths:
                    raise GenerationError(
                        f"{path}: implementation include {line.strip()} resolves "
                        f"to {relative}, which is not a profile input and is not "
                        "in IMPL_INCLUDE_IGNORE"
                    )
                deps.append(resolved)
                continue

            if include.group(1) == '"':
                if name not in QUOTED_INCLUDE_ALLOWLIST:
                    raise GenerationError(
                        f"{path}: unresolved quoted include {line.strip()}; "
                        "add it to QUOTED_INCLUDE_ALLOWLIST if it is external"
                    )
                body.append(line)
                continue
            if name.startswith("tugmcp/"):
                raise GenerationError(
                    f"{path}: unresolved project include {line.strip()}"
                )
            # System/third-party angle include: emit verbatim in place.
            body.append(line)
            continue

        body.append(line)

    return trim_blank_edges(body), deps


def profile_input_paths(profile: Profile) -> Tuple[Set[Path], Set[str]]:
    paths: Set[Path] = set()
    for name in profile["public_headers"]:
        paths.add((PUBLIC_ROOT / name).resolve())
    for relative in list(profile["internal_headers"]) + list(profile["sources"]):
        paths.add((ROOT / relative).resolve())
    relative_paths = {repo_relative(path) for path in paths}
    return paths, relative_paths


def validate_no_duplicate_inputs(profile_name: str, profile: Profile) -> None:
    seen: Dict[str, str] = {}
    duplicates: List[str] = []
    groups = (
        ("public header", PUBLIC_ROOT, profile["public_headers"]),
        ("internal header", ROOT, profile["internal_headers"]),
        ("source", ROOT, profile["sources"]),
    )
    for group_name, base, entries in groups:
        for entry in entries:
            path = (base / entry).resolve()
            try:
                key = repo_relative(path)
            except ValueError:
                raise GenerationError(
                    f"profile {profile_name}: input escapes the repository: {entry}"
                ) from None
            if key in seen:
                duplicates.append(f"{key} ({seen[key]} and {group_name})")
            else:
                seen[key] = group_name
    if duplicates:
        raise GenerationError(
            f"profile {profile_name}: duplicate inputs: " + "; ".join(duplicates)
        )


def validate_profile_completeness(
    profile_name: str, profile: Profile
) -> None:
    exclusions: Dict[str, str] = dict(profile.get("exclusions", {}))
    for relative, reason in exclusions.items():
        if not reason:
            raise GenerationError(
                f"profile {profile_name}: exclusion {relative} has no reason"
            )

    expected_headers = {
        repo_relative(path) for path in PUBLIC_ROOT.rglob("*.hpp")
    }
    expected_sources = {
        repo_relative(path) for path in (ROOT / "src").rglob("*.cpp")
    }
    expected_internal = {
        repo_relative(path) for path in (ROOT / "src").rglob("*.hpp")
    }
    expected_all = expected_headers | expected_sources | expected_internal

    unknown_exclusions = set(exclusions) - expected_all
    if unknown_exclusions:
        raise GenerationError(
            f"profile {profile_name}: exclusions reference unknown files: "
            f"{sorted(unknown_exclusions)}"
        )

    profile_headers = {
        f"include/tugmcp/{name}" for name in profile["public_headers"]
    }
    missing_header_inputs = profile_headers - expected_headers
    if missing_header_inputs:
        raise GenerationError(
            f"profile {profile_name}: public header inputs are missing: "
            f"{sorted(missing_header_inputs)}"
        )

    merged_headers = profile_headers | set(profile["internal_headers"])
    missing_headers = expected_headers - merged_headers - set(exclusions)
    if missing_headers:
        raise GenerationError(
            f"profile {profile_name}: public headers are neither merged nor "
            f"excluded: {sorted(missing_headers)}"
        )

    missing_sources = (
        expected_sources - set(profile["sources"]) - set(exclusions)
    )
    if missing_sources:
        raise GenerationError(
            f"profile {profile_name}: sources are neither merged nor excluded: "
            f"{sorted(missing_sources)}"
        )

    missing_internal = (
        expected_internal - set(profile["internal_headers"]) - set(exclusions)
    )
    if missing_internal:
        raise GenerationError(
            f"profile {profile_name}: internal headers are neither merged nor "
            f"excluded: {sorted(missing_internal)}"
        )

    for relative in list(profile["internal_headers"]) + list(profile["sources"]):
        if not (ROOT / relative).is_file():
            raise GenerationError(f"profile input not found: {relative}")


def public_header_order(profile: Profile) -> List[Path]:
    names = list(profile["public_headers"])
    paths = {name: PUBLIC_ROOT / name for name in names}
    for name, path in paths.items():
        if not path.is_file():
            raise GenerationError(f"public header not found: {path}")

    dependencies: Dict[str, List[str]] = {}
    for name, path in paths.items():
        _, deps = process_declaration_file(path, Prelude())
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
    visiting: Set[str] = set()
    done: Set[str] = set()

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


def mask_cpp(text: str) -> str:
    """Blank comments and string/char literal contents, preserving newlines."""
    chars = list(text)
    index = 0
    size = len(text)
    state = "normal"
    while index < size:
        char = text[index]
        if state == "normal":
            if char == "R" and index + 1 < size and text[index + 1] == '"':
                # Raw string literal: R"delim( ... )delim". Blank the whole
                # literal (preserving newlines) so quotes/parens inside it do
                # not confuse the declaration scanner.
                delimiter_start = index + 2
                open_paren = text.find("(", delimiter_start)
                if open_paren != -1:
                    delimiter = text[delimiter_start:open_paren]
                    terminator = ")" + delimiter + '"'
                    end = text.find(terminator, open_paren + 1)
                    stop = (end + len(terminator)) if end != -1 else size
                    for raw_index in range(index, stop):
                        if chars[raw_index] != "\n":
                            chars[raw_index] = " "
                    index = stop
                    continue
            if char == "/" and index + 1 < size and text[index + 1] == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "line"
                continue
            if char == "/" and index + 1 < size and text[index + 1] == "*":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "block"
                continue
            if char == '"':
                chars[index] = " "
                index += 1
                state = "string"
                continue
            if char == "'":
                chars[index] = " "
                index += 1
                state = "char"
                continue
            index += 1
            continue

        if state == "line":
            if char == "\n":
                state = "normal"
            else:
                chars[index] = " "
            index += 1
            continue

        if state == "block":
            if char == "*" and index + 1 < size and text[index + 1] == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "normal"
                continue
            if char != "\n":
                chars[index] = " "
            index += 1
            continue

        # string/char literal
        quote = '"' if state == "string" else "'"
        if char == "\\":
            chars[index] = " "
            if index + 1 < size and text[index + 1] != "\n":
                chars[index + 1] = " "
            index += 2
            continue
        if char == quote:
            chars[index] = " "
            index += 1
            state = "normal"
            continue
        if char != "\n":
            chars[index] = " "
        index += 1

    return "".join(chars)


def anonymous_top_level_text(text: str) -> str:
    """Return only declarations directly inside anonymous namespaces."""
    masked = mask_cpp(text)
    anon_opens = {match.end() - 1 for match in ANON_NAMESPACE_RE.finditer(masked)}
    output: List[str] = []
    stack: List[bool] = []
    for index, char in enumerate(masked):
        keep = bool(stack) and stack[-1]
        if char == "{":
            if keep:
                # A function/class/initializer body inside the anonymous
                # namespace: keep the preceding declaration, drop the body.
                output.append(";")
            else:
                output.append(" ")
            stack.append(index in anon_opens)
            continue
        if char == "}":
            if stack:
                stack.pop()
            output.append(" ")
            continue
        if char == "\n":
            output.append("\n")
            continue
        output.append(char if keep else " ")
    return "".join(output)


def strip_preprocessor_lines(text: str) -> str:
    """Blank preprocessor directive lines so they do not hide declarations."""
    output: List[str] = []
    continuation = False
    for line in text.split("\n"):
        stripped = line.lstrip()
        if continuation or stripped.startswith("#"):
            continuation = line.rstrip().endswith("\\")
            output.append("")
            continue
        output.append(line)
    return "\n".join(output)


def scan_anonymous_names(path: Path) -> Set[str]:
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    extracted = strip_preprocessor_lines(anonymous_top_level_text(text))
    names: Set[str] = set()

    for statement in extracted.split(";"):
        statement = " ".join(statement.split())
        if not statement or statement.startswith("#"):
            continue

        type_match = re.match(
            r"^(?:template\s*<[^>]*>\s*)?"
            r"(?:struct|class|union|enum(?:\s+class)?)\s+([A-Za-z_]\w*)",
            statement,
        )
        if type_match:
            names.add(type_match.group(1))
            continue

        using_match = re.match(
            r"^(?:template\s*<[^>]*>\s*)?using\s+([A-Za-z_]\w*)\s*=",
            statement,
        )
        if using_match:
            names.add(using_match.group(1))
            continue

        typedef_match = re.match(r"^typedef\b", statement)
        if typedef_match:
            match = re.search(r"([A-Za-z_]\w*)\s*$", statement)
            if match:
                names.add(match.group(1))
            continue

        first_paren = statement.find("(")
        first_equal = statement.find("=")
        if first_paren != -1 and (first_equal == -1 or first_paren < first_equal):
            match = re.search(
                r"([A-Za-z_]\w*)\s*\(", statement[: first_paren + 1]
            )
            if match and match.group(1) not in CONTROL_KEYWORDS:
                names.add(match.group(1))
            continue

        match = re.search(r"([A-Za-z_]\w*)\s*=", statement)
        if match:
            names.add(match.group(1))
            continue

        match = re.search(r"([A-Za-z_]\w*)\s*$", statement)
        if match and match.group(1) not in CONTROL_KEYWORDS:
            names.add(match.group(1))

    return names


def _brace_contexts(masked: str) -> Dict[int, bool]:
    """Map ``{`` positions to True when the brace opens a namespace."""
    contexts: Dict[int, bool] = {}
    for match in re.finditer(r"\{", masked):
        index = match.start()
        segment_start = max(
            masked.rfind(";", 0, index),
            masked.rfind("{", 0, index),
            masked.rfind("}", 0, index),
        )
        prefix = masked[segment_start + 1:index]
        contexts[index] = bool(re.search(r"\bnamespace\b", prefix))
    return contexts


def scan_file_scope_static_names(path: Path) -> Set[str]:
    """Return namespace/file-scope ``static`` declaration names.

    Class-scope and function-local statics are skipped: merging independent
    source files only creates collisions at namespace scope.
    """
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    masked = strip_preprocessor_lines(mask_cpp(text))
    contexts = _brace_contexts(masked)
    names: Set[str] = set()

    for match in re.finditer(r"\bstatic\b", masked):
        position = match.end()
        stack: List[bool] = []
        for index in range(position):
            if masked[index] == "{":
                stack.append(contexts.get(index, False))
            elif masked[index] == "}" and stack:
                stack.pop()
        if any(not namespace for namespace in stack):
            continue

        tail = masked[position:]
        semicolon = tail.find(";")
        brace = tail.find("{")
        limit_candidates = [value for value in (semicolon, brace) if value != -1]
        limit = min(limit_candidates) if limit_candidates else len(tail)
        declaration = tail[:limit]
        first_paren = declaration.find("(")
        assignment = declaration.find("=")
        if first_paren != -1 and (assignment == -1 or first_paren < assignment):
            name_match = re.search(
                r"([A-Za-z_]\w*)\s*$", declaration[:first_paren]
            )
        else:
            for stop_char in ("=", "["):
                stop = declaration.find(stop_char)
                if stop != -1:
                    declaration = declaration[:stop]
            name_match = re.search(r"([A-Za-z_]\w*)\s*$", declaration)
        if name_match and name_match.group(1) != "static":
            names.add(name_match.group(1))
    return names


def scan_macro_definitions(path: Path) -> Dict[str, str]:
    """Return ``#define NAME`` -> normalized replacement text."""
    if not path.is_file():
        raise GenerationError(f"input file not found: {path}")
    lines = normalize_lines(path.read_text(encoding="utf-8"))
    macros: Dict[str, str] = {}
    index = 0
    while index < len(lines):
        line = lines[index]
        match = re.match(
            r"^\s*#\s*define\s+([A-Za-z_]\w*)(.*)$", line
        )
        if not match:
            index += 1
            continue
        name = match.group(1)
        body = [match.group(2)]
        while line.rstrip().endswith("\\") and index + 1 < len(lines):
            index += 1
            line = lines[index]
            body.append(line)
        macros[name] = " ".join(part.rstrip("\\").strip() for part in body)
        index += 1
    return macros


def check_anonymous_collisions(entries: Sequence[Tuple[str, Path]]) -> None:
    anonymous_owners: Dict[str, Set[str]] = {}
    static_owners: Dict[str, Set[str]] = {}
    macro_owners: Dict[str, Dict[str, Set[str]]] = {}
    for label, path in entries:
        for name in scan_anonymous_names(path):
            anonymous_owners.setdefault(name, set()).add(label)
        for name in scan_file_scope_static_names(path):
            static_owners.setdefault(name, set()).add(label)
        for name, body in scan_macro_definitions(path).items():
            macro_owners.setdefault(name, {}).setdefault(body, set()).add(label)

    collisions: List[str] = []
    for kind, owners in (
        ("anonymous-namespace", anonymous_owners),
        ("file-scope static", static_owners),
    ):
        for name, labels in sorted(owners.items()):
            if len(labels) > 1:
                collisions.append(f"{kind} {name} in {', '.join(sorted(labels))}")
    for name, bodies in sorted(macro_owners.items()):
        if len(bodies) > 1:
            labels = sorted(
                label for owners in bodies.values() for label in owners
            )
            collisions.append(
                f"macro {name} with conflicting definitions in "
                + ", ".join(labels)
            )
    if collisions:
        raise GenerationError(
            "merged name collision across files: " + "; ".join(collisions)
        )


def resolve_profile(profile_name: str, tls: bool) -> Profile:
    if profile_name not in PROFILES:
        known = ", ".join(sorted(PROFILES))
        raise GenerationError(
            f"unknown profile '{profile_name}' (available: {known})"
        )
    if tls and profile_name != "http":
        raise GenerationError("--tls is only supported with --profile http")

    profile = dict(PROFILES[profile_name])
    if tls:
        profile["guard"] = HTTP_TLS_GUARD
        profile["display_name"] = "core + stdio + Streamable HTTP/SSE (TLS)"
        profile["implementation_prelude"] = HTTP_TLS_PRELUDE
        profile["banner_notes"] = [
            "//   * Link Threads::Threads, OpenSSL >= 3.0 (SSL/Crypto) and, on",
            "//     Windows, ws2_32 plus crypt32. cpp-httplib is external.",
            "//   * TUGMCP_TLS_ENABLED is 1 and CPPHTTPLIB_OPENSSL_SUPPORT is",
            "//     defined before <httplib.h>. All TUs must use this variant.",
            "//   * This artifact intentionally excludes legacy HTTP+SSE and schema",
            "//     validator backends.",
        ]
    return profile  # type: ignore[return-value]


def artifact_key(profile_name: str, tls: bool) -> str:
    return "http_tls" if profile_name == "http" and tls else profile_name


def generate(profile_name: str, tls: bool = False) -> str:
    profile = resolve_profile(profile_name, tls)
    validate_profile_completeness(profile_name, profile)
    validate_no_duplicate_inputs(profile_name, profile)
    input_paths, input_relative_paths = profile_input_paths(profile)

    collision_entries: List[Tuple[str, Path]] = []
    for relative in list(profile["internal_headers"]) + list(profile["sources"]):
        collision_entries.append((relative, ROOT / relative))
    check_anonymous_collisions(collision_entries)

    prelude = Prelude()
    declarations: List[str] = []
    implementation: List[str] = []

    for path in public_header_order(profile):
        body, _ = process_declaration_file(path, prelude)
        if body:
            if declarations:
                declarations.append("")
            declarations.extend(provenance_marker(path))
            declarations.extend(body)

    for relative in profile["internal_headers"]:
        path = ROOT / relative
        body, _ = process_implementation_file(
            path, input_paths, set(IMPL_INCLUDE_IGNORE)
        )
        if body:
            if implementation:
                implementation.append("")
            implementation.extend(provenance_marker(path))
            implementation.extend(body)

    for relative in profile["sources"]:
        path = ROOT / relative
        body, _ = process_implementation_file(
            path, input_paths, set(IMPL_INCLUDE_IGNORE)
        )
        if body:
            if implementation:
                implementation.append("")
            implementation.extend(provenance_marker(path))
            implementation.extend(body)

    guard = str(profile["guard"])
    display_name = str(profile["display_name"])
    banner = [
        "// Generated by tools/amalgamate.py; do not edit.",
        "//",
        f"// tugmcp single-header release artifact: {display_name}.",
        "//",
        "// This header is a build artifact generated from the split headers in",
        "// include/ and the implementation in src/. The split sources remain the",
        "// source of truth; regenerate instead of editing this file.",
        "//",
        "// Usage:",
        "//   * Define TUGMCP_IMPLEMENTATION in exactly one translation unit before",
        "//     including this header. Include it without the macro everywhere else.",
        "//   * Link Threads::Threads (std::thread, std::mutex, ...).",
        f"//   * nlohmann/json is an external dependency pinned at "
        f"{NLOHMANN_JSON_VERSION}; put",
        "//     <nlohmann/json.hpp> on the include path. It is not embedded.",
        *profile["banner_notes"],
        "//",
        f"// Guard: {guard}",
        f"// Profile: {profile_name}{' (TLS)' if tls else ''}",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
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
    implementation_prelude = list(profile["implementation_prelude"])
    if implementation_prelude:
        output.extend(implementation_prelude)
        output.append("")
    output.extend(implementation)
    output.extend(
        [
            "",
            "#endif // TUGMCP_IMPLEMENTATION",
            "",
            f"#endif // {guard}",
            "",
        ]
    )
    return "\n".join(output)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_recorded_hash(path: Path) -> str:
    if not path.is_file():
        raise GenerationError(f"recorded hash file not found: {path}")
    first = path.read_text(encoding="utf-8").split()
    if not first or not re.fullmatch(r"[0-9a-fA-F]{64}", first[0]):
        raise GenerationError(f"recorded hash file is malformed: {path}")
    return first[0].lower()


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temp_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the tugmcp single-header release artifact."
    )
    parser.add_argument("--profile", required=True, help="artifact profile")
    parser.add_argument(
        "--tls",
        action="store_true",
        help="generate the TLS HTTP variant (requires --profile http)",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", type=Path, help="write the header to this path")
    group.add_argument(
        "--check",
        type=Path,
        metavar="PATH",
        help="regenerate and compare byte-for-byte with PATH",
    )
    parser.add_argument(
        "--hash-file",
        type=Path,
        default=None,
        help=(
            "recorded SHA256 file; requires --check and is verified "
            "independently (auto-detected at tests/amalgamation/<profile>.sha256)"
        ),
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.hash_file is not None and args.check is None:
        parser.error("--hash-file requires --check")
    if args.tls and args.profile != "http":
        parser.error("--tls is only supported with --profile http")

    hash_file = args.hash_file
    if hash_file is None and args.check is not None:
        key = artifact_key(args.profile, args.tls)
        candidate = ROOT / "tests" / "amalgamation" / f"{key}.sha256"
        if candidate.is_file():
            hash_file = candidate

    try:
        generated = generate(args.profile, args.tls)
        payload = generated.encode("utf-8")
        generated_hash = sha256(payload)
        expected_hash = (
            read_recorded_hash(hash_file)
            if hash_file is not None
            else None
        )
    except GenerationError as error:
        print(f"amalgamate: error: {error}", file=sys.stderr)
        return 2

    if args.out is not None:
        atomic_write(args.out, payload)
        print(
            f"amalgamate: wrote {args.out} ({len(payload)} bytes, "
            f"sha256 {generated_hash})"
        )
        return 0

    assert args.check is not None
    if expected_hash is not None and expected_hash != generated_hash:
        print(
            "amalgamate: error: generated hash does not match the recorded hash\n"
            f"  recorded: {expected_hash}\n"
            f"  generated: {generated_hash}",
            file=sys.stderr,
        )
        return 1
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
            f"  expected: {len(payload)} bytes, sha256 {generated_hash}",
            file=sys.stderr,
        )
        return 1
    print(
        f"amalgamate: {args.check} is up to date "
        f"({len(payload)} bytes, sha256 {generated_hash})"
    )
    if expected_hash is not None:
        print(f"amalgamate: recorded hash matches ({expected_hash})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
