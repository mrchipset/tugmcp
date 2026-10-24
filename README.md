# tugmcp（拖船 MCP）

`tugmcp` 是一个**独立、可复用、无 GUI 依赖的纯 C++17 MCP 库**，同时提供 MCP server 与 client，支持 stdio 和 Streamable HTTP，协议以 **2026-07-28** 为主、**2025-11-25** 回退。

名字取“拖船”：库本身不做具体业务，只负责把宿主应用的 MCP 能力“拖”到 stdio/HTTP 上，并把协议、传输、schema、并发和打包问题收进一个可复用组件。

## 状态

**设计阶段**。当前仓库只有设计文档，没有可构建代码。

## 文档

| 文档 | 内容 |
|---|---|
| [`AGENTS.md`](./AGENTS.md) | 开发者/AI agent 的工作约定、架构不变量、构建与测试规则 |
| [`designs/mcp-library-design.md`](./designs/mcp-library-design.md) | 主设计：分层、API、协议、传输、schema、并发、安全、打包、里程碑 |
| [`designs/host-adapter.md`](./designs/host-adapter.md) | 宿主应用如何消费本库：进程内、执行器、安全/审批、工具映射 |
| [`designs/protocol-decisions.md`](./designs/protocol-decisions.md) | 已定协议/传输/依赖决策（ADR） |
| [`designs/header-only.md`](./designs/header-only.md) | header-only 可行性、取舍与工量评估 |
| [`designs/testing.md`](./designs/testing.md) | 测试分层、框架选型（doctest）、目录/target、CI 矩阵 |
| [`docs/roadmap.md`](./docs/roadmap.md) | 里程碑完成度、设计章节落地状态与下一步优先级 |

## 目标布局

```text
tugmcp/
  AGENTS.md
  README.md
  designs/             # 设计文档（含 testing.md 测试设计）
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
  src/                 # 仅编译模式需要
  tests/
  examples/
  CMakeLists.txt
  cmake/doctest.cmake  # doctest 的 CMake 模块（固定副本，MIT）
  LICENSES/            # 聚合许可证（REUSE 风格）
  3rd/                 # vendored 第三方源码 + INTERFACE target
```

默认构建产物为静态库；同时支持以 header-only 方式消费（`TUGMCP_HEADER_ONLY=ON` 显式开启）。

## 不是这个库负责的事

- 设备控制、业务执行、参数调优、任务语义、硬件联锁；
- GUI/UI 框架集成；
- OAuth/OIDC 授权服务器；
- REST/OpenAPI 网关；
- 设备安全结论。

这些属于宿主应用或硬件层。库只提供协议、传输、编解码、schema 校验、取消/进度、auth/approval/audit hooks。

## 快速开始

代码尚未实现。实现顺序见 `designs/mcp-library-design.md` §22.1：

`M1 core+stdio` → `M1.5 schema/canonical+conformance` → `M2 2026 server+client` → `M3 2025 server/session` → `M4 2025 client/replay` → `M5 宿主适配` → `M6 legacy`。

## 构建开关

| 开关 | 默认 | 说明 |
|---|---|---|
| `TUGMCP_TLS` | OFF | 是否启用 TLS |
| `TUGMCP_TLS_BACKEND` | `openssl` | TLS 后端（当前仅 OpenSSL ≥ 3.0） |
| `TUGMCP_USE_SYSTEM_DEPS` | OFF | 改用系统包而非 `3rd/` |
| `TUGMCP_VERIFY_3RD_HASHES` | ON | 校验 `3rd/` 依赖的 SHA256（system 模式忽略） |
| `TUGMCP_WITH_HTTP` | ON | 构建 `tugmcp::http` |
| `TUGMCP_WITH_JSONCONS` | OFF | 构建 `tugmcp::schema_jsoncons` |
| `TUGMCP_WITH_VALIJSON` | OFF | 构建 `tugmcp::schema_valijson` |
| `TUGMCP_ENABLE_LEGACY_SSE` | OFF | 启用 legacy HTTP+SSE 适配 |
| `TUGMCP_BUILD_TESTS` | ON | 构建测试 target |
| `TUGMCP_HEADER_ONLY` | OFF | **库** target 改为 INTERFACE（不编译 `src/`；测试可执行文件仍编译） |
| `TUGMCP_BUILD_EXAMPLES` | ON | 构建 `examples/`（`tugmcp_echo_server` 等） |
| `TUGMCP_CONFORMANCE` | OFF | 运行外部 conformance 套件（需 Python3、HTTP、测试与 examples 开启，且存在 `tugmcp_echo_server`） |

TLS 默认关闭；开启后需 OpenSSL ≥ 3.0（cpp-httplib 要求）与 `crypt32`（Windows），且同一进程只能有一套 OpenSSL。

依赖来源是**显式二选一**：默认用 `3rd/`；`TUGMCP_USE_SYSTEM_DEPS=ON` 时完全改用系统包，找不到即 configure 失败，不会自动回退 vendored。

## 文档权威

- 本仓库的 `designs/` 是后续开发的本地权威。
- 文档自包含，不依赖外部仓库；设计、决策与适配边界都以本目录为准。
- 实现与设计冲突时，先改设计再改代码。
