# 构建（Building）

> 状态快照：**2026-10-25**（M1–M6 完成；单头发布产物已支持）。
>
> 本文说明如何从源码配置、构建并测试 `tugmcp`。消费方式与宿主集成见
> [`docs/integration.md`](./integration.md)，公共 API 见
> [`docs/api.md`](./api.md)，单头产物细节见
> [`docs/amalgamation.md`](./amalgamation.md)。

## 1. 前置条件

| 组件 | 版本/要求 | 用途 |
|---|---|---|
| CMake | ≥ 3.20 | 配置与构建；`SYSTEM` include 在 MSVC 上的 `/external:I` 映射需 ≥ 3.24，本库为 best-effort |
| C++ 编译器 | MSVC 2019 16.8+ / GCC 9+ / Clang 10+ | C++17；`CMAKE_CXX_STANDARD=17`、`CXX_EXTENSIONS=OFF` |
| 线程库 | 平台默认 | `tugmcp::core` 以 PUBLIC 方式链接 `Threads::Threads` |
| Python 3 | 任意受支持版本 | 生成/校验单头产物、运行 `amalgamation.*` 测试、conformance runner |
| Node / npx | Inspector 2.12 需要 Node ≥ 22.19 | 仅 `amalgamation.http` 与 `TUGMCP_CONFORMANCE=ON` 需要 |
| OpenSSL | ≥ 3.0 | 仅 `TUGMCP_TLS=ON`；不支持 1.1.1 |

网络：默认构建**不需要网络**。依赖来自仓库内 `3rd/`，不使用 FetchContent。

## 2. 依赖策略

- 第三方源码 vendored 在 `3rd/`，通过手写 INTERFACE target 暴露。
- `TUGMCP_VERIFY_3RD_HASHES` 默认 `ON`，configure 时校验 `3rd/` 依赖的
  SHA256；版本、commit、SHA256 与许可证登记在 `3rd/README.md`。
- **`TUGMCP_USE_SYSTEM_DEPS` 当前未实现**：设为 `ON` 会直接
  `FATAL_ERROR`。不要依赖“自动回退 vendored”或系统包。
- **不使用 FetchContent 拉取依赖**，也不要求系统包；消费者只需本仓库。
- 单头产物仍要求外部提供 nlohmann/json（HTTP/TLS 还需 cpp-httplib，
  TLS 还需 OpenSSL）；见 §8 与 [`docs/amalgamation.md`](./amalgamation.md)。

## 3. CMake 选项

| 选项 | 默认 | 作用 | 限制/备注 |
|---|---|---|---|
| `TUGMCP_BUILD_TESTS` | `ON` | 生成测试可执行文件并 `enable_testing()` | 测试可执行文件始终编译，不会“按需跳过” |
| `TUGMCP_BUILD_EXAMPLES` | `ON` | 构建 `examples/` | 生成 `tugmcp_echo_server`；HTTP 开启时再生成 `tugmcp_echo_client` |
| `TUGMCP_BUILD_QT_EXAMPLE` | `OFF` | 构建 Qt 5 Widgets 示例（`examples/qt_widgets_server`） | 需 `TUGMCP_WITH_HTTP=ON`、`TUGMCP_BUILD_EXAMPLES=ON` 与 Qt5 Widgets；用 `-DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64` 指定 Qt |
| `TUGMCP_WITH_HTTP` | `ON` | 构建 `tugmcp::http`（Streamable HTTP/SSE） | legacy 与 conformance 依赖它 |
| `TUGMCP_TLS` | `OFF` | 启用 TLS（cpp-httplib + OpenSSL） | 需要 `TUGMCP_WITH_HTTP=ON` 且 OpenSSL ≥ 3.0 |
| `TUGMCP_TLS_BACKEND` | `openssl` | TLS 后端 | 当前只接受 `openssl`；其他值 configure 即 `FATAL_ERROR` |
| `TUGMCP_HEADER_ONLY` | `OFF` | split-header header-only 消费模式 | **未实现**：`ON` 即 `FATAL_ERROR`；单头产物**不用**这个开关 |
| `TUGMCP_USE_SYSTEM_DEPS` | `OFF` | 改用系统包 | **未实现**：`ON` 即 `FATAL_ERROR` |
| `TUGMCP_VERIFY_3RD_HASHES` | `ON` | 校验 `3rd/` 依赖 SHA256 | system 模式未实现，因此该开关始终作用于 vendored 依赖 |
| `TUGMCP_WITH_JSONCONS` | `OFF` | 构建 `tugmcp::schema_jsoncons`（2020-12） | 仅编译库可用，不进入单头 |
| `TUGMCP_WITH_VALIJSON` | `OFF` | 构建 `tugmcp::schema_valijson`（draft-07） | 仅编译库可用，不进入单头 |
| `TUGMCP_ENABLE_LEGACY_SSE` | `OFF` | 构建 legacy HTTP+SSE（2024-11-05）适配 | 需要 `TUGMCP_WITH_HTTP=ON`；不进入单头 |
| `TUGMCP_CONFORMANCE` | `OFF` | 注册官方 conformance 测试 | configure 需要 Python3、`npx`、`TUGMCP_WITH_HTTP=ON`、`TUGMCP_BUILD_TESTS=ON` |
| `TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION` | `ON` | 单头外部 Inspector/conformance 检查缺工具时仍注册并 fail loud | `OFF` 才移除 `amalgamation.http` |
| `TUGMCP_AMALGAMATION_RELEASE_DIR` | `${CMAKE_BINARY_DIR}/amalgamation/release` | `amalgamate-release` 输出目录 | cache 变量（PATH） |

