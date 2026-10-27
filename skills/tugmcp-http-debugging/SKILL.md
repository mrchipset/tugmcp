---
name: tugmcp-http-debugging
description: tugmcp Streamable HTTP 调试指南。Use when HTTP tests fail, conformance or MCP Inspector reports errors, or when diagnosing 4xx/5xx, SSE hangs, session, TLS, header/_meta or cpp-httplib problems in tugmcp.
---

# tugmcp Streamable HTTP 调试

> Bundle 路径约定：`bundle/...` 相对 `skills/` 根（仓库内即
> `skills/bundle/...`）。在仓库内优先读原文件；发布到仓库外时以 bundle
> 快照为准。

## 0. 先分类，再改代码

| 现象 | 优先怀疑的层 | 第一动作 |
|---|---|---|
| raw `curl` 就 400/403/404/405/413/415/429 | HTTP transport | 看响应 body 的 `error.data.reason`，对照第 3 节 |
| raw `curl` 正常，`McpClient` 失败 | client | 对比 `ClientOptions::protocolVersion` 与 `StreamableHttpClientOptions::protocolVersion` |
| 200 但结果不对 | dispatcher/core | 用 unit/`FakeTransport` 隔离，不要先改 HTTP |
| SSE 挂起/无事件/断流 | server SSE 或 client 解析 | 检查 `Accept`、`maxConcurrentStreams`、`perStreamQueue` |
| 只有 conformance 失败 | wire 细节 | 对照 D18/D19 与 `docs/conformance-report.md` |
| TLS 创建失败 | 构建/选项 | 先分清 `tls_not_built` / `unsupported_scheme` / `tls_not_configured` |
| 端口/连接失败 | 绑定/网络 | 默认 `127.0.0.1`；`port=0` 用 `boundPort()` |

原则：**先确定是 transport、client、dispatcher 还是宿主**。不要用改状态码或
加 `sleep` 掩盖分层问题。

## 1. 证据来源

| 证据 | 位置/方式 |
|---|---|
| JSON-RPC 错误 | HTTP body 的 `error.code` / `error.data.reason` / `error.data.status` |
| 传输错误 | `ITransport::setOnError` 回调；不是 JSON-RPC dispatch 错误 |
| ctest 失败 | `ctest ... --output-on-failure`；doctest 会打印断言与输入 |
| 单测/集成日志 | `tugmcp_integration_tests.exe --test-case=...` |
| conformance | `build\conformance\<revision>\` 下的报告 |
| Inspector | CLI 输出；HTTP 2025 session 会打印 session 行为 |
| 源码 | `http_server.cpp::handlePost/handleGet/handleDelete`、`respondError`、`http_client.cpp` 请求构造 |

`http_server.cpp` 的关键位置：

- `registerHandlers()`：注册 `POST/GET/DELETE /mcp`。
- `handlePost()`：Host/Origin → auth → Content-Type/body → version → `_meta` →
  header 一致性 → session → dispatch。
- `handleGet()` / `handleDelete()`：2026 直接 405；2025 校验 session/replay。
- `respondError()`：写 JSON-RPC error；2026 状态映射在 `handlePost` 末尾
  （`-32601`→404，`-32602/-32020/-32021/-32022`→400）。
- `http_client.cpp`：`Accept`、`MCP-Protocol-Version`、`Mcp-Method`、
  `Mcp-Name`、`MCP-Session-Id`、`Last-Event-ID`。

## 2. 常见 `reason` 速查

| reason | 产生方 | 含义 / 首要检查 |
|---|---|---|
| `unauthorized` | server transport | 认证失败 → 401；检查 `IAuthenticator`、请求头、2025 session |
| `forbidden` | dispatcher/server | 授权拒绝 → 403；检查 `IAuthorizer` 与角色 |
| `approval_denied` / `approval_timeout` | dispatcher/server | 审批拒绝/超时 → 403；检查 `IApprovalHook` 与超时 |
| `audit_failed` | dispatcher/server | audit hook 抛异常；请求继续，错误经 `onError` 上报 |
| `missing_session` | server | 2025 POST 缺少/无效 `MCP-Session-Id` → 400 |
| `no_session_stream` | server | 2025 无 GET 流时调用 `sendToSession` |
| `session_closed` / `transport_closed` | core/server/client | 生命周期/关闭顺序；检查 `stop()`/`join()` |
| `stream_overflow` | server | `perStreamQueue` 溢出；消费端太慢或队列太小 |
| `route_not_found` | server | `sendTo` 使用了已关闭/不存在的 route |
| `tls_not_built` | factory | 构建未开 `TUGMCP_TLS`，但选项启用 TLS |
| `unsupported_scheme` | factory | `https://` 与 `tls.enabled` 不匹配，或 URL 非法 |
| `tls_not_configured` | server factory | server 启用 TLS 但缺 cert/key/不可读 |
| `option_invalid` | factory | TLS 选项组合非法（含未接线字段被设置） |
| `tls_handshake_failed` | client | CA/证书/主机名/握手失败 |
| `network_error` | client | 连接、读、编码、服务器未启动 |
| `http_status` + `data.status` | client | 收到非 2xx；去看 server 响应体 |
| `cancelled` | client | 请求发送前已被取消 |
| `transport_closed` | client | 在 close 后继续调用 |

