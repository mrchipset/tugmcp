# 外部 MCP 互操作与 conformance 测试记录

> 日期：2026-10-24
> 代码基线：`4c7d150`（feat(examples): add Streamable HTTP mode to echo server）
> 目的：记录首次使用**外部 MCP 客户端/官方 conformance 套件**的测试结果，作为 2026-07-28 协议对齐与 P4 的输入。
> 本文是测试证据，不是设计权威；wire 决策见 `designs/protocol-decisions.md` D18。

## 1. 环境

| 项 | 值 |
|---|---|
| OS | Windows 10.0.26100 x64 |
| 编译器 | MSVC 14.42（`/W4 /permissive- /utf-8`） |
| CMake | 4.2.1 |
| Node / npm | v22.13.1 / 11.3.0（registry: `https://registry.npmmirror.com`） |
| MCP Inspector | `@modelcontextprotocol/inspector@2.12.0`（CLI 模式） |
| MCP Conformance | `@modelcontextprotocol/conformance@0.2.0-alpha.12` |
| 被测 server | `build/examples/Debug/tugmcp_echo_server.exe`（仅 `echo` 工具），stdio 与 `--http` 两种模式 |
| 本库测试基线 | TLS=OFF 237/237；TLS=ON 238/238；schema 191/191 |

Inspector 2.12.0 声明 Node ≥22.19；本机 22.13.1 仅 `EBADENGINE` 警告，CLI 实际可用。

## 2. MCP Inspector（外部客户端）

全部通过：

| 传输 | 操作 | 结果 |
|---|---|---|
| stdio | `initialize` + `tools/list` | 返回 `echo` 工具 |
| stdio | `tools/call`（`text=hello`） | 返回 text content + structuredContent |
| HTTP 2025 session | `initialize`（服务端返回 `MCP-Session-Id`） | 通过 |
| HTTP 2025 session | `tools/list` | 返回 `echo` |
| HTTP 2025 session | `tools/call`（`text=hello-http`） | 返回 text content + structuredContent |

结论：2025 的 stdio 与 Streamable HTTP 基本握手、session、工具调用可被独立客户端消费。

复现：

```powershell
$exe = (Resolve-Path 'build\examples\Debug\tugmcp_echo_server.exe').Path
npx -y @modelcontextprotocol/inspector@2.12.0 --cli $exe --method tools/list
npx -y @modelcontextprotocol/inspector@2.12.0 --cli $exe --method tools/call --tool-name echo --tool-arg text=hello

# HTTP（先启动 --http --port 18081）
npx -y @modelcontextprotocol/inspector@2.12.0 --cli `
  --server-url http://127.0.0.1:18081/mcp --transport http --method tools/list
```

## 3. 官方 conformance（2026-07-28）

命令：

```powershell
$exe = (Resolve-Path 'build\examples\Debug\tugmcp_echo_server.exe').Path
$p = Start-Process -FilePath $exe -ArgumentList '--http','--port','18083' -PassThru `
  -RedirectStandardError "$env:TEMP\tugmcp-conf.err" -RedirectStandardOutput "$env:TEMP\tugmcp-conf.out"
Start-Sleep -Milliseconds 1200
npx -y @modelcontextprotocol/conformance@0.2.0-alpha.12 server `
  --url http://127.0.0.1:18083/mcp --requirements 2026-07-28 --timeout 8000 `
  -o build\conformance-2026-echo