> 结论：当前**只有** `TUGMCP_HEADER_ONLY=ON` 与
> `TUGMCP_USE_SYSTEM_DEPS=ON` 是 configure 期硬失败；不要把二者写进构建脚本。
> `TUGMCP_ENABLE_LEGACY_SSE` 默认关闭，2026/2025 主路径不需要它。

## 4. 标准构建与测试

### 4.1 Debug（默认推荐）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --parallel 4
```

- 单配置生成器（Ninja、Makefiles）用 `-DCMAKE_BUILD_TYPE`。
- 多配置生成器（Visual Studio、Xcode）忽略 `CMAKE_BUILD_TYPE`，应改为：

```bash
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure --parallel 4
```

### 4.2 Release

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
ctest --test-dir build-release -C Release --output-on-failure
```

### 4.3 TLS

```bash
cmake -S . -B build-tls -DCMAKE_BUILD_TYPE=Debug -DTUGMCP_TLS=ON
cmake --build build-tls --parallel
ctest --test-dir build-tls --output-on-failure
```

Windows 上 OpenSSL 不在默认搜索路径时显式指定根目录：

```powershell
cmake -S . -B build-tls -DTUGMCP_TLS=ON `
  -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL"
cmake --build build-tls --config Debug --parallel
```

要求 OpenSSL ≥ 3.0（`find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)`）。
同一进程只允许一套 OpenSSL。

### 4.4 Schema 后端

```bash
cmake -S . -B build-schema -DCMAKE_BUILD_TYPE=Debug \
  -DTUGMCP_WITH_JSONCONS=ON -DTUGMCP_WITH_VALIJSON=ON \
  -DTUGMCP_WITH_HTTP=OFF
cmake --build build-schema --parallel
ctest --test-dir build-schema --output-on-failure
```

- schema 后端是独立静态库；两者可同时开启。
- schema 构建关闭 HTTP 时，`amalgamation.http` 会因缺少 HTTP 产物而不适用；
  [`docs/roadmap.md`](./roadmap.md) 记录的 schema 口径为
  `TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF`（总计 198，`amalgamation` 2/2）。

### 4.5 Legacy HTTP+SSE

```bash
cmake -S . -B build-legacy -DCMAKE_BUILD_TYPE=Debug \
  -DTUGMCP_ENABLE_LEGACY_SSE=ON
cmake --build build-legacy --parallel
ctest --test-dir build-legacy --output-on-failure
```

legacy 默认关闭，且只在编译库中提供；不要用单头产物承载 legacy。

### 4.6 外部 conformance

```bash
cmake -S . -B build-conformance -DCMAKE_BUILD_TYPE=Debug \
  -DTUGMCP_CONFORMANCE=ON \
  -DTUGMCP_WITH_HTTP=ON \
  -DTUGMCP_BUILD_TESTS=ON
cmake --build build-conformance --parallel
ctest --test-dir build-conformance -L conformance --output-on-failure
```

configure 需要 Python3 与 `npx`，缺失即 `FATAL_ERROR`。测试会构建/启动
`tugmcp_conformance_server`，分别跑 2026-07-28 与 2025-11-25；不是本地
vectors 的替代品。

### 4.7 单头产物

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # 需要 Python3 才会注册 target
cmake --build build --target amalgamate
cmake --build build --target amalgamate-release
ctest --test-dir build -L amalgamation --output-on-failure
```