> 2025 的 JSON-RPC 错误通常仍是 HTTP 200；不要只看状态码，必须解析 body。

## 3. HTTP 状态映射

| 状态 | 典型原因 |
|---|---|
| 200 | 成功；2025 的多数 JSON-RPC 错误也在这里 |
| 202 | 通知已接受 / 无 body |
| 400 | 解析失败、版本不支持（`-32022`）、`_meta` 缺失/形状错（`-32602`）、header/`_meta` 不一致（`-32020`）、2025 缺 session |
| 401 | 认证失败（`unauthorized`），不是 `-32021` |
| 403 | Host/Origin 非法；授权拒绝；审批拒绝/超时 |
| 404 | 2026 未知/已移除方法（`-32601`）；2025 session 未知/过期 |
| 405 | 2026 的 `GET /mcp` / `DELETE /mcp`（设计如此） |
| 413 | body 超过 `maxBodyBytes` |
| 415 | `Content-Type` 不是 JSON |
| 429 | admission 拒绝；`maxSessions`；`maxConcurrentStreams` |
| 500 | handler 未安装/内部异常 |
| 503 | transport 已关闭 |
| 504 | 等待响应超过 `requestTimeout`（不是工具执行超时） |

## 4. 最小复现

### 4.1 跑单个 ctest

```powershell
ctest --test-dir build -C Debug -N -R "integration\."          # 列出 HTTP/集成用例
ctest --test-dir build -C Debug -R "integration.HTTP client and server round-trip" --output-on-failure
ctest --test-dir build -C Debug -R "integration.2026 missing _meta" --output-on-failure
ctest --test-dir build -C Debug -R "integration.Mcp-Method mismatch" --output-on-failure
```

### 4.2 直接跑 doctest（更快拿到输出）

```powershell
# VS 多配置生成器
build\tests\Debug\tugmcp_integration_tests.exe --list-test-cases
build\tests\Debug\tugmcp_integration_tests.exe --test-case="*Mcp-Method*"

# Ninja/Makefiles
build\tests\tugmcp_integration_tests.exe --test-case="*_meta*"
```

### 4.3 启动 echo server + raw curl

```powershell
build\examples\Debug\tugmcp_echo_server.exe --http --port 18081 --bind 127.0.0.1
```

**2026 `tools/list`**（必须有 `_meta` 与 `Mcp-Method`）：

> Windows PowerShell 5.1 把内嵌双引号传给 native exe 时可能破坏 JSON
> （表现为 `-32700 malformed JSON`）。下面统一用 stdin + `--data-binary '@-'`，
> 或改用 `Invoke-WebRequest`。

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
$body | curl.exe -i -sS -X POST http://127.0.0.1:18081/mcp `
  -H 'Content-Type: application/json' `
  -H 'Accept: application/json, text/event-stream' `
  -H 'MCP-Protocol-Version: 2026-07-28' `
  -H 'Mcp-Method: tools/list' `
  --data-binary '@-'
```

**2026 `tools/call`** 额外要求 `Mcp-Name`：

```powershell
$body = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
$body | curl.exe -i -sS -X POST http://127.0.0.1:18081/mcp `
  -H 'Content-Type: application/json' `
  -H 'Accept: application/json, text/event-stream' `
  -H 'MCP-Protocol-Version: 2026-07-28' `
  -H 'Mcp-Method: tools/call' `
  -H 'Mcp-Name: echo' `
  --data-binary '@-'
```

**2025 initialize**（响应头拿 `MCP-Session-Id`）：

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
$body | curl.exe -i -sS -X POST http://127.0.0.1:18081/mcp `
  -H 'Content-Type: application/json' `
  -H 'Accept: application/json, text/event-stream' `
  --data-binary '@-'
