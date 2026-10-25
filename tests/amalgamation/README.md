# Single-header amalgamation tests

This directory exercises the generated **core + stdio** single header. The
source of truth remains `include/` plus `src/`; `tools/amalgamate.py` produces
the release artifact into `build/amalgamation/` (never committed).

## Run

```bash
python tests/amalgamation/run_tests.py
```

The script:

1. generates `build/amalgamation/tugmcp.hpp`;
2. generates it a second time into a temporary directory and asserts the two
   SHA256 hashes are identical;
3. re-verifies the artifact with `tools/amalgamate.py --check`;
4. configures, builds and runs the standalone two-TU consumer project
   (`consumer_core.cpp` owns `TUGMCP_IMPLEMENTATION`; `consumer_core_tu2.cpp`
   includes the header without it);
5. links `Threads::Threads` and runs the `amalgamation.smoke` test.

Requirements: Python 3.8+, CMake 3.20+, a C++17 compiler, and the vendored
nlohmann/json headers under `3rd/nlohmann_json`.

## Manual generation

```bash
python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
python tools/amalgamate.py --profile core --check build/amalgamation/tugmcp.hpp
```

The generated header requires `TUGMCP_IMPLEMENTATION` in exactly one
translation unit. All other TUs include it without the macro. Consumers must
link `Threads::Threads`; nlohmann/json remains an external include dependency
and is not embedded.

This lane is intentionally **not** part of the default ctest suite yet.