Stop-Process -Id $p.Id -Force
```

结果：**79 passed / 88 failed**（50 scenarios；含 extension/pending 不计分项）。结果目录 `build/conformance-2026-echo/`（构建产物，不入库）。

### 3.1 必须修的真实协议偏差

| # | check id | 期望 | 实际 |
|---|---|---|---|
| 1 | `sep-2575-request-meta-invalid-missing-meta` | `-32602` + HTTP 400 | 200，正常结果 |
| 2 | `sep-2575-request-meta-invalid-missing-protocol-version` | `-32602` + HTTP 400 | 200，正常结果 |
| 3 | `sep-2575-request-meta-invalid-missing-client-capabilities` | `-32602` + HTTP 400 | 200，正常结果 |
| 4 | `sep-2575-http-server-header-mismatch-400` | HTTP 400 + `-32020` | 200 |
| 5 | `sep-2575-server-unsupported-version-error` | `-32022`，`error.data.supported` 数组 + `requested` 回显 | 结构不匹配 |
| 6 | `sep-2575-http-server-error-jsonrpc-id` | 错误回显请求 id（301） | `id:null` |
| 7 | `sep-2575-http-server-method-not-found-404-initialize` | 404 + `-32601` | 200，成功 |
| 8 | `sep-2575-http-server-method-not-found-404-ping` | 404 + `-32601` | 200，成功 |
| 9 | `sep-2575-http-server-method-not-found-404-logging-setlevel` | 404 + `-32601` | 200 + `-32601` |
| 10 | `sep-2575-http-server-method-not-found-404-resources-subscribe` | 404 + `-32601` | 200 + `-32601` |
| 11 | `sep-2575-http-server-method-not-found-404-resources-unsubscribe` | 404 + `-32601` | 200 + `-32601` |
| 12 | `sep-2575-http-server-method-not-found-404`（未知方法） | 404 + `-32601` | 200 + `-32601` |
| 13 | `sep-2575-server-identifies-in-result-meta` | result `_meta["io.modelcontextprotocol/serverInfo"]`（SHOULD） | 仅顶层 `serverInfo`（warning） |
| 14 | `sep-2575-server-tags-subscription-id` | 通知 `params._meta["io.modelcontextprotocol/subscriptionId"]` | 顶层 `subscriptionId` |
| 15 | `localhost-host-rebinding-rejected` | 非法 Host/Origin → 4xx | 200 |
| 16 | `completion-complete` | `{completion:{values:[]}}` | `-32601` |
| 17 | `sep-2575-server-sends-tools-list-changed-on-subscription` | 变更后 `notifications/tools/list_changed`（SHOULD） | 未发送（warning） |

关键规范细节（来自 conformance 0.2.0-alpha.12 的 schema/scenario）：

- 2026 请求 `params._meta` **必须**含 `io.modelcontextprotocol/protocolVersion` 与 `io.modelcontextprotocol/clientCapabilities`；`clientInfo` 为 SHOULD；`progressToken` 仍是普通键；日志级别键为 `io.modelcontextprotocol/logLevel`。
- 2026 移除 `initialize`/`ping`/`logging/setLevel`/`resources/subscribe`/`resources/unsubscribe`；未知方法同样 404 + `-32601`。
- 版本不支持：`-32022`，`data.requested` 回显，`data.supported` 为支持版本数组；HTTP 400。
- 订阅：请求 filter 为 `params.notifications.{toolsListChanged,promptsListChanged,resourcesListChanged,...}`；ack 首帧，`params._meta.subscriptionId` 等于 `subscriptions/listen` 的 JSON-RPC id；所有流通知带同一 `_meta.subscriptionId`；不再使用顶层 `subscriptionId`/`requestId`。
- `completion/complete` 属 2026 必需场景；最小空数组即可通过，但必须声明 `completions` capability 并可响应。

### 3.2 已通过的信号

- `tools-list` 3/3；`server-sse-multiple-streams` 1/1；`sep-2164-resource-not-found` 2/2；
- `http-header-validation` 14/14（pending 项）；
- MRTR 结构子集：`missing-input-response`、`unsupported-methods`、`ignore-extra-params`、`validate-input`；
- `caching` 的 `tools/list` hints。

### 3.3 Fixture 缺口（不是库 bug）

当前 echo server 只有 `echo`，而 conformance 期望测试资产：

- `test_simple_text`、`test_image_content`、`test_audio_content`、`test_embedded_resource`、`test_mixed_content`、`test_error`、`test_progress`；
- `test_missing_capability`、`test_streaming_elicitation`、`test_logging_tool`；
- `test_trigger_tool_change`、`test_trigger_prompt_change`；
- resources/read（text/binary）、resources/templates、prompts（simple/args/embedded/image）、MRTR 多轮等。

因此 `resources-*`、`prompts-*`、`tools-call-*`、`input-required-*` 的大量失败属 fixture 不足；必须在测试侧提供专用 conformance fixture server，不能用生产 example 代替。

## 4. 限制与后续

- 官方 conformance 的 `server` 子命令只接受 `--url`（HTTP/SSE），**不支持 stdio command**；stdio 外部验证目前用 Inspector。`designs/testing.md` §8.4 的 runner 需按此现实修订。
- `tasks-*` 为 extension，`json-schema-2020-12`/`http-header-validation` 等为 pending/draft，本次不计分。
- 外部套件结果只写入 `build/`，不污染源码树。
- 下一步：先按 D18 修真实偏差，再建 conformance fixture server，重跑 `--requirements 2026-07-28` 与 `2025-11-25`，直到必需场景真实失败清零；然后进入 P4。
