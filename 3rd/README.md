# Third-party dependencies (`3rd/`)

All dependencies are **vendored in-tree** as plain source files. They are **not**
git submodules and are not fetched at configure time (no `FetchContent`).

The SHA256 values below are verified by `cmake/verify_hashes.cmake`, which is
included from the top-level `CMakeLists.txt` when `TUGMCP_VERIFY_3RD_HASHES=ON`
(the default). A mismatch aborts configuration with `FATAL_ERROR`. Verification
is skipped entirely when `TUGMCP_USE_SYSTEM_DEPS=ON`.

| Name | Version | Tag | Commit | License | Upstream URL |
|---|---|---|---|---|---|
| doctest | 2.5.3 | `v2.5.3` | `2d0a9359a60c51affe2a9bebb1be1dca47868151` | MIT | https://github.com/doctest/doctest |
| nlohmann/json | 3.12.0 | `v3.12.0` | `55f93686c01528224f448c19128836e7df245f72` | MIT | https://github.com/nlohmann/json |
| jsoncons | 1.10.0 | `v1.10.0` | `9e57e421285422d7aeeb5aa7dc839f6a19121ee6` | BSL-1.0 | https://github.com/danielaparker/jsoncons |
| Valijson | 1.1.3 | `v1.1.3` | `5eac4b66f20b2c189ae40281c29e71cf39a958ec` | BSD-2-Clause | https://github.com/tristanpenman/valijson |

## Vendored files and SHA256

| File | SHA256 |
|---|---|
| `3rd/doctest/doctest/doctest.h` | `cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421` |
| `3rd/doctest/scripts/cmake/doctest.cmake` | `6a60f778f86958c23d308d532356b37ec6e735137be7adf02cb17d688b559711` |
| `3rd/doctest/scripts/cmake/doctestAddTests.cmake` | `3ec2839f12b7a12b0d7ef6244b60eecae40ad797caa3f016fc09875956a518dc` |
| `3rd/nlohmann_json/nlohmann/json.hpp` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| `3rd/nlohmann_json/nlohmann/json_fwd.hpp` | `fb6aa70cbece087f37ab4685c182b287c53be54f785f981b9db9d30d2d028b37` |
| `LICENSES/doctest-MIT.txt` | `0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5` |
| `LICENSES/nlohmann-json-MIT.txt` | `46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603` |
| `LICENSES/jsoncons-BSL-1.0.txt` | `97550c71a0bdfc20bf85c6c5167d7b7785f9bd3448baee048fbedf742ed52270` |
| `LICENSES/valijson-BSD-2-Clause.txt` | `61b5766db38eaa3f320c366357956d7743424bfbf90fd78f5943866ba237589e` |

### Multi-file libraries

`jsoncons` and `Valijson` are vendored as full header trees, so they use
per-file manifests instead of single hashes:

- `3rd/jsoncons/MANIFEST.sha256` (177 files)
- `3rd/valijson/MANIFEST.sha256` (49 files)

Each line is `<sha256>  <path>` relative to the library directory. The
manifests are verified by `cmake/verify_manifest.cmake`, which is invoked from
`cmake/verify_hashes.cmake` when `TUGMCP_VERIFY_3RD_HASHES=ON`.
`SOURCE.txt` in each directory records the upstream tarball hash and vendoring
notes (Valijson's `fast_float` submodule is intentionally not vendored).

## CMake targets

`3rd/CMakeLists.txt` exposes header-only INTERFACE targets with `SYSTEM`
include directories so third-party warnings do not pollute our builds:

- `tugmcp_3rd_json` — nlohmann/json
- `tugmcp_3rd_doctest` — doctest
- `tugmcp_3rd_jsoncons` — jsoncons (2020-12 schema)
- `tugmcp_3rd_valijson` — Valijson (draft-07 schema)

Aggregated license texts live in `LICENSES/`.
