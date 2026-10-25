# Single-header amalgamation tests

This directory exercises the generated **core + stdio** single header. The
source of truth remains `include/` plus `src/`; `tools/amalgamate.py` produces
the release artifact into `build/amalgamation/` (never committed).

## Run directly

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
5. configures, builds and runs the standalone two-TU consumer project
   (`consumer_core.cpp` owns `TUGMCP_IMPLEMENTATION`; `consumer_core_tu2.cpp`
   includes the header without it);
6. exercises stdio framing over real OS pipes and the `McpClient` public API.

The same command is registered as the `amalgamation.core` ctest test (label
`amalgamation`, timeout 600) when CMake finds Python 3. It is intentionally not
part of the fast `unit|integration` label budget.

Requirements: Python 3.8+, CMake 3.20+, a C++17 compiler, and the vendored
nlohmann/json headers under `3rd/nlohmann_json`.

## Manual generation

```bash
python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
python tools/amalgamate.py --profile core --check build/amalgamation/tugmcp.hpp
```

`--check` automatically uses `tests/amalgamation/<profile>.sha256` when it
exists; pass `--hash-file PATH` to use a different recorded hash. When the
artifact intentionally changes, regenerate the header and update
`core.sha256` with the new hash (the `--out` command prints it).

The generated header uses the profile-specific guard
`TUGMCP_SINGLE_HEADER_CORE_HPP`, requires `TUGMCP_IMPLEMENTATION` in exactly one
translation unit, and must be linked with `Threads::Threads`. nlohmann/json is
pinned at 3.12.0 but remains an external include dependency; it is not embedded.
Includes from implementation-section files stay verbatim inside
`#ifdef TUGMCP_IMPLEMENTATION` and are never hoisted into the declaration
prelude.
