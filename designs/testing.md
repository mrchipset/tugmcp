# 测试与一致性设计

> 本文是 `tugmcp` **测试与一致性**的权威文档：测试分层、框架选型、目录与 target、必测清单、fixtures、确定性卫生、CI 门禁与里程碑验收，均以本文为准。
>
> 与主设计的关系：`mcp-library-design.md` §19 是概览表与最低测试矩阵；本文把 §19 展开为可执行的测试契约。两者冲突时以本文为准，并回填 §19，避免两处口径漂移。
>
> 状态：**设计稿（design draft）**。仓库尚未进入实现阶段；本文出现的 target、目录、代码块均为设计草图，**未实现、未编译**，不作为“已存在”的证据。术语、target 名与构建开关沿用 `mcp-library-design.md`（§6.1/§20.1）与 `protocol-decisions.md`（D9/D12/D14/D15）。

## 1. 目标与非目标

### 1.1 目标

| 目标 | 含义 |
|---|---|
| 可执行 | 每条测试都能以单一命令复现；不依赖人工准备或外部服务（conformance 除外，见 §8）。 |
| 确定性 | 同一输入在任意机器、任意并发调度下得到同一结论；不以时间/顺序偶然性通过。 |
| 可诊断 | 失败信息直接指出“哪条断言、期望值、实际值、对应设计条目”，无需二次调试。 |
| 分层 | 单元、fake、vectors、loopback、负例、ODR、conformance、性能各自独立，可单独筛选运行。 |
| 双形态一致 | `TUGMCP_HEADER_ONLY` ON/OFF、`TUGMCP_TLS` ON/OFF 跑同一测试集，行为一致。 |

### 1.2 非目标

本库测试**不包括**：

- MCP 规范本身的正确性——交给 conformance 套件、Inspector 与自定义 2026 vectors，不在单元测试里重新论证规范；
- 第三方库内部行为（nlohmann/json、cpp-httplib、jsoncons、Valijson、OpenSSL、doctest）；
- 宿主业务语义、执行器策略、硬件或安全结论；
- 真实网络、公网服务、真实凭据；
- 性能数字作为合并门禁（仅夜线观测，见 §8）。

> 测试只证明“库按设计契约表现”，不证明“库保证设备安全”。安全边界仍由应用与硬件承担（`protocol-decisions.md` D10）。

## 2. 框架选型

### 2.1 结论

采用 **doctest v2.5.3** 作为唯一测试框架。

| 维度 | doctest v2.5.3 |
|---|---|
| 许可 | MIT |
| 语言下限 | C++11（满足本库 C++17 下限） |
| 形态 | 单头 `doctest.h`，vendored 到 `3rd/`，经 `tugmcp_3rd_doctest` INTERFACE 传播 |
| 数据驱动 | `GENERATE(...)` 生成器，适合 vectors 与参数化边界 |
| 报告 | JUnit reporter，CI 直接消费 |
| CTest 集成 | `doctest_discover_tests()`，每个 `TEST_CASE` 注册为独立 ctest 用例 |
| 编译开销 | 同量级框架中最低，适合“每 TU 都 include”的常态 |

### 2.2 备选与排除

| 候选项 | 判断 | 理由 |
|---|---|---|
| Catch2 v3.16.1 | 备选 | 功能完整，但 v3 需要单独编译库、语言下限 C++14；相对 doctest 增加构建与链接面。仅当 doctest 出现阻塞性缺陷时启用。 |
| Boost.UT | 排除 | C++20 依赖，违反本库 C++17 下限。 |
| snitch | 排除 | C++20 依赖，同上。 |
| UnitTest++ | 排除 | 长期停滞，维护与工具链适配风险高。 |
| μtest | 排除 | 无稳定版本号，不满足依赖固定版本/可审计要求（D15）。 |
| GoogleTest | 仅基线参照 | 需编译库、依赖面更大；不作为本库测试框架。 |

### 2.3 header-only 框架在 CI 的真实含义

doctest 是 header-only，但**不意味着“编译一次、到处复用”**：

- 每个包含 `doctest.h` 的 TU 仍会编译框架实现（模板与宏展开），CI 的编译时间随测试 TU 数线性增长；
- `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` 只在每个测试可执行文件的**恰好一个** TU（`tests/main.cpp`）中定义，runner/main 与信号处理等实现只实例化一次，其余 TU 只 include，不重复定义；
- 因此“单头”降低的是依赖与链接管理成本，不是把编译成本降为零；编译时间基线仍按 `header-only.md` §7 记录并在 CI 观测。

## 3. 测试分层

| 层 | 内容 | 执行形态 | 失败时能定位什么 |
|---|---|---|---|
| Unit | JSON-RPC codec、严格 JSON（duplicate keys/UTF-8/int64/NaN/Batch）、错误模型、registry、RequestId、canonical JSON/`tool_schema_hash`、schema adapter | 进程内，无 IO | 具体编解码/校验规则被破坏，可直接定位到函数与输入样本 |
| Fakes | `FakeTransport`（乱序/重复/超大/断连）、`FakeExecutor`（队列满/优先级）、`FakeClock`、`FakeSchemaValidator`、`TempDir` | helper 头 | 是库逻辑错误还是真实传输/时钟干扰；排除环境因素 |
| Protocol vectors | 2026/2025 请求/响应/通知/错误样本 | JSON 文件驱动 | 某个具体 wire 字段/版本行为偏离样本，向量文件名即定位线索 |
| Loopback | `McpServer`+`McpClient` 同进程：stdio、Streamable HTTP、TLS on/off | 真实 socket（loopback） | 端到端链路（framing、header、session、流）在哪一层断开 |
| Negative/TLS | `reason × 构建状态 × 角色/观测点` 矩阵（§6.2）、宏守卫负例编译、非法 option | 创建期断言 + `try_compile` | 配置/构建期契约被破坏，且确认失败发生在发包之前 |
| ODR | 双 TU 同时 include 公共头，链接无重复符号 | 独立可执行 | 头文件的 inline/全局对象违规，或宏跨 TU 不一致 |
| Conformance | `@modelcontextprotocol/conformance@0.2.0-alpha.12` + 自定义 2026 vectors | 外部 runner，nightly | 与规范/参考实现的行为差异，按 conformance 用例名定位 |
| Performance | 延迟/吞吐/内存/8h soak | 非门禁，nightly | 回归趋势与资源上限，不用于单次合并判定 |

