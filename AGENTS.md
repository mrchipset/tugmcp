# AGENTS.md — tugmcp（拖船 MCP）

> 本文件面向在本仓库工作的开发者与 AI agent。先读本文件，再读 `designs/`。
>
> 状态：**设计阶段**。仓库目前只有设计文档，没有可构建代码。

## 1. 项目是什么

`tugmcp`（拖船 MCP）是一个**独立、可复用、无 GUI 依赖的纯 C++17 MCP 库**，同时提供：

- **MCP server** 和 **MCP client**；
- **stdio** 与 **Streamable HTTP** 传输；
- 协议 **2026-07-28 主**、**2025-11-25 回退**；
- 可选 legacy HTTP+SSE（2024-11-05）适配，默认关闭；
- 可插拔 schema 校验、auth/approval/audit hooks、canonical JSON、conformance 测试。

目标：让宿主应用以**静态库或 header-only**方式嵌入 MCP 能力，而不把设备/业务逻辑带进库。

## 2. 非目标

- 不实现设备控制、业务执行、参数调优、任务语义、硬件联锁。
- 不实现 GUI/UI 框架集成（UI 由宿主自带；库不绑定任何 GUI 工具包）。
- 不实现 OAuth/OIDC 授权服务器；只提供 auth hook。
- 不把 MCP 做成 REST/OpenAPI。
- 不默认开启 legacy HTTP+SSE。
- 不声称库本身能强制设备安全；安全必须由应用/硬件层保证。

## 3. 仓库布局（目标）

```text
tugmcp/
  AGENTS.md
  README.md
  designs/
    README.md
    mcp-library-design.md      # 主设计
    host-adapter.md            # 宿主应用如何消费本库
    protocol-decisions.md      # ADR/关键决策
    header-only.md             # header-only 可行性与工量
    testing.md                 # 测试分层/框架/目录与 target/CI
  include/tugmcp/
    core.hpp
    server.hpp
    client.hpp
    transport/stdio.hpp
    transport/http.hpp
    detail/http_httplib.hpp    # header-only 模式唯一 include cpp-httplib 的实现头
    protocol/v2026.hpp
    protocol/v2025.hpp
    schema/{jsoncons,valijson,null}.hpp
  src/                         # 仅编译模式需要
  tests/
  examples/
  CMakeLists.txt
  cmake/doctest.cmake          # doctest 的 CMake 模块（固定副本，MIT）
  LICENSES/                    # 聚合许可证（REUSE 风格）
  3rd/                         # vendored 第三方源码 + INTERFACE target
```

> 不要在没有设计变更记录的情况下新增顶层目录或公共头文件。

## 4. 权威文档

| 文档 | 作用 |
|---|---|
| `designs/mcp-library-design.md` | 库的分层、API、协议、传输、schema、测试、打包 |
| `designs/host-adapter.md` | 宿主应用适配边界：哪些在库、哪些在宿主 |
| `designs/protocol-decisions.md` | 已定决策与理由，改决策必须先改这里 |
| `designs/header-only.md` | header-only vs 编译库的取舍与工量 |
| `designs/testing.md` | 测试分层、框架、目录/target、CI 与覆盖率口径 |

## 5. 架构不变量

1. **core 不依赖 GUI 框架、cpp-httplib、宿主应用类型**。GUI/HTTP 只出现在可选 target/header。
2. **传输只搬消息**：`ITransport` 不懂 MCP method；MCP 语义在 `IProtocolAdapter`/server/client。
3. **协议代际隔离**：2026 与 2025 的 lifecycle/headers/session 行为放在各自 adapter；core 不写散落的 `if (version == ...)`。
4. **error-as-value**：协议/传输错误用 `Result<T, McpError>`/`Status`，异常不得穿过 transport/线程边界。
5. **显式所有权**：server/client 拥有 transport/executor；sink 非拥有；停止后不再回调。
6. **线程边界**：每个 transport 一个 reader/writer；handler 在 `IExecutor`；不共享可变 JSON。
7. **安全边界**：认证、授权、审批、审计、硬件联锁由应用提供；库只提供 hooks/context。
8. **协议安全默认**：HTTP 默认 bind `127.0.0.1`；legacy SSE 默认关闭；schema 未配置时 fail closed。

## 6. C++ 约定

