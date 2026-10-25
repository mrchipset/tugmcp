# Single-header amalgamation tests

This directory exercises the generated single headers. The source of truth
remains `include/` plus `src/`; `tools/amalgamate.py` produces the release
artifacts into `build/amalgamation/` (never committed).

## Profiles

- `core`: core + stdio, output `build/amalgamation/tugmcp.hpp`, guard
  `TUGMCP_SINGLE_HEADER_CORE_HPP`.
- `http`: core + stdio + Streamable HTTP/SSE (non-TLS default), output
  `build/amalgamation/tugmcp_http.hpp`, guard
  `TUGMCP_SINGLE_HEADER_HTTP_HPP`. cpp-httplib stays an external include and is
  emitted once inside the implementation section; TLS is a separate H3 variant.

## Core checks

```bash
python tests/amalgamation/run_tests.py
```

The script:

1. generates `build/amalgamation/tugmcp.hpp`;
2. generates it a second time into a temporary directory and asserts identical
   SHA256;
3. verifies the artifact against the recorded SHA in
   `tests/amalgamation/core.sha256` and against its own bytes via
   `tools/amalgamate.py --check`;
4. runs negative tests for unresolved includes, implementation includes outside
   the profile, duplicate anonymous-namespace names and `--check` mismatch;
5. configures, builds and runs the standalone two-TU consumer project and the
   stdio echo server (`consumer_core.cpp` owns `TUGMCP_IMPLEMENTATION`;
   `consumer_core_tu2.cpp` includes the header without it);
6. exercises stdio framing over real OS pipes and the `McpClient` public API.

The same command is registered as the `amalgamation.core` ctest test (label
`amalgamation`, timeout 600) when CMake finds Python 3. It is intentionally not
part of the fast `unit|integration` label budget.

## HTTP external checks

```bash
python tests/amalgamation/run_external.py
```

This script:

1. generates both artifacts and verifies `core.sha256` / `http.sha256`;
2. configures and builds the standalone stdio server, HTTP echo server and the
   conformance fixture server from the generated headers only;
3. runs MCP Inspector 2.12 CLI against the stdio server
   (`initialize`, `tools/list`, `tools/call` with the `echo` tool);
4. starts the HTTP echo server on an ephemeral port and runs Inspector CLI
   `tools/list` and `tools/call` against `http://127.0.0.1:<port>/mcp`;
5. runs the official conformance suite against the single-header fixture server
   for `2026-07-28` and `2025-11-25`, requiring zero required failures.

It is registered as `amalgamation.http` (label `amalgamation`, timeout 1800)
when Python 3, `npx` and `TUGMCP_WITH_HTTP` are available. Node/npx is a hard
requirement: the script fails loudly instead of silently skipping the external
verification.

## Manual generation

```bash
python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
python tools/amalgamate.py --profile http --out build/amalgamation/tugmcp_http.hpp
python tools/amalgamate.py --profile http --check build/amalgamation/tugmcp_http.hpp
```

`--check` automatically uses `tests/amalgamation/<profile>.sha256` when it
exists; pass `--hash-file PATH` to use a different recorded hash. When an
artifact intentionally changes, regenerate the header and update its `.sha256`
file with the new hash (the `--out` command prints it).

Requirements: Python 3.8+, CMake 3.20+, a C++17 compiler, the vendored
nlohmann/json headers under `3rd/nlohmann_json`, and (for the HTTP artifact)
the vendored cpp-httplib under `3rd/cpp-httplib`. On Windows the HTTP consumers
link `ws2_32` and compile with `/bigobj`.