## 4. 目录与 target

### 4.1 目录布局

```text
tests/
  CMakeLists.txt
  main.cpp                     # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN（每个可执行一个）
  helpers/                     # → tugmcp::tests INTERFACE
    fake_transport.hpp  fake_executor.hpp  fake_clock.hpp
    fake_schema_validator.hpp  fake_authenticator.hpp  fake_authorizer.hpp
    fake_approval_hook.hpp  fake_audit_hook.hpp
    temp_dir.hpp  fixture.hpp
  unit/                        # codec/strict_json/error/registry/canonical/schema
  integration/                 # loopback_stdio/loopback_http/cancel_progress/subscriptions/mrtr/shutdown/resources/legacy/win_stdio
  executor/                    # 真实线程池/并发取消/饱和队列/shutdown/异常
  conformance/
    vectors/2026/*.json  vectors/2025/*.json  conformance_test.cpp  run_external.py
    expected_cases.json        # coverage_2026.json 写到 build 目录，不进源码树
  odr/ tu_a.cpp tu_b.cpp
  fixtures/tls/                # 仅测试用；禁止用于生产
    generate.sh  ca.pem  ca.key  server.pem  server.key  client.pem  client.key  bad_*.pem
```

> 上表是目标布局，当前**未实现**。新增测试文件应落在这几类目录内；不为单个测试另开顶层目录。

### 4.2 target 清单

| target | 形态 | 主 label | 说明 |
|---|---|---|---|
| `tugmcp::tests`（实体 `tugmcp_tests`） | INTERFACE helper 库 | — | fake/helper/fixture 头；INTERFACE 传播 `tugmcp_3rd_doctest`；不编译源文件 |
| `tugmcp_unit_tests` | 可执行 | `unit` | Unit + Fakes + 部分 vectors（进程内） |
| `tugmcp_integration_tests` | 可执行 | `integration` | loopback stdio；`TUGMCP_WITH_HTTP` 时追加 HTTP、取消/进度、订阅、MRTR、Resources、关闭、legacy（`TUGMCP_ENABLE_LEGACY_SSE`） |
| `tugmcp_tls_tests` | 可执行 | `tls` | TLS reason 矩阵、9 字段、mTLS、证书负例；仅 `TUGMCP_WITH_HTTP` 时构建 |
| `tugmcp_slow_tests` | 可执行 | `slow` | 慢客户端、超大消息、有界队列/断连阈值；仅 `TUGMCP_WITH_HTTP` 时构建 |
| `tugmcp_executor_tests` | 可执行 | `integration` | 真实 executor：默认线程池、并发取消、饱和队列、重复 shutdown、handler 抛异常 |
| `tugmcp_win_stdio_tests` | 可执行 | `win-only` | 仅 `WIN32` 构建；Windows binary mode/stdout 纯净/EOF 子集 |
| `tugmcp_schema_tests` | 可执行 | `unit` | `TUGMCP_WITH_JSONCONS` 或 `TUGMCP_WITH_VALIJSON` 时构建 |
| `tugmcp_conformance_tests` | 可执行 | `conformance` | 本地 vectors + 外部 runner 入口 |
| `tugmcp_odr_tests` | 可执行 | `unit` | 双 TU ODR 链接检查；`tu_a.cpp`/`tu_b.cpp` 各含最小 `TEST_CASE`，只 include 公共头 |

- 每个可执行**只打一个主 label**：`unit`/`integration`/`tls`/`slow`/`win-only`/`conformance`。TLS 与 slow 必须是独立可执行，避免同一 exe 混合快线/夜线语义。
- `tugmcp_unit_tests`/`tugmcp_integration_tests`/`tugmcp_odr_tests`/`tugmcp_executor_tests`/`tugmcp_conformance_tests` 始终编译（`TUGMCP_BUILD_TESTS=ON` 时），不受 `TUGMCP_HEADER_ONLY` 影响；该开关只切换库 target（`tugmcp::core`/`tugmcp::http`/`tugmcp::schema_*`）为 INTERFACE（D9）。`tugmcp_tls_tests`/`tugmcp_slow_tests`/`tugmcp_schema_tests` 随对应开关条件构建，`tugmcp_win_stdio_tests` 仅 `WIN32` 构建。
- `tugmcp::tests` 只放 helper 头，不复制库内部实现；helper 的行为必须是“可预期的假实现”，不是第二套协议栈。

### 4.3 doctest 约定

1. **单 main**：每个测试可执行恰好一个 TU 定义 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`（`tests/main.cpp`）；其余 TU 只 `#include <doctest/doctest.h>`，避免重复符号与重复注册。
2. **标签**：`unit` / `integration` / `tls` / `slow` / `conformance` / `win-only`。**每个可执行只挂一个主 label**；TLS 与 slow 用例放独立可执行（`tugmcp_tls_tests`/`tugmcp_slow_tests`），不靠同一 exe 内混合 tag 切分快线/夜线。
3. **发现**：`doctest_discover_tests()` 把每个 `TEST_CASE` 注册为独立 ctest 用例，并加 `TEST_PREFIX`（如 `unit.`）便于失败定位；超时按层设置：unit/integration/odr = 2s，TLS/executor = 10s，slow = 30s（见 §4.4）；不使用单一“大用例”掩盖失败。
4. **数据驱动**：边界值与 vectors 用 `GENERATE(...)` 或外部 JSON 文件，禁止复制粘贴多份近似用例。
5. **负例编译**：`try_compile` 断言宏守卫不一致时**编译失败**（如 `#ifndef TUGMCP_TLS_ENABLED` 与 `CPPHTTPLIB_OPENSSL_SUPPORT` 冲突）。正/负例都通过 `LINK_LIBRARIES tugmcp::http tugmcp::core` 取得真实 include/宏，不手写 include 路径；必须同时跑正例，并检查失败输出确实来自守卫而非缺 include；这类检查在 configure 阶段执行，不进 ctest 运行时门禁。
6. **不依赖执行顺序**：用例之间不共享可变全局状态；需要共享的只有只读 fixture。

### 4.4 CMake 骨架（草图，未编译）

> 目标是可落地：vendored/system 两种依赖模式、条件 target、单主 label、负例 `try_compile`、数据路径都写清。实际实现仍需按构建结果调整。**草图，未编译。**