- C++17；命名空间 `mcp::v1`；公共 API 用稳定值类型 + pimpl/impl。
- 不使用 C++20 特性（C++17 下限；MSVC 2019 16.8+ / GCC 9+ / Clang 10+）。
- `nlohmann/json` 作为 wire JSON；公共头暴露 `Json` 时接受 ABI/版本约束。
- 依赖固定版本：`nlohmann/json 3.12.0`；`cpp-httplib` 固定 commit + SHA256（当前稳定 v0.63.0，v0.62.0 作 fallback）。
- 第三方依赖统一放 `3rd/`，经 INTERFACE target（SYSTEM include）暴露；版本/commit/SHA256 与许可证登记在 `3rd/README.md`；默认不使用系统包（`TUGMCP_USE_SYSTEM_DEPS=OFF`）与 FetchContent。
- 不引入全局可变状态；常量用 `inline constexpr`。
- 公共头不得 include 宿主应用/GUI 框架头；示例和测试可以。
- 公共头只允许 include `nlohmann/json`（`Json` 暴露）与对应可选 schema target 的第三方头；`core/server/client/transport/http.hpp` 永不 include cpp-httplib；`transport/http.hpp` 不随 TLS/构建模式变化（`TlsOptions` 字段无条件存在）；header-only 实现只在 `detail/http_httplib.hpp`。
- 目录 `include/tugmcp/` 是发行名，命名空间 `mcp::v1` 是符号域，二者不同是有意的。
- 错误码：JSON-RPC `-32700/-32600/-32601/-32602/-32603`；MCP `-32020/-32021/-32022`；认证失败走 HTTP 401，不用 `-32021`。
- 工具结果：`isError` 表示工具执行失败；协议错误用 JSON-RPC error，不混用。

## 7. Header-only 策略

**默认构建产物为静态库；同时支持以 header-only 方式消费（`TUGMCP_HEADER_ONLY=ON` 显式开启）。**

- 核心/协议/stdio 可 header-only；`http` 相关实现隔离在 `detail/http_httplib.hpp`，避免 core 编译膨胀。
- 所有非模板自由函数用 `inline`；常量/变量用 `inline constexpr`；禁止在头文件定义非 inline 全局对象。
- 单头文件 `tugmcp.hpp` 只作为**生成的 amalgamation**，不要手写维护。
- 必须通过“两个 TU 同时 include”的 ODR/链接测试；Windows 需链接 `ws2_32`（启用 TLS 时追加 `crypt32`）；线程依赖用 `Threads::Threads`。
- `tugmcp::tests` 是 **INTERFACE helper 库**（fake transport / conformance helpers / fixture 头，INTERFACE 传播 `tugmcp_3rd_doctest`）；测试可执行文件（`tugmcp_unit_tests` 等）是独立 target，**始终编译**，不受 `TUGMCP_HEADER_ONLY` 影响。
- `TUGMCP_IMPLEMENTATION` 是 **header-only / amalgamation 场景的单 TU 实现宏**，不是编译模式开关。
- 详细取舍见 `designs/header-only.md`。

## 8. 协议规则

- 2026-07-28：stateless；`server/discover`；每请求 `_meta` + `MCP-Protocol-Version`/`Mcp-Method`/`Mcp-Name`；`resultType`；`ttlMs/cacheScope`；`subscriptions/listen` + ack；MRTR `input_required`/`requestState`/`inputResponses`；无 session/GET/Last-Event-ID/ping。
- 2025-11-25：`initialize`/`initialized`；`MCP-Session-Id`；GET/DELETE；per-stream `Last-Event-ID`；server→client 请求按 capability。
- 2025-06-18 不单列 target；如需兼容，走 2025 adapter 的显式 compat flag 并加测试。
- JSON-RPC batch 在 2025-06-18 起移除；core 拒绝 batch；legacy 2024 adapter 可单独兼容。
- 旧 HTTP+SSE（`GET /sse` + `POST /messages`）只作为 legacy adapter，默认关闭。
- 不实现 REST/OpenAPI；MCP 是 JSON-RPC。

## 9. 构建与测试（待实现后生效）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# TLS 默认 OFF；需要时显式开启：
cmake -S . -B build -DTUGMCP_TLS=ON

