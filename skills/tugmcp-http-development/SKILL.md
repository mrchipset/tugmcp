---
name: tugmcp-http-development
description: tugmcp Streamable HTTP 功能开发指南。Use when changing or extending tugmcp HTTP transport code (http_server.cpp, http_client.cpp, transport/http.hpp), /mcp routing, 2026-07-28 / 2025-11-25 protocol behavior, SSE, sessions, TLS, cpp-httplib boundaries, or the tugmcp_http single-header artifact.
---

# tugmcp Streamable HTTP 开发

## 0. 适用范围

覆盖 `tugmcp::http`（Streamable HTTP server/client、SSE、2025 session、TLS、
legacy adapter）的源码、公共头与单头产物。

不覆盖：stdio 传输、schema 后端、宿主业务逻辑、OMS/qmake 集成。遇到这些
内容先回到 `AGENTS.md` 的边界。

## 1. 先读（按权威顺序）

| 文档 | 读什么 |
|---|---|
| `AGENTS.md` | 架构不变量、公共头/依赖/错误码约束 |
| `designs/protocol-decisions.md` | D2/D3（传输）、D14（TLS）、D18（2026 wire）、D19（2025 logging/subscription）、D20（安全 hooks） |
| `designs/mcp-library-design.md` | HTTP 分层、生命周期、关闭顺序 |
| `docs/api.md` §7.3、§11.6 | `StreamableHttpServerOptions` / `ClientOptions`、HTTP 状态映射 |
| `docs/integration.md` §5–§6、§11–§12 | 宿主接入、线程/关闭、部署边界、已知限制 |
| `designs/testing.md` + `tests/CMakeLists.txt` | 测试分层；实际 target/label 以 CMake 为准 |
| `docs/conformance-report.md` | 外部 Inspector/conformance 证据与偏差处理 |

改 wire 行为（header、`_meta`、状态码、session、SSE 事件）时，**先改
`protocol-decisions.md`，再改代码**。

## 2. 代码地图

| 位置 | 职责 |
|---|---|
| `include/tugmcp/transport/http.hpp` | 公共 API：`TlsOptions`、`StreamableHttpServerOptions`、`StreamableHttpClientOptions`、`HttpServerTransport`、两个工厂 |
| `include/tugmcp/transport.hpp` | `ITransport` 契约、`MessageMetadata`（protocolVersion/route/sessionId/clientCapabilities/auth/emergency） |
| `src/transport/http_server.cpp` | server `Impl`：`/mcp` 路由、Host/Origin、认证、body 限制、2026/2025 分支、session、SSE 队列、错误状态映射 |
| `src/transport/http_client.cpp` | client：URL/scheme 校验、请求 header、`_meta`、SSE 解析、session、取消/超时 |
| `src/transport/http_security.hpp` | `hostAllowed` / `originAllowed` / loopback 判定 |
| `src/transport/legacy_sse.cpp` | legacy `/sse` + `/messages`，仅 `TUGMCP_ENABLE_LEGACY_SSE=ON` 编译 |
| `include/tugmcp/detail/http_httplib.hpp` | **唯一** cpp-httplib include 点；检查 `TUGMCP_TLS_ENABLED` 与 `CPPHTTPLIB_OPENSSL_SUPPORT` 一致 |
| `src/core/dispatcher.cpp` | MCP method 语义、MRTR、auth/authorization/approval/audit；**不要**把 method 语义搬进 transport |
| `tests/integration/http_*.cpp` | HTTP 行为的事实规格；改行为先看这些用例 |
| `CMakeLists.txt`（根） | `tugmcp_http` target：PUBLIC core、PRIVATE httplib、Windows `ws2_32`、TLS OpenSSL/`crypt32` |

## 3. 不可破坏的边界

1. **传输只搬消息**：`ITransport` 不理解 MCP method。2026/2025 的 lifecycle、
   session、header、SSE 行为留在 HTTP adapter；core/dispatcher 不写散落的
   `if (version == ...)`。
2. **2026 与 2025 语义隔离**：2026 stateless（无 session/GET/
   `Last-Event-ID`/ping）；2025 有 `initialize`/session/GET 流/DELETE/replay。
   不要用同一个分支同时改两代行为。