```cmake
# tests/CMakeLists.txt —— 草图，未编译
include(CTest)
enable_testing()

# doctest CMake 模块固定副本：仓库内 cmake/doctest.cmake（MIT，登记在 LICENSES/）。
# vendored 与 system 两种模式共用；不依赖任一第三方脚本目录或系统安装路径。
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(doctest)

add_library(tugmcp_tests INTERFACE)
# vendored: tugmcp_3rd_doctest 是 INTERFACE target；
# system: 它是 doctest::doctest 的 ALIAS（§14.3），下游无需 if(system-deps) 分支。
target_link_libraries(tugmcp_tests INTERFACE tugmcp_3rd_doctest)
add_library(tugmcp::tests ALIAS tugmcp_tests)

# 所有相对路径以 tests/ 为基准，避免出现 tests/tests/...。
set(TUGMCP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(TUGMCP_TEST_MAIN "${TUGMCP_TEST_DIR}/main.cpp")
set(TUGMCP_TEST_DATA_DEF "TUGMCP_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
# 等价写法：target_compile_definitions(<exe> PRIVATE TUGMCP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# --- unit（主 label: unit） ---
add_executable(tugmcp_unit_tests
  "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/unit/codec_test.cpp")
target_link_libraries(tugmcp_unit_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_unit_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
doctest_discover_tests(tugmcp_unit_tests TEST_PREFIX unit.
  PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

# --- ODR（主 label: unit；快线可跑） ---
# tu_a.cpp / tu_b.cpp 只 include 公共头，各含一个最小 TEST_CASE；main 由 main.cpp 提供。
add_executable(tugmcp_odr_tests
  "${TUGMCP_TEST_MAIN}"
  "${TUGMCP_TEST_DIR}/odr/tu_a.cpp" "${TUGMCP_TEST_DIR}/odr/tu_b.cpp")
target_link_libraries(tugmcp_odr_tests PRIVATE tugmcp::core tugmcp::tests)
# ODR 是链接产物，直接 add_test 运行
add_test(NAME unit.odr_link COMMAND tugmcp_odr_tests)
set_tests_properties(unit.odr_link PROPERTIES LABELS unit TIMEOUT 2
  WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

# --- integration：stdio 始终构建；HTTP/TLS/slow 受 TUGMCP_WITH_HTTP 控制 ---
add_executable(tugmcp_integration_tests
  "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/integration/stdio_loopback_test.cpp")
target_link_libraries(tugmcp_integration_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_integration_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
doctest_discover_tests(tugmcp_integration_tests TEST_PREFIX integration.
  PROPERTIES LABELS integration TIMEOUT 2 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

# --- Windows stdio（主 label: win-only，独立可执行；仅 Windows 构建） ---
if(WIN32)
  add_executable(tugmcp_win_stdio_tests
    "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/integration/win_stdio_test.cpp")
  target_link_libraries(tugmcp_win_stdio_tests PRIVATE tugmcp::core tugmcp::tests)
  target_compile_definitions(tugmcp_win_stdio_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
  doctest_discover_tests(tugmcp_win_stdio_tests TEST_PREFIX win.
    PROPERTIES LABELS win-only TIMEOUT 2 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")
endif()

if(TUGMCP_WITH_HTTP)
  target_sources(tugmcp_integration_tests PRIVATE
    "${TUGMCP_TEST_DIR}/integration/http_loopback_test.cpp"
    "${TUGMCP_TEST_DIR}/integration/mrtr_test.cpp")
  target_link_libraries(tugmcp_integration_tests PRIVATE tugmcp::http)

  # --- TLS（主 label: tls，独立可执行） ---
  add_executable(tugmcp_tls_tests
    "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/integration/tls_test.cpp")
  target_link_libraries(tugmcp_tls_tests PRIVATE tugmcp::core tugmcp::http tugmcp::tests)
  target_compile_definitions(tugmcp_tls_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
  doctest_discover_tests(tugmcp_tls_tests TEST_PREFIX tls.
    PROPERTIES LABELS tls TIMEOUT 10 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

  # --- slow（主 label: slow，独立可执行；超时例外放宽） ---
  add_executable(tugmcp_slow_tests
    "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/integration/slow_client_test.cpp")
  target_link_libraries(tugmcp_slow_tests PRIVATE tugmcp::core tugmcp::http tugmcp::tests)
  target_compile_definitions(tugmcp_slow_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
  doctest_discover_tests(tugmcp_slow_tests TEST_PREFIX slow.
    PROPERTIES LABELS slow TIMEOUT 30 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")
endif()

# --- 真实 executor（主 label: integration） ---
add_executable(tugmcp_executor_tests
  "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/executor/executor_test.cpp")
target_link_libraries(tugmcp_executor_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_executor_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
doctest_discover_tests(tugmcp_executor_tests TEST_PREFIX executor.
  PROPERTIES LABELS integration TIMEOUT 10 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

# --- schema（仅启用对应 validator 时构建；主 label: unit） ---
if(TUGMCP_WITH_JSONCONS OR TUGMCP_WITH_VALIJSON)
  add_executable(tugmcp_schema_tests
    "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/unit/schema_test.cpp")
  target_link_libraries(tugmcp_schema_tests PRIVATE tugmcp::core tugmcp::tests)
  if(TUGMCP_WITH_JSONCONS)
    target_link_libraries(tugmcp_schema_tests PRIVATE tugmcp::schema_jsoncons)
  endif()
  if(TUGMCP_WITH_VALIJSON)
    target_link_libraries(tugmcp_schema_tests PRIVATE tugmcp::schema_valijson)
  endif()
  target_compile_definitions(tugmcp_schema_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
  doctest_discover_tests(tugmcp_schema_tests TEST_PREFIX schema.
    PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")
endif()

# --- legacy（仅开关启用时把用例并入 integration） ---
if(TUGMCP_ENABLE_LEGACY_SSE)
  target_sources(tugmcp_integration_tests PRIVATE
    "${TUGMCP_TEST_DIR}/integration/legacy_sse_test.cpp")
endif()

# --- conformance：本地 vectors 始终有；外部 runner 仅 TUGMCP_CONFORMANCE=ON ---
add_executable(tugmcp_conformance_tests
  "${TUGMCP_TEST_MAIN}" "${TUGMCP_TEST_DIR}/conformance/conformance_test.cpp")
target_link_libraries(tugmcp_conformance_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_conformance_tests PRIVATE ${TUGMCP_TEST_DATA_DEF})
doctest_discover_tests(tugmcp_conformance_tests TEST_PREFIX conformance.
  PROPERTIES LABELS conformance TIMEOUT 2 WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")

if(TUGMCP_CONFORMANCE)
  if(NOT TARGET tugmcp_echo_server)
    message(FATAL_ERROR "TUGMCP_CONFORMANCE=ON 需要 examples/echo_server 的 tugmcp_echo_server target")
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "TUGMCP_CONFORMANCE=ON 需要 Python3 Interpreter（不得静默降级）")
  endif()

  add_test(NAME conformance.external
    COMMAND Python3::Interpreter "${TUGMCP_TEST_DIR}/conformance/run_external.py"
      --echo-server "$<TARGET_FILE:tugmcp_echo_server>"
      --transport stdio --transport http
      --package @modelcontextprotocol/conformance@0.2.0-alpha.12
      --expected-cases "${TUGMCP_TEST_DIR}/conformance/expected_cases.json"
      --coverage-out "${CMAKE_CURRENT_BINARY_DIR}/coverage_2026.json")

  # fixture：runner 自启 echo server；env 用例只负责声明 conformance 环境的生命周期。
  add_test(NAME conformance.env COMMAND ${CMAKE_COMMAND} -E true)
  set_tests_properties(conformance.env PROPERTIES FIXTURES_SETUP conformance_env)
  set_tests_properties(conformance.external PROPERTIES
    LABELS conformance TIMEOUT 900
    FIXTURES_REQUIRED conformance_env
    DEPENDS conformance.env
    WORKING_DIRECTORY "${TUGMCP_TEST_DIR}")
endif()

# --- 宏守卫正/负例（仅 TUGMCP_WITH_HTTP=ON 时；否则 tugmcp::http 不存在） ---
# 守卫源 compile_guard_negative.cpp 必须先 #undef TUGMCP_TLS_ENABLED 再定义目标值，
# 避免与 target 传播的宏重复定义（否则失败原因不是守卫）。
if(TUGMCP_WITH_HTTP)
  set(TUGMCP_GUARD_SRC "${TUGMCP_TEST_DIR}/unit/compile_guard_negative.cpp")

  # 正例：TLS 宏与 cpp-httplib 宏一致（都启用），必须编译通过。
  try_compile(TUGMCP_GUARD_OK
    "${CMAKE_CURRENT_BINARY_DIR}/guard_ok" "${TUGMCP_GUARD_SRC}"
    CMAKE_FLAGS "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    LINK_LIBRARIES tugmcp::http tugmcp::core
    COMPILE_DEFINITIONS "TUGMCP_TLS_ENABLED=1;CPPHTTPLIB_OPENSSL_SUPPORT"
    OUTPUT_VARIABLE TUGMCP_GUARD_OK_OUT)
  if(NOT TUGMCP_GUARD_OK)
    message(FATAL_ERROR "守卫正例应编译通过，但失败：${TUGMCP_GUARD_OK_OUT}")
  endif()

  # 负例：TLS 宏关闭却启用 cpp-httplib TLS，应被守卫拒绝。
  try_compile(TUGMCP_GUARD_BAD
    "${CMAKE_CURRENT_BINARY_DIR}/guard_bad" "${TUGMCP_GUARD_SRC}"
    CMAKE_FLAGS "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    LINK_LIBRARIES tugmcp::http tugmcp::core
    COMPILE_DEFINITIONS "TUGMCP_TLS_ENABLED=0;CPPHTTPLIB_OPENSSL_SUPPORT"
    OUTPUT_VARIABLE TUGMCP_GUARD_BAD_OUT)
  if(TUGMCP_GUARD_BAD)
    message(FATAL_ERROR "TLS 宏漂移未被守卫捕获")
  endif()
  if(NOT TUGMCP_GUARD_BAD_OUT MATCHES "TUGMCP_TLS_ENABLED")
    message(FATAL_ERROR "负例失败原因不是守卫（可能缺 include）：${TUGMCP_GUARD_BAD_OUT}")
  endif()
endif()
```

