# tugmcp（拖船 MCP）

[English](README.md) | **简体中文**

`tugmcp` 是一个**独立、可复用、无 GUI 依赖的纯 C++17 MCP 库**，同时提供 MCP server 与 client，支持 stdio 和 Streamable HTTP，协议以 **2026-07-28** 为主、**2025-11-25** 回退。

名字取“拖船”：库本身不做具体业务，只负责把宿主应用的 MCP 能力“拖”到 stdio/HTTP 上，并把协议、传输、schema、并发和打包问题收进一个可复用组件。

## 状态

**M1–M6 已完成**（core+stdio、schema/canonical/conformance、2026 server/client、2025 server/session、2025 client/replay、宿主适配 hooks、legacy HTTP+SSE）。默认构建产物为静态库，同时提供**单头发布产物** `tugmcp.hpp` / `tugmcp_http.hpp` / `tugmcp_http_tls.hpp`（生成、打包、SHA256、许可证与外部验证均已落地）。split-header `TUGMCP_HEADER_ONLY` 模式仍未实现（configure 即 `FATAL_ERROR`）；单头产物不依赖该开关。详见 [`docs/amalgamation.md`](./docs/amalgamation.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [`AGENTS.md`](./AGENTS.md) | 开发者/AI agent 的工作约定、架构不变量、构建与测试规则 |
| [`designs/mcp-library-design.md`](./designs/mcp-library-design.md) | 主设计：分层、API、协议、传输、schema、并发、安全、打包、里程碑 |
| [`designs/host-adapter.md`](./designs/host-adapter.md) | 宿主应用如何消费本库：进程内、执行器、安全/审批、工具映射 |
| [`designs/protocol-decisions.md`](./designs/protocol-decisions.md) | 已定协议/传输/依赖决策（ADR） |
| [`designs/header-only.md`](./designs/header-only.md) | header-only 可行性、单头 artifact、取舍与工量评估 |
| [`designs/testing.md`](./designs/testing.md) | 测试分层、框架选型（doctest）、目录/target、CI 矩阵 |
| [`docs/api.md`](./docs/api.md) | 公共 API：类型、错误语义、server/client/transport、hooks 与扩展点 |
| [`docs/building.md`](./docs/building.md) | 前置条件、依赖策略、全部构建开关、构建/测试命令、target、测试数量与常见问题 |
| [`docs/integration.md`](./docs/integration.md) | 三种消费方式、stdio/HTTP server 与 client 嵌入、executor/关闭、安全 hooks、schema 与部署清单 |
| [`docs/amalgamation.md`](./docs/amalgamation.md) | 单头产物矩阵、消费/链接、TLS 宏/ODR、生成与发布、哈希/许可证策略 |
| [`examples/qt_widgets_server/README.md`](./examples/qt_widgets_server/README.md) | Qt 5 Widgets + Streamable HTTP 示例：GUI 线程 marshal、worker 线程进度/取消、构建/运行/测试说明 |
| [`docs/roadmap.md`](./docs/roadmap.md) | 里程碑完成度、设计章节落地状态与下一步优先级 |

## 目标布局

```text
tugmcp/
  AGENTS.md
  README.md            # 英文（GitHub 默认）
  README.zh-CN.md      # 中文（Gitee 按语言选择）
  LICENSE              # 项目许可证（MIT）
  designs/             # 设计文档（含 testing.md 测试设计）
  docs/                # API、构建、集成、单头、roadmap
  include/tugmcp/
    core.hpp / server.hpp / client.hpp
    error.hpp / message.hpp / version.hpp
    operation.hpp / tool.hpp / resource.hpp / prompt.hpp
    request_context.hpp / time_source.hpp / executor.hpp / security.hpp
    transport.hpp
    transport/stdio.hpp
    transport/http.hpp
    transport/legacy_sse.hpp      # 可选 legacy 适配，默认关闭
    detail/http_httplib.hpp       # 单头实现唯一 include cpp-httplib 的实现头
    schema.hpp / canonical.hpp
    schema/{jsoncons,valijson,null}.hpp
  src/                 # 仅编译模式需要
  tests/               # doctest 测试 + tests/amalgamation
  examples/
  tools/               # amalgamate.py / package_amalgamation.py
  CMakeLists.txt
  cmake/doctest.cmake  # doctest 的 CMake 模块（固定副本，MIT）
  LICENSES/            # 聚合许可证（REUSE 风格）
  3rd/                 # vendored 第三方源码 + INTERFACE target
```

默认构建产物为静态库；另外提供由 `tools/amalgamate.py` 生成的单头发布产物（`tugmcp.hpp`、`tugmcp_http.hpp`、`tugmcp_http_tls.hpp`）。split-header 的 `TUGMCP_HEADER_ONLY=ON` 模式尚未实现（FATAL），单头消费不需要该开关。

## 不是这个库负责的事

- 设备控制、业务执行、参数调优、任务语义、硬件联锁；
- GUI/UI 框架集成；
- OAuth/OIDC 授权服务器；
- REST/OpenAPI 网关；
- 设备安全结论。

这些属于宿主应用或硬件层。库只提供协议、传输、编解码、schema 校验、取消/进度、auth/approval/audit hooks。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --parallel 4
```

单头发布：

```bash
cmake --build build --target amalgamate          # 生成三个头
cmake --build build --target amalgamate-release  # 生成并打包 build/amalgamation/release
```

实现与里程碑见 `designs/mcp-library-design.md` §22.1 与 [`docs/roadmap.md`](./docs/roadmap.md)：`M1 core+stdio` → `M1.5 schema/canonical+conformance` → `M2 2026 server+client` → `M3 2025 server/session` → `M4 2025 client/replay` → `M5 宿主适配` → `M6 legacy`。

## 构建开关

| 开关 | 默认 | 说明 |
|---|---|---|
| `TUGMCP_TLS` | OFF | 是否启用 TLS |
| `TUGMCP_TLS_BACKEND` | `openssl` | TLS 后端（当前仅 OpenSSL ≥ 3.0） |
| `TUGMCP_USE_SYSTEM_DEPS` | OFF | **未实现**：`ON` configure 即失败；依赖仍来自 `3rd/` |
| `TUGMCP_VERIFY_3RD_HASHES` | ON | 校验 `3rd/` 依赖的 SHA256 |
| `TUGMCP_WITH_HTTP` | ON | 构建 `tugmcp::http` |
| `TUGMCP_WITH_JSONCONS` | OFF | 构建 `tugmcp::schema_jsoncons` |
| `TUGMCP_WITH_VALIJSON` | OFF | 构建 `tugmcp::schema_valijson` |
| `TUGMCP_ENABLE_LEGACY_SSE` | OFF | 启用 legacy HTTP+SSE 适配 |
| `TUGMCP_BUILD_TESTS` | ON | 构建测试 target |
| `TUGMCP_HEADER_ONLY` | OFF | **未实现**：split-header INTERFACE 模式 ON 即 FATAL；单头产物不依赖此开关 |
| `TUGMCP_BUILD_EXAMPLES` | ON | 构建 `examples/`（`tugmcp_echo_server` 等） |
| `TUGMCP_BUILD_QT_EXAMPLE` | OFF | 构建 Qt 5 Widgets 示例（`examples/qt_widgets_server`）；需 `TUGMCP_WITH_HTTP=ON` 与 Qt5 Widgets（`CMAKE_PREFIX_PATH`） |
| `TUGMCP_CONFORMANCE` | OFF | 运行外部 conformance 套件（需 Python3、npx/Node、HTTP 与测试；构建 `tugmcp_conformance_server`） |
| `TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION` | ON | 单头外部 Inspector/conformance 测试缺工具时仍注册并 fail loud；OFF 才跳过 |
| `TUGMCP_AMALGAMATION_RELEASE_DIR` | `${CMAKE_BINARY_DIR}/amalgamation/release` | `amalgamate-release` 输出目录 |

TLS 默认关闭；开启后需 OpenSSL ≥ 3.0（cpp-httplib 要求）与 `crypt32`（Windows），且同一进程只能有一套 OpenSSL。

依赖来自 `3rd/`。`TUGMCP_USE_SYSTEM_DEPS` **未实现**：`ON` 在 configure 阶段失败，也不会自动回退 vendored 源码。

## 许可证

本项目采用 **MIT License**，全文见根 [`LICENSE`](./LICENSE)。单头发布目录同时携带该 `LICENSE` 与第三方依赖的 MIT 许可证文本（`LICENSES/`）。

## 文档权威

- 本仓库的 `designs/` 是后续开发的本地权威。
- 文档自包含，不依赖外部仓库；设计、决策与适配边界都以本目录为准。
- 实现与设计冲突时，先改设计再改代码。
