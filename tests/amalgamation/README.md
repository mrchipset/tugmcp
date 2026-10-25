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
  emitted once inside the implementation section.
- `http --tls`: OpenSSL >= 3.0 variant, output
  `build/amalgamation/tugmcp_http_tls.hpp`, guard
  `TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP`, recorded in `http_tls.sha256`.
  `TUGMCP_TLS_ENABLED=1` and `CPPHTTPLIB_OPENSSL_SUPPORT` are emitted before
  `<httplib.h>` in the implementation prelude. Link OpenSSL SSL/Crypto and, on
  Windows, `ws2_32` plus `crypt32`.

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

## Release packaging checks

```bash
python tests/amalgamation/run_release.py
```

This script generates and packages all three artifacts into
`build/amalgamation/release` through `tools/package_amalgamation.py`, then
verifies:

1. all three headers, `SHA256SUMS`, `NOTICE`, `README.md` and
   `LICENSES/{nlohmann-json-MIT.txt,cpp-httplib-MIT.txt}` exist;
2. every `SHA256SUMS` entry matches and the headers match the recorded
   `core.sha256`/`http.sha256`/`http_tls.sha256` values;
3. guards, `TUGMCP_IMPLEMENTATION`, link requirements and dependency versions
   (`0.1.0`, `3.12.0`, `0.63.0`) are recorded;
4. a second packaging run is byte-identical (no timestamps).

It is registered as the `amalgamation.release` ctest test (label `amalgamation`,
timeout 300) and needs only Python 3, never npx or the network.

## HTTP external checks

```bash
python tests/amalgamation/run_external.py
```

This script:

1. generates both artifacts and verifies `core.sha256` / `http.sha256`;
2. configures and builds the standalone stdio server, HTTP echo server and the
   conformance fixture server from the generated headers only;
3. builds and runs the HTTP two-TU ODR target (`consumer_http_tu2.cpp` is
   compiled without `TUGMCP_IMPLEMENTATION` and without a cpp-httplib include
   path) and the TLS two-TU compile/link/ODR target;
4. runs MCP Inspector 2.12 CLI against the stdio server
   (`initialize`, `tools/list`, `tools/call` with the `echo` tool);
5. starts the HTTP echo server on an ephemeral port and runs Inspector CLI
   `tools/list` and `tools/call` against `http://127.0.0.1:<port>/mcp`;
6. runs the official conformance suite against the single-header fixture server
   for `2026-07-28` and `2025-11-25`, requiring zero required failures and the
   pinned required-check counts (121 and 73).

It is registered as `amalgamation.http` (label `amalgamation`, timeout 1800).
`TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION` defaults to `ON`: the test is registered
even when Python/npx/Node/OpenSSL is missing and fails loudly at test time.
Configure with `-DTUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF` to skip external
verification explicitly.

## Manual generation

```bash
python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
python tools/amalgamate.py --profile http --out build/amalgamation/tugmcp_http.hpp
python tools/amalgamate.py --profile http --tls --out build/amalgamation/tugmcp_http_tls.hpp
python tools/amalgamate.py --profile http --check build/amalgamation/tugmcp_http.hpp
python tools/amalgamate.py --profile http --tls --check build/amalgamation/tugmcp_http_tls.hpp
```

`--check` automatically uses `tests/amalgamation/<profile>.sha256` when it
exists (`http_tls.sha256` for `http --tls`); pass `--hash-file PATH` to use a
different recorded hash. When an artifact intentionally changes, regenerate the
header and update its `.sha256` file with the new hash (the `--out` command
prints it).

Requirements: Python 3.8+, CMake 3.20+, a C++17 compiler, the vendored
nlohmann/json headers under `3rd/nlohmann_json`, and (for the HTTP artifact)
the vendored cpp-httplib under `3rd/cpp-httplib`. On Windows the HTTP consumers
link `ws2_32` and compile with `/bigobj`.