> 说明：`conformance.external` 仅在 `TUGMCP_CONFORMANCE=ON` 时注册，依赖 examples 的 `tugmcp_echo_server` target（examples 必须先于 tests 加入）；若 target 缺失，configure 直接 `FATAL_ERROR`。宏守卫检查仅在 `TUGMCP_WITH_HTTP=ON` 时执行。所有测试数据路径通过 `TUGMCP_TEST_DATA_DIR` 传入，CTest 用 `WORKING_DIRECTORY` 固定到 `tests/`。**草图，未编译。**

## 5. 必测清单

| 用例组 | 断言点 | 归属 target | 设计条目 |
|---|---|---|---|
| 严格 JSON | duplicate keys 拒绝；非法 UTF-8 拒绝；int64 边界不经过 double；NaN/Inf 拒绝；batch 拒绝；`id` 仅接受 string/int64/uint64 | `tugmcp_unit_tests` | §7.4；§19 Malformed |
| 错误模型 | `-32700/-32600/-32601/-32602/-32603` 映射；`-32020/-32022` 协议 header/version；`-32021` 缺客户端能力；本地错误 5 个 reason 与发生时点；认证失败走 HTTP 401 而非 `-32021` | `tugmcp_unit_tests` | §7.2；D10；D14；§19 Security |
| 生命周期 | 2026 stateless；2025 `initialize`/`initialized`/session；header 与 `_meta` 不一致的拒绝路径；不支持版本 → `-32022` | `tugmcp_integration_tests` | §13；§10；D12；§22.1 M2/M3 |
| MRTR | tools/resources/prompts 三类操作均支持多轮 `input_required`；客户端新 id；`requestState` 原样回传；篡改 state 拒绝；缺 `inputResponses` 拒绝；超时 | `tugmcp_integration_tests` | §13.4；§19 MRTR；D12 |
| 订阅 | ack/filter；`params._meta["io.modelcontextprotocol/subscriptionId"]` 与 listen 请求 id 关联；2025 replay；2026 re-subscribe；慢消费者不阻塞写路径 | `tugmcp_integration_tests` | §13；§19 Subscriptions；§16；D18 |
| 取消/进度 | `CancelMode` 三态（`Auto`/`CloseStream`/`Notification`）；超时同时取消 pending 与 handler；progress 仅在带 token 时发送 | `tugmcp_integration_tests` | §7.1；§19 Async；§16 |
| 背压/关闭 | 队列满返回 `QueueFull`；断连清理；shutdown 严格遵循主设计 §16（关闭顺序：停新请求→取消/等待→关流→关 transport→drain→join），本文不另立版本 | `tugmcp_integration_tests` | §8；§16；§19 Backpressure |
| TLS reason 矩阵 | 按 §6.2 的 `reason × 构建状态 × 角色/观测点` 逐行断言；TLS-off 的创建期失败用 `CountingListener` 断言 accept 次数 = 0；TLS-on 握手/证书/mTLS | `tugmcp_tls_tests` | D14；§18.1；§19 Security |
| ODR | 双 TU 同时 include 公共头链接无重复符号；`tu_a.cpp`/`tu_b.cpp` 各含一个最小 `TEST_CASE`；`TUGMCP_HEADER_ONLY` ON/OFF 同一测试集行为一致 | `tugmcp_odr_tests` | D9；`header-only.md` §7 |
| Schema | 无 validator 时 fail closed；validator 编译一次多次调用；`outputSchema` 校验 `structuredContent` | `tugmcp_schema_tests` | §17.1/§17.2；M1.5 |
| Canonical | 键序/数字/UTF-8/无 NaN；`tool_schema_hash` 对等价输入稳定、对实质变更敏感 | `tugmcp_unit_tests` | §17.3；M1.5 |
| Resources | `resources/templates/list` 分页 `cursor`/`nextCursor`；cache 字段位于 Page envelope；authorized filtering；traversal/非法 URI 拒绝 | `tugmcp_integration_tests` | §13.4；§19 Resources |
| Emergency | 饱和 pool + active rate limit 下 `/emergency` 请求仍 accepted/executed；记录 admission 结果 | `tugmcp_integration_tests` | §16；§19 Emergency |
| Reconnect | 2025 每流 `Last-Event-ID` replay；2026 re-discover/re-subscribe/分页补齐；重连时 pending 取消并清理 | `tugmcp_integration_tests` | §13；§19 Reconnect |
| 2025 server→client requests | roots/list、sampling/createMessage、elicitation/create 按 capability gate；缺能力 → `-32021`；2026 用 MRTR 替代 | `tugmcp_integration_tests` | §13；§19 2025 requests |
| Security limits | Origin/Host 校验、rate、body/stream 上限；401/403；413/429；坏 token 不落 JSON-RPC `-32021` | `tugmcp_integration_tests` | §18；§19 Security |
| `resultType`/`ttlMs`/`cacheScope` | `resultType` 与工具结果一致；`ttlMs`/`cacheScope` 出现在规定 envelope 且类型合法；缺失/非法值的拒绝路径 | `tugmcp_integration_tests` | §7.3；§13.4 |
| 2026 GET/DELETE 与头校验 | 2026 下 GET/DELETE → 405；`Mcp-Method`/`Mcp-Name` 缺失或不匹配 → 拒绝；header 与 `_meta` 不一致 → `-32020` | `tugmcp_integration_tests` | §10；§13 |
| stdio | Windows binary mode（无 CRLF 转换）；stdout 仅输出协议消息、无杂散日志；EOF 干净关闭；`maxMessageBytes` 边界 | `tugmcp_win_stdio_tests`（Windows；其他平台归 `tugmcp_integration_tests`） | §9；§19 Windows stdio |
| pending registry | 响应到达、取消、关闭、重连四类路径均移除条目；无泄漏、无重复完成 | `tugmcp_integration_tests` | §16；§19 Reconnect |
| slow client | 有界队列不阻塞 writer；超过断连阈值后关闭流；服务端内存不随慢消费者无界增长 | `tugmcp_slow_tests` | §16；§19 Backpressure |
| 超大消息 | `maxMessageBytes` 边界内接受、边界外拒绝；拒绝发生在协议解析前；无部分消息残留 | `tugmcp_slow_tests` | §7.4；§8；§19 Malformed |
| 安全 hook 失败路径 | 认证失败 → HTTP 401；授权拒绝；审批拒绝/超时/取消且保留 `reason`；审计 hook 异常被**收敛**（containment），经 `IMessageSink::onError` 上报 `ErrorCode::InternalError` + `data.reason="audit_failed"`，请求继续处理；不存在绕过 hook 的路径 | `tugmcp_integration_tests` | D10；§18 |
| 真实 executor | 默认线程池调度；并发取消；饱和队列返回明确结果；重复 shutdown 幂等；handler 抛异常不穿线程边界 | `tugmcp_executor_tests` | §16；§22.1 M5 之外的库内契约 |