3. **cpp-httplib 不进公共头**：除 `detail/http_httplib.hpp` 外，任何公共头
   不得 include `httplib.h`。
4. **error-as-value**：协议/传输错误用 `Result`/`Status`；异常不得穿过
   transport/线程边界。route handler 最外层必须有 `try/catch`，返回 500 而
   不是让异常逃逸。
5. **不共享可变 Json**：按请求 decode；handler 在 `IExecutor` 执行；注意
   `MessageMetadata` 是唯一跨层上下文。
6. **安全默认**：默认 bind `127.0.0.1`；legacy 默认 OFF；TLS 默认 OFF；
   Host/Origin 空列表表示仅 loopback；`schema` 未配置时 fail closed（core 侧）。
7. **关闭语义**：`close()` 幂等；关闭后不再有 `onMessage`/`onError`，最多一次
   `onClose`；SSE pending/路由必须清理。
8. **不把客户端 capability 当授权**：`clientCapabilities` 只是声明值。

## 4. 协议处理流水线

### 4.1 2026-07-28（stateless）

固定校验顺序（与 D18 一致）：

| 顺序 | 检查 | 失败结果 |
|---|---|---|
| 1 | Host / Origin | 403，无 JSON-RPC |
| 2 | `Content-Type: application/json` | 415 |
| 3 | body ≤ `maxBodyBytes` | 413 |
| 4 | `MCP-Protocol-Version` 支持 | 400 + `-32022`，`error.data.requested/supported` |
| 5 | `params._meta` 是对象，且含 `io.modelcontextprotocol/protocolVersion`（string）与 `io.modelcontextprotocol/clientCapabilities`（object） | 400 + `-32602`；通知返回 400 空 body |
| 6 | `_meta.protocolVersion` == header | 400 + `-32020` |
| 7 | `Mcp-Method` == `message.method` | 400 + `-32020` |
| 8 | `tools/call` 时 `Mcp-Name` == `params.name` | 400 + `-32020` |
| 9 | 方法存在 | 404 + `-32601` |
| 10 | 认证 / 授权 / 审批 | 401 / 403 |
| 11 | dispatch | 200；超时 504；transport closed 503；通知 202 或 SSE |

- 2026 的 `initialize`、`ping`、`logging/setLevel`、`resources/subscribe`、
  `resources/unsubscribe` 以及未知方法都返回 **404 + `-32601`**。
- 错误必须回显请求 id（能解析时不得 `id:null`）。
- 订阅通知的 `params._meta["io.modelcontextprotocol/subscriptionId"]` 必须与
  `subscriptions/listen` 的 JSON-RPC id **类型和值都一致**；不要用顶层
  `subscriptionId`。

### 4.2 2025-11-25（session）

- `initialize` 创建 session，响应头 `MCP-Session-Id`；客户端随后每个 POST/
  GET/DELETE 带该 header。
- `GET /mcp` 打开 SSE 流；`Last-Event-ID` 触发 per-stream replay。
- `DELETE /mcp` 关闭 session，返回 204；之后 POST 为 404。
- 2025 的 JSON-RPC 错误通常仍是 HTTP 200；只有 session/流/认证/限流/解析
  等传输层问题用非 200。
- `enable2025Session` 字段当前**不参与判定**；行为由协商版本驱动。

### 4.3 legacy HTTP+SSE

- `/sse` + `/messages`，仅 `TUGMCP_ENABLE_LEGACY_SSE=ON` 编译；默认关闭，
  不进单头。改 legacy 时同时检查 `tests/integration/legacy_sse_test.cpp`。

## 5. 修改流程

1. **决策**：wire/状态码/session/TLS 变化 → 先更新
   `designs/protocol-decisions.md`（必要时新增 D 编号）。
2. **公共 API**：新增/修改 `StreamableHttpServerOptions`、
   `StreamableHttpClientOptions`、`TlsOptions` 或 `ITransport` 契约时，
   同步 `docs/api.md`、`docs/integration.md`、测试；公共头保持 C++17 与
   `Result`/`Status` 风格。