# 快线（PR）与夜线（nightly）：
ctest --test-dir build --output-on-failure -L "unit|integration"
ctest --test-dir build -L conformance
```

测试分层与 target 见 `designs/testing.md`；测试可执行文件始终编译。

- 目标：`tugmcp::core`（无 GUI/HTTP 依赖）、`tugmcp::http`、`tugmcp::schema_jsoncons`、`tugmcp::schema_valijson`、`tugmcp::tests`。**库 target 默认 STATIC**，`TUGMCP_HEADER_ONLY=ON` 时改为 INTERFACE（测试可执行文件仍编译）。
- 依赖归属：`tugmcp::core` 需 `Threads::Threads`（PUBLIC，configure 需 `find_package(Threads REQUIRED)`）并 INTERFACE 传播 `tugmcp_3rd_json`；`tugmcp::http` 需 `ws2_32`（TLS 时 `crypt32`）、TLS 时 `OpenSSL::SSL`/`OpenSSL::Crypto`，`tugmcp_3rd_httplib` 编译模式 PRIVATE、header-only INTERFACE；`tugmcp::schema_jsoncons`/`schema_valijson` 需对应 3rd target 的 INTERFACE include（宏 PRIVATE）；编译模式这些依赖为 PRIVATE（静态库经 `$<LINK_ONLY:...>` 传递给最终链接），header-only 模式为 INTERFACE。
- Windows：`ws2_32`（TLS 时追加 `crypt32`）；TLS 由 `TUGMCP_TLS` 显式开启（默认 OFF），要求 OpenSSL ≥ 3.0，不支持 1.1.1；同一进程只允许一套 OpenSSL。
- 协议验收：MCP Inspector + `@modelcontextprotocol/conformance`（先验证 0.2.0-alpha.12 对 2026 的覆盖）+ 自定义 2026 vectors。
- 最低测试矩阵：stdio/HTTP、server/client、2026/2025、malformed JSON/UTF-8/int64/duplicate keys、cancel/progress/subscriptions/MRTR、slow client、shutdown。

## 10. 贡献流程

1. **先改设计**：API/协议/传输变更先更新 `designs/`，再改代码。
2. 新增/修改公共 API：补类型、错误语义、线程归属、测试；不要只加实现。新增公共 API 必须同时补对应测试层用例（见 `testing.md` §5）。
3. 协议变更：更新 `protocol-decisions.md` + conformance 用例；不要静默改变 wire 行为。
4. 安全相关：auth/approval/audit hook 的失败路径必须有测试；不要引入“库保证安全”的表述。
5. 依赖变更：固定版本/commit/SHA；说明 MSVC/GCC/Clang 影响。
6. 提交前：构建、单测、loopback、conformance 冒烟；不留 TODO 在公共 API。

## 11. 里程碑

按 `designs/mcp-library-design.md` §22.1：

`M1 core+stdio` → `M1.5 schema/canonical+conformance` → `M2 2026 server+client` → `M3 2025 server/session` → `M4 2025 client/replay` → `M5 宿主适配` → `M6 legacy`。

## 12. 与宿主应用的关系

- 宿主应用是**消费者**，不是库的一部分。
- 设备控制/UI 调度/任务管理/硬件联锁/审批留在宿主适配层。
- 库只提供 `RequestContext`、cancellation、progress、transport、protocol、schema/auth hooks。
- 宿主应用以静态库或 header-only 方式链接；不得让客户端启动第二个宿主进程（设备/单实例争用）。

## 13. 第三方依赖与构建开关

依赖只从 `3rd/`（vendored 源码 + 手写 INTERFACE target）或显式 `TUGMCP_USE_SYSTEM_DEPS=ON` 获取；两者互斥，不得混用。system 模式是**显式二选一**，不是自动回退：找不到包即 configure 失败，不回退 vendored。版本/commit/SHA256 与许可证登记在 `3rd/README.md`，聚合许可证放 `LICENSES/`。

| 开关 | 默认/作用 |
|---|---|
| `TUGMCP_TLS` | OFF；是否启用 TLS |
| `TUGMCP_TLS_BACKEND` | `openssl`；TLS 后端（当前仅 OpenSSL ≥ 3.0） |
| `TUGMCP_USE_SYSTEM_DEPS` | OFF；改用系统包而非 `3rd/` |
| `TUGMCP_VERIFY_3RD_HASHES` | ON；校验 `3rd/` 依赖的 SHA256（system 模式忽略） |
| `TUGMCP_BUILD_TESTS` | ON；构建测试 target |
| `TUGMCP_WITH_HTTP` | ON；构建 `tugmcp::http` |
| `TUGMCP_WITH_JSONCONS` | OFF；构建 `tugmcp::schema_jsoncons` |
| `TUGMCP_WITH_VALIJSON` | OFF；构建 `tugmcp::schema_valijson` |
| `TUGMCP_ENABLE_LEGACY_SSE` | OFF；启用 legacy HTTP+SSE 适配 |
| `TUGMCP_HEADER_ONLY` | OFF；**库** target 改为 INTERFACE（不编译 `src/`；测试可执行文件仍编译） |
| `TUGMCP_BUILD_EXAMPLES` | ON；构建 `examples/`（`tugmcp_echo_server` 等） |
| `TUGMCP_CONFORMANCE` | OFF；运行外部 conformance 套件（需 Python3、`TUGMCP_WITH_HTTP=ON`、`TUGMCP_BUILD_TESTS=ON`、`TUGMCP_BUILD_EXAMPLES=ON` 且存在 `tugmcp_echo_server`） |

`CPPHTTPLIB_OPENSSL_SUPPORT` 与 `TUGMCP_TLS_ENABLED` 只由 `tugmcp::http` target 传播：编译模式为 PRIVATE（仅作用于 `src/transport/`），header-only 模式为 INTERFACE；禁止 `add_definitions` 全局注入；同一进程所有 TU 必须一致。

`TUGMCP_VERIFY_3RD_HASHES` 在 `TUGMCP_USE_SYSTEM_DEPS=ON` 时忽略（system 模式完全不引入 `3rd/`）。

`SYSTEM` include 在 MSVC 上映射为 `/external:I` 需要 CMake ≥3.24；本库最低 3.20，因此在 MSVC 上为 best-effort，第三方告警抑制不保证。