> 仍**未实现**。清单是验收目标，不是现状描述；`归属 target` 与 §4.2 一致。

## 6. 测试数据与 fixtures

### 6.1 helpers 职责与边界

| fixture | 职责 | 边界（不做什么） |
|---|---|---|
| `FakeTransport` | 内存双端队列；按脚本注入乱序/重复/超大/断连；记录发送序列 | 不解析 MCP 语义，不模拟 HTTP/stdio 的字节级 framing；不复制真实 transport 的重连逻辑 |
| `FakeExecutor` | 可配置队列容量、满队列行为、优先级顺序、延迟投递（由测试显式触发） | 不实现宿主执行器策略；不做真实线程池调度承诺 |
| `FakeClock` | 通过 `ITimeSource` 注入（`ServerOptions`/`ClientOptions`/HTTP options 的 `timeSource` 字段）手动推进单调时钟；驱动超时/重试/ttl 判定 | 不改变生产时钟 API；不绕过 `timeSource->now()`；不复制超时逻辑 |
| `FakeSchemaValidator` | 可脚本化的通过/失败/计数；断言调用次数与调用顺序 | 不实现 JSON Schema 语义，不替代 jsoncons/Valijson 的真实校验测试 |
| `FakeAuthenticator` | 按 header/场景返回固定 `AuthContext` 或认证失败 | 不实现真实凭据校验；不生成 token；不绕过 401 路径 |
| `FakeAuthorizer` | 可配置 allow/deny 与拒绝 reason；记录授权调用 | 不实现角色模型；不放行未显式允许的请求 |
| `FakeApprovalHook` | 可脚本化 approve/deny/timeout/cancel；记录审批请求与 reason | 不实现业务审批策略；不把 `confirmDangerous` 当审批结果 |
| `FakeAuditHook` | 记录审计事件；可注入抛错以验证异常 containment（不穿透、上报 `audit_failed`） | 不持久化、不替代宿主审计；不在 hook 内做业务判定；实现不得向库外抛异常 |
| `CountingListener` | 仅测试用：bind 一个 loopback 端口，在测试窗口内统计 `accept(2)`/`poll` 命中次数 | 不实现生产监听/协议；不终结 TLS；只用于证明“未发生连接尝试” |
| `TempDir` | 每测试独立临时目录，析构清理；路径不跨测试复用 | 不用于生产路径；不写仓库内文件；不存放凭据 |