```

后续 2025 请求带 session：

```powershell
$body = '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
$body | curl.exe -i -sS -X POST http://127.0.0.1:18081/mcp `
  -H 'Content-Type: application/json' `
  -H 'Accept: application/json, text/event-stream' `
  -H 'MCP-Protocol-Version: 2025-11-25' `
  -H 'MCP-Session-Id: <session-id>' `
  --data-binary '@-'
```

**2025 GET 流 / DELETE**：

```powershell
curl.exe -N -sS -H 'Accept: text/event-stream' `
  -H 'MCP-Protocol-Version: 2025-11-25' `
  -H 'MCP-Session-Id: <session-id>' http://127.0.0.1:18081/mcp

curl.exe -i -sS -X DELETE -H 'MCP-Session-Id: <session-id>' `
  -H 'MCP-Protocol-Version: 2025-11-25' http://127.0.0.1:18081/mcp
```

### 4.4 Inspector / conformance

```powershell
npx -y @modelcontextprotocol/inspector@2.12.0 --cli `
  --server-url http://127.0.0.1:18081/mcp --transport http --method tools/list

npx -y @modelcontextprotocol/conformance@0.2.0-alpha.12 server `
  --url http://127.0.0.1:18081/mcp --requirements 2026-07-28 --timeout 8000 `
  -o build\conformance-debug
```

conformance 的 `server` 模式只接受 `--url`（HTTP/SSE），**不支持 stdio**；
stdio 用 Inspector CLI。

## 5. 故障树

### 5.1 400（在 dispatch 之前）

按固定顺序排查：

1. `Host` 或 `Origin` 非法 → 其实是 403；检查是否被反代改写。
2. `Content-Type` 不是 `application/json` → 415。
3. body 超限 → 413。
4. `MCP-Protocol-Version` 不支持 → `-32022`，看 `data.requested/supported`。
5. `_meta` 不是对象，或缺 namespaced 键 → `-32602`。通知违规时是 **400
   空 body**，没有 JSON-RPC error。
6. `_meta.protocolVersion` 与 header 不一致 → `-32020`。
7. `Mcp-Method` 与 `method` 不一致 → `-32020`。
8. `tools/call` 的 `Mcp-Name` 缺失/不匹配 → `-32020`。
9. 2025 缺 `MCP-Session-Id` → `missing_session`。

常见拼写错误：

- `io.modelcontextprotocol/protocolVersion`
- `io.modelcontextprotocol/clientCapabilities`
- 不是 `protocolVersion` / `clientCapabilities` 顶层键。

### 5.2 401 / 403

- 401：transport 认证失败，`data.reason="unauthorized"`。
- 403 + 有 JSON-RPC：authorizer/approval 拒绝。
- 403 + 无 JSON-RPC：Host/Origin 校验失败；检查 `allowedHosts` /
  `allowedOrigins`。`Origin: null` 被拒绝；代理场景需显式配置或 `"*"`。
- 不要把认证失败映射成 `-32021`（能力缺失）。

### 5.3 404

- 2026：`initialize`、`ping`、`logging/setLevel`、`resources/subscribe`、
  `resources/unsubscribe`、未知方法 → 404 + `-32601`（设计如此）。
- 2025：session 不存在/过期 → 404；先确认 `MCP-Session-Id` 与
  `sessionTimeout`。
- `DELETE` 后再 POST → 404。

### 5.4 405

2026 的 `GET /mcp` / `DELETE /mcp` 恒为 405；这是 stateless 设计，不是路由
缺失。2025 才支持 GET/DELETE。

### 5.5 413 / 415 / 429

- 413：`maxBodyBytes`；检查客户端是否把大对象塞进单条 JSON。
- 415：`Content-Type` 必须 JSON。
- 429：admission 拒绝、`maxSessions`、`maxConcurrentStreams`。区分：
  - 新 `initialize` 被拒 → `maxSessions`；
  - 第二条 SSE → `maxConcurrentStreams`；
  - 业务 admission → `IAdmissionHook::Reject`。

### 5.6 500 / 503 / 504

- 500 `message handler unavailable`：`McpServer` 尚未安装回调就收到请求；
  通常是先 `transport->start()` 再构造 `McpServer` 的竞态。用测试里的
  ready-signal wrapper，或直接 `server.run()`。
- 503 `transport is closed`：close 之后仍有请求/通知。
- 504 `request timed out`：`requestTimeout` 是等待响应发送，不是工具执行
  超时；慢工具会表现为 504。工具执行超时应在 handler/executor 侧处理。

### 5.7 SSE

- 没收到事件：客户端 `Accept` 必须包含 `text/event-stream`；否则 server 走
  普通 JSON 响应，progress 通知会被丢弃。
- 流被拒 429：`maxConcurrentStreams`。
- `stream_overflow`：`perStreamQueue` 满；消费端太慢。先确认客户端持续读，
  再考虑调大队列。
- `Last-Event-ID` 重放仅 2025；2026 无 GET 流。
- 关闭时检查 pending/route 清理；`close()` 应只触发一次 `onClose`。

### 5.8 session

- `initialize` 响应没有 `MCP-Session-Id`：确认走的是 2025（客户端/transport
  的 `protocolVersion` 一致），而不是 2026。
- 后续 POST `missing_session`：把响应头原样带回。
- session 过期：`sessionTimeout` + `timeSource`；测试用 `ManualTimeSource`
  推进时间，不要 `sleep`。
- `enable2025Session` 当前不参与判定；不要靠它开关行为。

### 5.9 TLS

| 现象 | 原因 |
|---|---|
| 创建期 `tls_not_built` | 构建未开 `TUGMCP_TLS=ON` |
| 创建期 `unsupported_scheme` | `https://` 与 `tls.enabled` 不一致，或 URL 非法 |
| 创建期 `tls_not_configured` | server 缺 cert/key 或不可读 |
| 创建期 `option_invalid` | TLS 选项非法/未接线字段被设置 |
| 首个请求 `tls_handshake_failed` | CA/证书/主机名/握手失败；检查 `verifyPeer`、`caBundlePath` |
| 服务端连接级 `onError` | 握手失败，不会有 JSON-RPC 响应 |