- `amalgamate` 输出三个头到 `${CMAKE_BINARY_DIR}/amalgamation/`。
- `amalgamate-release` 额外输出发布目录（默认
  `${CMAKE_BINARY_DIR}/amalgamation/release`）。
- 这些 target 不进入 `ALL`，且不需要网络。
- 详细矩阵、哈希、许可证与外部验证见
  [`docs/amalgamation.md`](./amalgamation.md)。

## 5. Target 一览

默认库 target 均为 **STATIC**。当前**没有 install/export 规则**，因此不能
`find_package(tugmcp)`；消费方式是 `add_subdirectory`、FetchContent 源码集成
或单头产物（见 [`docs/integration.md`](./integration.md)）。

| Target | 类型 | 依赖/传播 | 说明 |
|---|---|---|---|
| `tugmcp::core` | STATIC | PUBLIC `Threads::Threads`、`tugmcp_3rd_json`；PUBLIC include `include/`、`cxx_std_17` | MCP server/client、stdio、core 协议 |
| `tugmcp::http` | STATIC | PUBLIC `tugmcp::core`；PRIVATE `tugmcp_3rd_httplib`；Windows PRIVATE `ws2_32`；TLS 时 PRIVATE `OpenSSL::SSL`/`OpenSSL::Crypto`、Windows `crypt32` | Streamable HTTP server/client；legacy 源文件按开关加入 |
| `tugmcp::schema_jsoncons` | STATIC | PUBLIC `tugmcp::core`；PRIVATE `tugmcp_3rd_jsoncons` | `TUGMCP_WITH_JSONCONS=ON` 时存在 |
| `tugmcp::schema_valijson` | STATIC | PUBLIC `tugmcp::core`；PRIVATE `tugmcp_3rd_valijson` | `TUGMCP_WITH_VALIJSON=ON` 时存在 |
| `tugmcp::tests` | INTERFACE | 传播 `tugmcp_3rd_doctest` 与测试 helpers include | 仅测试使用；不进入安装/发布 |
| `amalgamate` | 自定义 target | Python3 | 生成三份单头 |
| `amalgamate-release` | 自定义 target | Python3 | 打包发布目录 |
| `tugmcp_unit_tests` | 可执行 | `tugmcp::core`、`tugmcp::tests` | label `unit` |
| `tugmcp_odr_tests` | 可执行 | `tugmcp::core`、`tugmcp::tests` | 双 TU/ODR；label `unit` |
| `tugmcp_executor_tests` | 可执行 | `tugmcp::core`、`tugmcp::tests` | 线程池；label `integration` |
| `tugmcp_integration_tests` | 可执行 | `tugmcp::core`、`tugmcp::tests`；HTTP 时 `tugmcp::http`、`tugmcp_3rd_httplib` | label `integration`；按开关追加 HTTP/TLS/legacy 源文件 |
| `tugmcp_conformance_server` | 可执行 | `tugmcp::core`、`tugmcp::http` | 仅 `TUGMCP_CONFORMANCE=ON`；测试 fixture |
| `tugmcp_echo_server` | 可执行（example） | `tugmcp::core`；HTTP 时 `tugmcp::http` | 默认 stdio，`--http` 启动 HTTP |
| `tugmcp_echo_client` | 可执行（example） | `tugmcp::http`、`tugmcp::core` | 仅 `TUGMCP_WITH_HTTP=ON` |

## 6. 测试标签与预期数量

实际注册的 CTest label 只有四类：

| Label | 内容 | 过滤命令 |
|---|---|---|
| `unit` | `tugmcp_unit_tests`、`tugmcp_odr_tests` | `ctest -L unit` |
| `integration` | `tugmcp_executor_tests`、`tugmcp_integration_tests`（含 HTTP/TLS/legacy 用例） | `ctest -L integration` |
| `amalgamation` | `amalgamation.core`、`amalgamation.release`、`amalgamation.http` | `ctest -L amalgamation` |
| `conformance` | `conformance.external`、`conformance.external.2025` | `ctest -L conformance` |