通用边界：fixture **不得复制库内部实现**（否则测试与实现同错）。fixture 只提供“可控输入/可观测输出”，协议行为仍由被测代码产生。

> `timeSource` 以 **transport options 为准**（见主设计）：同一对象树中 transport options 的 `timeSource` 优先；`ServerOptions`/`ClientOptions` 的字段仅在对应 transport options 为 `nullptr` 时生效。本文的 `FakeClock` 注入按此优先级执行。

### 6.2 TLS 测试材料、reason 矩阵与字段覆盖

**生成**：`tests/fixtures/tls/generate.sh` 只用于生成测试材料——测试专用 CA → server leaf（SAN 同时包含 `localhost` 与 `127.0.0.1`）→ client leaf（供 mTLS）。生成脚本与产物一并入库，保证离线可复现；**禁止用于生产**，不得包含真实 CA、真实域名或真实凭据。

**CI 校验**（材料过期/错配即失败，不靠人工目检）：

```bash
# 草图，未编译：仅示意 CI 校验步骤
openssl x509 -in tests/fixtures/tls/server.pem -noout -checkend 0
openssl x509 -in tests/fixtures/tls/server.pem -noout -text | grep -E "DNS:localhost|IP Address:127\.0\.0\.1"
openssl x509 -in tests/fixtures/tls/server.pem -noout -subject | grep -E "CN *= *(localhost|tugmcp-test)"
openssl x509 -in tests/fixtures/tls/server.pem -noout -modulus | openssl sha256
openssl rsa  -in tests/fixtures/tls/server.key  -noout -modulus | openssl sha256   # 与上一行一致
```

**reason × 构建状态 × 角色/观测点**（唯一口径；不再用单一数量概括 TLS-off 失败）：

| reason | TLS-off | TLS-on | 角色/观测点 |
|---|---|---|---|
| `tls_not_built` | ✅ 创建期 | — | server/client `enabled=true` + 未编入 |
| `unsupported_scheme` | ✅ 创建期 | — | client `https://` + 未编入，accept 次数 = 0 |
| `tls_not_configured` | — | ✅ 创建期 | server `enabled=true` 缺 cert/key |
| `option_invalid` | ✅ | ✅ | 路径非法/字段互斥/`minVersion` 非法 |
| `tls_handshake_failed` | — | ✅ 运行时 | client 首请求 `Result`；server 连接级 `onError` |

`TlsOptions` **9 字段覆盖矩阵**：

| 字段 | 覆盖用例 | 断言点 |
|---|---|---|
| `enabled` | on/off；off 且 `https://` | 未编入时 `tls_not_built`/`unsupported_scheme`，创建期失败 |
| `certChainPath` | 正确链/缺失/不可读 | server 创建成功或 `tls_not_configured`/`option_invalid` |
| `privateKeyPath` | 匹配/不匹配/缺失 | 与证书匹配才允许启动 |
| `privateKeyPassword` | 加密私钥 + 正确/错误口令 | 正确可加载；错误 → `option_invalid` |
| `clientCaFile` | server 要求 mTLS | 仅接受由该 CA 签发的 client leaf |
| `verifyPeer` | true/false | true 时校验对端；false 明确只用于测试 |
| `caBundlePath` | 可信 CA/未信任 CA | 可信通过；未信任 → `tls_handshake_failed` |
| `serverName` | 匹配/不匹配主机名 | 不匹配主机名握手失败 |
| `minVersion` | 合法下限/非法值/高于对端 | 非法值创建期失败；版本不匹配握手失败 |

**负例**：错误主机名、过期证书、未信任 CA、mTLS 缺客户端证书、`minVersion` 非法/过高。每个负例都断言具体 `reason` 与观测点，不笼统断言“失败”。

### 6.3 数据文件位置与命名

| 类型 | 位置 | 命名 |
|---|---|---|
| 测试数据 | `tests/**/data/` | `<层>_<主题>_<情形>.json`，如 `unit_strict_json_duplicate_keys.json` |
| 协议 vectors | `tests/conformance/vectors/{2026,2025}/` | `<method>_<direction>_<case>.json`，如 `tools_call_server_error.json` |
| TLS 材料 | `tests/fixtures/tls/` | `ca.pem`/`ca.key`/`server.pem`/`server.key`/`client.pem`/`client.key`/`bad_*.pem` |

- 数据文件不含凭据、个人信息或真实端点。
- 同一向量只存一份；跨层复用通过路径引用，不复制。
- 测试通过 `TUGMCP_TEST_DATA_DIR` 定位数据，不依赖进程当前目录猜测。

## 7. 确定性与卫生

| 规则 | 说明 |
|---|---|
| 禁止 `sleep_for` 同步 | 用条件变量、事件/闩锁或 `FakeClock` 等待状态；超时只作为失败兜底，不作为同步手段 |
| 单测超时 | 单条测试 ≤ 2s；超过即视为设计问题（死等/无界重试），不用更大超时掩盖 |
| 网络限制 | 只允许 loopback；禁止公网、外部 DNS、真实服务；端口由系统分配，避免固定端口冲突 |
| 临时目录隔离 | `TempDir` 按测试隔离并在析构清理；失败时保留路径供诊断，但不跨用例复用 |
| 无共享可变状态 | 不用全局单例/静态缓存传递测试状态；并发用例各自持有对象 |
| 无 flaky 重试 | 不允许用“重跑直到通过”或 CTest 重复运行作为门禁；失败必须可复现，否则修复根因 |
| 无静默跳过 | 用例/平台不支持必须显式列出并链接设计条目；禁止 `SKIP` 掉已知失败或从套件删除 |
| 政策项只写约定 | 无法机验的流程约束留在本节，不放进 §10 验收清单；§10 只保留可执行、可判定的项 |
| 失败信息格式 | 至少包含：断言/用例名、`reason`、期望值、实际值、相关设计条目（§号或 D 编号） |