`serverName`、`minVersion`、`privateKeyPassword` 当前未接线；SNI 使用 URL
host。TLS 需要 OpenSSL ≥ 3.0，且所有 TU 的 TLS 宏一致。

### 5.10 版本/默认值错配

- `ClientOptions::protocolVersion` 默认 **2026-07-28**；
  `StreamableHttpClientOptions::protocolVersion` 默认 **2025-11-25**。
  不显式设置两者，容易出现“客户端发 2026、transport 发 2025”的混合行为。
- 2026 需要 `_meta` 与 `Mcp-Method`/`Mcp-Name`；2025 需要 session。
- stdio 没有版本元数据，dispatcher 按 2025 处理；不要把 stdio 的默认值
  套到 HTTP 调试结论里。

### 5.11 绑定/端口

- 默认 `bind=127.0.0.1`；外部访问需显式 `0.0.0.0`，并自行评估 Host/Origin/
  TLS/admission。
- `port=0` 时用 `boundPort()` 取实际端口；不要猜固定端口。
- 端口冲突/防火墙先看 `create`/`start` 的 `Status`，不要先怀疑协议。

## 6. 插桩

- 优先用 `setOnError` 捕获 transport 错误；它不是 HTTP 状态码的替代。
- 需要临时日志时写 **stderr**，绝不能写 stdout（stdio 模式 stdout 只允许
  协议帧）。
- 不要默认打印完整 body/headers/token；可能含凭据。
- 审计事件用 `IAuditHook`，不要靠日志猜测 auth/approval 路径。
- 复现 conformance 失败时保留 `build\conformance\<revision>\` 输出。

## 7. 隔离实验

1. **raw curl**：只验证 transport（header/status/body）。
2. **`McpClient` loopback**：验证 client + server 组合。
3. **unit + `FakeTransport`**：验证 dispatcher/core 语义。
4. **conformance**：验证规范细节。

若 raw curl 正常而 `McpClient` 失败，先查 client 的
`protocolVersion`/header/session，而不是 server。

## 8. 不要做

- 不要把新失败加进 `bundle/tests/conformance/expected_failures.yaml`（仓库源：
  `tests/conformance/expected_failures.yaml`）来“变绿”；该文件只登记已知、
  已解释的未计分项。
- 不要用 catch-all 500 掩盖 4xx；状态码是 wire 契约。
- 不要用 `sleep_for` 同步 server/client；用 ready signal 或 ctest 超时。
- 不要为了通过测试改 `_meta` 键名或删掉 header 校验；先改 D18/D19 并补
  conformance。
- 不要静默跳过外部验证；需要跳过时显式
  `-DTUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF` 并记录原因。