> 设计文档中提到的 `tls`/`slow`/`win-only` 过滤属于 CI 规划口径；当前
> `tests/CMakeLists.txt` **没有**为 TLS/慢测试打独立 label，TLS 用例随
> `integration` 一起注册。

2026-10-25 实测总量（见 [`docs/roadmap.md`](./roadmap.md)）：

| 构建 | 通过数 | 说明 |
|---|---|---|
| 默认 Debug | **267/267** | 含 `amalgamation` 3 项 |
| `TUGMCP_TLS=ON` | **269/269** | 追加 TLS loopback |
| `TUGMCP_ENABLE_LEGACY_SSE=ON` | **275/275** | 追加 legacy 用例 |
| Schema（jsoncons+valijson，HTTP OFF，外部单头检查 OFF） | **198/198** | `amalgamation` 2/2 |
| `ctest -R conformance` | **2/2** | 2026 121 required checks / 2025 73，均 0 failure |
| `ctest -L amalgamation` | **3/3** | default/TLS/legacy；schema 构建为 2/2 |

建议：

```bash
# 快速反馈，仅编译库测试
ctest --test-dir build -L "unit|integration" --output-on-failure --parallel 4

# 单跑一个用例（doctest 名称带前缀）
ctest --test-dir build -R "integration\." --output-on-failure

# 单头/发布验证（内部 RUN_SERIAL，耗时较长）
ctest --test-dir build -L amalgamation --output-on-failure

# 外部 conformance（耗时最长）
ctest --test-dir build -L conformance --output-on-failure
```

`amalgamation.core`/`amalgamation.release`/`amalgamation.http` 均设置了
`RUN_SERIAL TRUE`，不要用并行参数期待它们互相重叠。

## 7. 常见问题

| 现象 | 原因/处理 |
|---|---|
| configure 报找不到 OpenSSL | 仅 TLS 构建需要；确认 OpenSSL ≥ 3.0，设置 `-DOPENSSL_ROOT_DIR=...`，并让 `SSL`/`Crypto` 可被 `find_package` 找到 |
| Windows 链接缺 `ws2_32` | 编译模式由 `tugmcp::http` PRIVATE 传播到最终链接；**单头消费者必须自己链接** `ws2_32` |
| Windows TLS 链接缺 `crypt32` | TLS 编译模式由 target 传播；单头 TLS 消费者需自行链接 `crypt32` |
| 找不到 `npx` | `TUGMCP_CONFORMANCE=ON` 在 configure 即失败；默认构建只在运行 `amalgamation.http` 时失败。安装 Node ≥ 22.19，或显式 `-DTUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF` 移除该检查 |
| `TUGMCP_HEADER_ONLY` 直接失败 | 该模式未实现，是预期行为；改用静态库或单头产物 |
| `TUGMCP_USE_SYSTEM_DEPS` 直接失败 | 同上；继续使用 `3rd/` vendored 依赖 |
| TLS/非 TLS 混用出现诡异链接错误 | `TUGMCP_TLS_ENABLED`/`CPPHTTPLIB_OPENSSL_SUPPORT` 必须对每个看到 `httplib.h` 的 TU 一致；不要混用 `tugmcp_http_tls.hpp` 与 `tugmcp_http.hpp`/`tugmcp.hpp` |
| 找不到生成的单头 | 产物只写 `build/`（`${CMAKE_BINARY_DIR}/amalgamation/`），不提交源码树；先运行 `amalgamate` |
| `ctest` 跑不到测试 | 多配置生成器需要 `-C Debug`/`-C Release`；确认 `TUGMCP_BUILD_TESTS=ON` |
| 没有 `amalgamate` target | configure 时未找到 Python3；安装后重新 configure |

## 8. 单头发布产物

```bash
cmake --build build --target amalgamate
cmake --build build --target amalgamate-release
```

产物矩阵（消费与链接要求、TLS 宏/ODR、发布目录内容、SHA256 校验与外部
Inspector/conformance 验证）以 [`docs/amalgamation.md`](./amalgamation.md)
为准。要点：

- `tugmcp.hpp`：core + stdio，外部 nlohmann/json，链接 `Threads::Threads`。
- `tugmcp_http.hpp`：另需 cpp-httplib，Windows 链接 `ws2_32`。
- `tugmcp_http_tls.hpp`：另需 OpenSSL ≥ 3.0，Windows 链接 `ws2_32` + `crypt32`。
- schema/legacy **不进入**单头；需要它们时用编译库 target。