## 8. CI 与门禁

### 8.1 PR 快线与夜线拆分

**PR 快线（3 个 job，只跑编译模式可确定的部分）**

| job | 编译器/配置 | 关键开关 | 说明 |
|---|---|---|---|
| 1 | GCC Debug | `TUGMCP_TLS=OFF`，编译模式 | 主快线，覆盖 unit/integration 与 ODR |
| 2 | MSVC 2022 x64 Debug | `TUGMCP_TLS=OFF`，编译模式 | Windows stdio/链接差异 |
| 3 | GCC Debug | `TUGMCP_HEADER_ONLY=ON`，`TUGMCP_TLS=OFF` | 验证 INTERFACE 消费与 ODR |

快线命令固定为 `ctest -L "unit|integration" -LE "slow|tls|win-only"`，**不得包含 `tls`/`slow` 可执行**；CI step timeout 120s，目标 < 60s。

**夜线（全矩阵 + 扩展检查）**

- 编译器 × 开关全矩阵：MSVC 2022 x64 / GCC 11 / Clang 15（AppleClang best-effort）× `TUGMCP_TLS` on/off × `TUGMCP_HEADER_ONLY` on/off × Debug/Release。
- `TUGMCP_USE_SYSTEM_DEPS=ON` 至少 1 个 job。
- conformance（外部 runner）、`tls`/`slow`/`win-only`、sanitizers（GCC/Clang ASAN+UBSAN；Linux TSAN）、perf/soak。
- 夜线失败必须可定位到 job/构建组合；不得用一个 job 代表全部组合。

### 8.2 命令

```bash
# 草图，未编译：命令口径
# PR 快线（3 个 job 各自执行；step timeout 120s，目标 <60s）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUGMCP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure -L "unit|integration" -LE "slow|tls|win-only"

# 夜线：TLS/slow/win-only
ctest --test-dir build --output-on-failure -L "tls|slow|win-only"

# 夜线：conformance（conformance job configure 时加 -DTUGMCP_CONFORMANCE=ON）
ctest --test-dir build --output-on-failure -L conformance

# 夜线：ASAN+UBSAN（GCC/Clang）
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure -L "unit|integration" -LE "slow"

# 夜线：TSAN（Linux；GCC/Clang）
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure -R "executor|integration"
```

- 快线只跑 `unit|integration` 且排除 `slow|tls|win-only`，目标是 < 60s；`tls`/`slow`/`conformance` 留在夜线，避免 PR 反馈被长尾拖慢。
- `TUGMCP_BUILD_TESTS=OFF` 时只验证库可构建，不跑测试。

### 8.3 覆盖率口径

- GCC/Clang 用 `--coverage`；MSVC 用 OpenCppCoverage（可选）。
- 报告口径：`tugmcp::core` 行覆盖目标 ≥ 80%，协议 adapter ≥ 70%；v1 不作硬门禁，只做趋势与评审依据。
- 覆盖率不替代必测清单；未覆盖的关键分支不得以行覆盖数字抵扣。

### 8.4 Conformance

- 仅在 `TUGMCP_CONFORMANCE=ON` 时注册 `conformance.external`；默认 OFF，不把外部套件强加给普通构建。
- 官方套件 `@modelcontextprotocol/conformance@0.2.0-alpha.12` 的 `server` 子命令**只支持 `--url`（HTTP/SSE），不支持 stdio command**。stdio 外部验证用 MCP Inspector CLI（`--cli`），不冒充 conformance 计分。
- 依赖测试侧 fixture server `tugmcp_conformance_server`（`tests/conformance/`，HTTP，仅测试资产），实现官方场景期望的 diagnostic tools/resources/prompts；**不得用生产 `echo_server` 充数**。
- `tests/conformance/run_external.py` 是外部 runner 的唯一入口：启动 fixture server（`127.0.0.1:0`）→ 运行
  `server --url http://127.0.0.1:<port>/mcp --requirements 2026-07-28`，再跑 `2025-11-25` → 断言**必需场景真实失败为 0**；extension/pending 用 `tests/conformance/expected_failures.yaml` 显式登记。
- 固定 `@modelcontextprotocol/conformance@0.2.0-alpha.12`；缺 Node/npx/Python3 → `FATAL_ERROR`，不得静默降级为“只跑本地 vectors”。
- 结果写 `${CMAKE_CURRENT_BINARY_DIR}/conformance/<revision>/`（源码树只保留 `expected_failures.yaml`）；`ctest -L conformance` 必须调用 `run_external.py`。
- `expected_failures.yaml` 中登记的用例一旦重新通过，runner 必须**失败**（防止静默接受回归）；未登记失败一律失败。
- fixture server 启动后必须在 stderr 打印实际 bound port；runner 等到端口可连接后再跑套件（不得固定 sleep）。
- Node/npx 版本固定（Inspector 2.12 需要 Node ≥22.19；conformance 0.2.0-alpha.12）；离线或包不可得 → `FATAL_ERROR`，不得跳过。
- 本地 `tugmcp_conformance_tests` 只是补充，不替代外部套件。
- 示例调用（草图，未编译）：

```bash
# 草图：HTTP-only 官方 server 模式；stdio 用 Inspector 另行冒烟
python tests/conformance/run_external.py \
  --fixture-server build/tests/tugmcp_conformance_server \
  --revision 2026-07-28 \
  --package @modelcontextprotocol/conformance@0.2.0-alpha.12 \
  --expected-failures tests/conformance/expected_failures.yaml \
  --out build/conformance/2026-07-28
```

### 8.5 覆盖矩阵与 CI 构建矩阵

主设计 §19 的覆盖矩阵（8 个分层/维度口径）回答“**测什么**”；本文 §8.1 的 CI 构建矩阵回答“**在哪些构建组合下执行**”。两者不是同一张表：§19 不规定编译器/开关组合，§8.1 不重新定义必测内容。