3. **实现**：
   - server 选项 → 工厂校验（`optionFailure` + `data.reason`）+ `Impl` 使用 +
     `docs/integration.md` 选项表。
   - 新路由 → `Impl::registerHandlers()` 注册；在 handler 里复制同样的
     Host/Origin → auth → body/version → session 顺序；错误用 `respondError`。
   - 改 header/`_meta` → 同时改 client 发送端、server 校验端、2026/2025 分支。
   - SSE/队列 → 检查 `maxConcurrentStreams`、`perStreamQueue`、`stream_overflow`、
     close/pending 清理。
4. **测试**：按 `tugmcp-http-testing` 增加 raw HTTP + loopback + 失败路径。
5. **单头**：`cmake --build build --target amalgamate`；若是有意变更，更新
   `tests/amalgamation/http.sha256`（TLS 改 `http_tls.sha256`），再跑
   `ctest -L amalgamation`。
6. **文档**：更新 `docs/api.md` / `docs/integration.md` / `docs/building.md`
   中受影响的段落；不要只改代码。

## 6. 构建与本地运行

```powershell
# 默认 Debug（HTTP ON、TLS OFF）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target tugmcp_integration_tests tugmcp_echo_server

# 跑 HTTP echo server
build\examples\Debug\tugmcp_echo_server.exe --http --port 18081 --bind 127.0.0.1
```

TLS 构建：

```powershell
cmake -S . -B build-tls -G "Visual Studio 17 2022" -A x64 `
  -DTUGMCP_TLS=ON -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL"
cmake --build build-tls --config Debug --target tugmcp_integration_tests
```

单头：

```powershell
cmake --build build --target amalgamate
cmake --build build --target amalgamate-release
```

## 7. Windows / MSVC 细节

- `tugmcp::http` 是 STATIC；消费者链接顺序：`tugmcp_http` → `tugmcp_core` →
  `ws2_32`。TLS 再加 `OpenSSL::SSL`/`OpenSSL::Crypto` 与 `crypt32`。
- `TUGMCP_TLS_ENABLED` / `CPPHTTPLIB_OPENSSL_SUPPORT` 必须对所有看到
  `httplib.h` 的 TU 一致；禁止在同一程序混用 TLS/非 TLS 单头。
- 单头 HTTP 消费者在 Windows 需 `/bigobj`（`tests/amalgamation` 已验证）。
- Qt/OMS 宿主切 C++17 时，若使用 `using namespace std;` 且包含 Windows SDK，
  可能遇到 `std::byte` 与全局 `byte` 冲突；仓库外的消费方可定义
  `_HAS_STD_BYTE=0`（本项目 `src/` 不使用 `std::byte`）。
- 默认 `/W4 /permissive- /utf-8`；不要引入 `/W4` 新告警。

## 8. 已知未接线项（不要“顺手实现”）

以下字段/行为在头文件或文档中标注未接线，改动需要 ADR：

- `enable2025Session` 不参与判定。
- `emergencyPath`、`dedicatedEmergencyListener` 未接线。
- `TlsOptions::privateKeyPassword`、`serverName`、`minVersion` 未接线。
- `requestTimeout` 只约束等待发送响应，不是工具执行超时。
- `TUGMCP_HEADER_ONLY=ON` 仍是 `FATAL_ERROR`；单头 artifact 独立于该开关。
- 单头不包含 schema（jsoncons/Valijson）与 legacy。

## 9. 完成定义

- [ ] 设计文档/ADR 已更新（wire/API/状态码变化）。
- [ ] 公共头与实现一致；没有把 httplib 或 MCP method 语义带进 core。
- [ ] 2026 与 2025 分支都覆盖；失败路径有状态码与 `reason`。
- [ ] `ctest -L integration` 通过（必要时 TLS/legacy 单独构建）。
- [ ] raw HTTP 用例覆盖 header/`_meta`/session/status。
- [ ] 单头 hash 已更新且 `ctest -L amalgamation` 通过（如适用）。
- [ ] `docs/api.md`、`docs/integration.md`、`docs/building.md` 同步。