| 覆盖维度（§19） | 本文落点 |
|---|---|
| 分层（Unit/Fake/Protocol/Loopback/Negative/ODR/Conformance/Performance） | §3、§5 |
| Era（2026/2025/legacy） | §5 生命周期/legacy、§9 M2–M4/M6 |
| Transport（stdio/Streamable HTTP/legacy） | §5 stdio/HTTP/legacy、§4.2 target |
| Role（server/client/双向 loopback） | §5、§9 |
| Failure（断连/超时/取消/队列满/慢客户端/非法 JSON） | §5、§6.2 |
| Schema（2020-12/draft-07/无 validator） | §5 Schema、§4.4 条件 target |
| 安全与限制（auth/审批/审计/Origin/Host/body/stream） | §5 安全 hook、Security limits |
| 编译器/开关/配置（构建矩阵，非覆盖内容） | §8.1、§8.2 |

## 9. 里程碑映射

| 里程碑 | 必须落地的测试集 | 验收信号 |
|---|---|---|
| **M1** core + stdio | Unit：codec/RequestId/错误模型/registry；integration：stdio echo loopback、非法 JSON/EOF、断连、stdout 纯净；`tugmcp_odr_tests`；`tugmcp_executor_tests` 的默认线程池/重复 shutdown 基础用例 | 默认配置构建 + 哈希校验通过；echo tool 往返；双 TU 链接通过 |
| **M1.5** schema/canonical/conformance harness | Unit：canonical JCS 子集/`tool_schema_hash`、schema adapter、`FakeSchemaValidator`；`tugmcp_schema_tests`（按开关）；vectors 骨架；`run_external.py` 可跑 | 2020-12/draft-07 矩阵；hash 稳定；conformance 0.2.0-alpha.12 + 自定义 vectors 可运行 |
| **M2** 2026 server+client | integration：MRTR 多轮/篡改 state、templates 分页/cache、订阅 ack/filter、`CloseStream` cancel、header/`_meta` mismatch、`resultType`/`ttlMs`/`cacheScope`；`tugmcp_tls_tests` 覆盖 reason 矩阵与 9 字段；`tugmcp_slow_tests` 覆盖慢客户端/超大消息 | 2026 server↔client loopback 全覆盖；TLS-off 的 `https://`/`enabled=true` 创建期失败且 accept 次数 = 0；TLS-on 冒烟 |
| **M3** 2025 server/session | integration：initialize/session、POST/GET/DELETE、server→client requests（roots/sampling/elicitation）能力 gate、per-stream replay | Inspector 2025 冒烟；session/replay 用例通过 |
| **M4** 2025 client/replay | integration：2025 client initialize/session、pending registry、progress/notification、`Last-Event-ID` replay、server request handlers、cancel、重连 pending 清理 | 与 M3 双向 loopback；超时/取消/replay 用例通过 |
| **M6** legacy shim | integration（`TUGMCP_ENABLE_LEGACY_SSE=ON` 构建）：`/sse` + `/messages` 兼容；默认关闭；可选 batch 兼容 | 仅兼容客户端；默认构建不启用，不进入主验收 |

> M5（宿主适配）不属于本库测试范围：执行器策略、审批/审计、业务语义由宿主验收；库只测 hook 的调用契约与失败路径。
>
> 当前所有里程碑测试**未实现**。

## 10. 验收标准

| 验收项 | 验证方式（target + 命令 + 期望） |
|---|---|
| 双 TU 无重复符号 | `tugmcp_odr_tests`；`ctest --test-dir build -R unit.odr_link --output-on-failure`；退出码 0，无 duplicate symbol |
| header-only/编译模式同测试集一致 | 分别以 `TUGMCP_HEADER_ONLY=ON/OFF` 构建；`ctest --test-dir build -L "unit|integration" -LE "slow|tls|win-only"`；两次全绿且用例数一致（`ctest -N` 对比） |
| TLS reason 矩阵 | `tugmcp_tls_tests`；`ctest --test-dir build -L tls --output-on-failure`；5 行 reason 各自命中预期观测点。TLS-off 的 `tls_not_built`/`unsupported_scheme` 用 `CountingListener` bind loopback 端口：断言 **accept 次数 = 0** 且 **listen 队列为空/无 pending 连接**（仅 accept=0 不足以排除内核已完成握手），证明未发生连接尝试 |
| 宏守卫负例 | configure 阶段 `try_compile`；正/负例均 `LINK_LIBRARIES tugmcp::http tugmcp::core`；正例（`TUGMCP_TLS_ENABLED=1` + `CPPHTTPLIB_OPENSSL_SUPPORT`）通过，负例（`TUGMCP_TLS_ENABLED=0` + `CPPHTTPLIB_OPENSSL_SUPPORT`）失败且 `OUTPUT_VARIABLE` 含 `TUGMCP_TLS_ENABLED`（排除缺 include） |
| conformance 真实接线 | 以 `TUGMCP_CONFORMANCE=ON` 配置；`ctest --test-dir build -L conformance --output-on-failure`；必须执行 `tests/conformance/run_external.py`，退出码 0 且 `expected_cases.json` 清单齐全（版本 `0.2.0-alpha.12`） |
| 快线时限 | `ctest --test-dir build -L "unit|integration" -LE "slow|tls|win-only"`；CI step timeout 120s，实测 < 60s（Debug，参考机型） |
| 覆盖率口径 | 夜线 `ctest --test-dir build --coverage`（或 gcovr 报告）；`tugmcp::core` 行覆盖 ≥ 80%，adapter ≥ 70%；v1 非硬门禁，但报告必须生成 |
| 单测超时生效 | `ctest --test-dir build --show-only=json-v1`；unit/integration/odr 的 `TIMEOUT` 为 2，TLS/executor 为 10，slow 为 30 |
| Windows stdio 独立 | Windows 构建后 `ctest --test-dir build -L win-only --output-on-failure`；`tugmcp_win_stdio_tests` 全绿，且该子集不在 `tugmcp_integration_tests` |
| `tugmcp::tests` 仅 helper | CMake 配置脚本读取 target 类型；期望 `INTERFACE_LIBRARY`，且不产生编译产物 |
| executor 并发正确性 | `tugmcp_executor_tests`；夜线 ASAN/UBSAN 与 Linux TSAN 下 `ctest -R executor`；无泄漏、无 data race、重复 shutdown 不崩 |

> 以上为设计验收清单，当前**均未实现**。纯政策项（禁止 flaky 重试、无静默跳过、失败可复现）已前移到 §7，不占验收表。
