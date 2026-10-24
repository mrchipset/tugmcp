# 路线图（Roadmap）

> 状态快照：**2026-10-24**（Gate 2b 通过，M2 完成）
> 本文是**项目级进度视图**，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。
> 状态口径：`已完成` / `部分` / `未开始`；标「部分」时必须写清缺口。

## 1. 里程碑状态（design §22.1）

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | core 类型/错误模型/严格 JSON-RPC；stdio server + client；echo tool；真实管道 loopback；非法 JSON/EOF/超长行用例；默认构建 + vendored 哈希校验；unit 测试集 |
| **M1.5** schema / canonical / conformance harness | 部分 | `ISchemaValidator` + jsoncons 1.10（2020-12）+ Valijson 1.1.3（draft-07）、fail-closed、canonical 子集、`tool_schema_hash`。缺口：**外部 conformance harness 未接**（`TUGMCP_CONFORMANCE` 仍 FATAL） |
| **M2** 2026 server + client（HTTP/TLS） | **已完成（P2b）** | JSON + 2026 headers/`_meta`、`resultType`/`ttlMs`/`cacheScope`、progress、resources/prompts/templates、分页、MRTR、`subscriptions/listen`+ack/filter+推送、per-request SSE、route 隔离、admission/并发流限制、`discover`/2025 回退、client SSE/progress/MRTR/cancel/订阅、`echo_client`。验收：215/216 ctest；外部 conformance 向量待 P4 |
| **M3** 2025 server/session | 未开始 | 仅有 `initialize` 子集；无 session / POST/GET/DELETE / per-stream replay |
| **M4** 2025 client/replay | 未开始 | 客户端无 session、`Last-Event-ID` replay、server→client 请求处理 |
| **M5** 宿主适配 | 部分 | `IExecutor` 已落地（D17），默认仍同步；缺 auth/approval/audit hooks、默认执行器翻转 |
| **M6** legacy shim | 未开始 | `TUGMCP_ENABLE_LEGACY_SSE` 置 ON 即 `FATAL_ERROR`（P4） |

## 2. 设计章节逐节状态

| 章节 | 状态 | 说明 |
|---|---|---|
| §1–§3 目标 / 非目标 / 原则 | 已完成 | 设计已固化 |
| §4 协议与传输矩阵 | 部分 | stdio + Streamable HTTP（JSON/SSE）已实现；2025 session/replay、legacy 未做 |
| §5 组件架构 | 部分 | `ITransport`/`ITimeSource`/`IExecutor`/server/client 已实现；未拆 `IProtocolAdapter`（版本行为集中在 dispatcher） |
| §6 模块与目录 | 已完成（公共头） | 21 个公共头，含 `operation/resource/prompt/executor/transport/http` |
| §6.1 CMake targets | 部分 | `core`/`tests`/`executor_tests`/`schema_*`/`http`；header-only/install 未做 |
| §7 核心类型与错误模型 | 已完成 | `resultType`/`OperationOutcome`/`Page`/`Progress`/`SubscriptionFilter` 已落地 |
| §7.4 严格 JSON | 已完成 | UTF-8、重复 key、深度、非有限数、batch 拒绝 |
| §8 并发模型 | 部分 | 单 reader + 执行器 + 有界队列 + 取消注册；handler 跨线程取消仅 `stop()` 触发 |
| §9 stdio 传输 | 已完成 | 行 framing、CRLF、EOF 尾行、超长恢复、Windows 中断 |
| §10 Streamable HTTP server | 已完成（P2b） | JSON/SSE、route 隔离、admission 429、`maxConcurrentStreams`、`perStreamQueue`、`_meta`/header 一致性、关闭等待流 |
| §11 Streamable HTTP client | 部分 | SSE 增量解析、progress/MRTR、cancel、订阅；**per-send client**（连接复用留 M4/M5）；2025 fallback 未真正切换发送版本 |
| §12 legacy HTTP+SSE | 未开始 | P4 |
| §13 协议适配 | 已完成（M2 范围） | `discover`/`initialize`/`ping`/tools/resources/prompts、MRTR 结构校验、订阅、2025 省略 2026 字段 |
| §14 Server API | 部分 | tools/providers/notify/activeSubscriptions/run/stop；缺 auth/approval/audit hooks、动态 provider 注册 |
| §15 Client API | 部分 | discover/initialize/list*/read/get/callTool/subscribe/unsubscribe/cancel；返回 `Json`（强类型 outcome 留 M4） |
| §16 线程 / 背压 / 关闭 | 部分 | 执行器、队列、SSE 背压、路由关闭清理已实现；HTTP 超时取消 handler 未做（P3） |
| §17 schema / canonical JSON | 部分 | 同 M1.5，conformance harness 未接 |
| §18 安全 | 未开始 | 无 auth/approval/audit、Origin/Host/rate（P4） |
| §19 测试与一致性 | 部分 | unit/integration/ODR 215/216；conformance、覆盖率口径未接 |
| §20 构建 / 依赖 / ABI | 部分 | 静态库 + vendored + SHA256；header-only/system-deps/install 未做 |
| §21 宿主适配边界 | 部分 | 文档完成；代码 hooks 属 P4 |
| §22 里程碑 / 风险 / 未决 | 部分 | M2 完成；D17 与 P2b 偏差已入档 |

## 3. 关键设计要素

| 要素 | 状态 | 备注 |
|---|---|---|
| 静态库（默认产物） | 已完成 | `tugmcp::core` 为 STATIC |
| `IExecutor` | 已完成（P2a） | 三级优先、有界队列、`noexcept` 契约、`stats`/`waitIdle`、自我排除关闭；默认仍同步（P3 翻转，D17） |
| 2026 协议面 | 已完成（P2b） | envelope、分页、resources/prompts、MRTR、progress、订阅 |
| SSE / 背压 / admission | 已完成（P2b） | per-request SSE、route 隔离、429、有界队列、池大小 `max(8, streams+4)` |
| HTTP client 2026 | 部分 | SSE/progress/MRTR/cancel/订阅；连接复用与 2025 发送版本留 P3/M4 |
| header-only 消费 | 未开始 | `TUGMCP_HEADER_ONLY=ON` → `FATAL_ERROR` |
| OpenSSL 可选编译 | 已完成 | `TUGMCP_TLS=ON`（OpenSSL ≥3.0）loopback 通过；TLS=OFF 时 `https://` 显式失败 |
| `3rd/` vendoring | 部分 | nlohmann/json 3.12.0、doctest 2.5.3、cpp-httplib 0.63.0、jsoncons 1.10.0、Valijson 1.1.3 |
| `TUGMCP_USE_SYSTEM_DEPS` | 未开始 | → `FATAL_ERROR` |
| conformance 套件 | 未开始 | `TUGMCP_CONFORMANCE` → `FATAL_ERROR` |
| examples | 部分 | `echo_server` + `echo_client`（HTTP 2026） |
| install / export | 未开始 | 设计 v1 明确不提供 |

## 4. 实现与验证证据

- **公共头（21）**：`core` / `version` / `error` / `message` / `transport` / `transport/stdio` / `transport/http` / `request_context` / `time_source` / `tool` / `operation` / `resource` / `prompt` / `server` / `client` / `executor` / `schema` / `canonical` / `schema/{null,jsoncons,valijson}`（另有实现专用 `detail/http_httplib.hpp`）
- **源码（17）**：`src/core/{version,error,message,codec,canonical,registry,dispatcher,server,executor,client,operation}.cpp`、`src/core/dispatcher.hpp`、`src/transport/{stdio,http_server,http_client}.cpp`、`src/schema/{jsoncons,valijson}_validator.cpp`
- **测试**：12 个 unit 文件 + 独立 `tugmcp_executor_tests`（integration，10s）+ 8 个 integration 文件 + 双 TU ODR + `tests/helpers/fake_transport.hpp`
- **ctest**：默认 `TLS=OFF` **215/215**；`TLS=ON` **216/216**；schema 构建 **191/191**
- **构建**：MSVC `/W4 /permissive- /utf-8` 无新告警（仅 vendored httplib C4702）

| commit | 内容 |
|---|---|
| `3f52356` | `feat(core)`：构建骨架、vendored 依赖、错误模型、严格 JSON-RPC codec |
| `2468603` | `feat(stdio)`：registry、dispatcher、stdio server、echo example |
| `efe6d1d` | `feat(client)`：`McpClient`、pending/超时、loopback |
| `b43961f` | `feat(schema)`：M1.5 schema、canonical、`tool_schema_hash` |
| `22f2fbb` | `feat(http)`：Streamable HTTP JSON 传输 |
| `2f359e0` | `feat(executor)`：`IExecutor`、线程池、off-thread dispatch（P2a） |
| `9426a6d` | `fix(executor)`：冻结契约、自我排除关闭、ADR D17 |
| `f20220f` | `feat(protocol)`：envelope/resources/prompts/MRTR/progress（P2b-B1） |
| `d3c59f4` | `feat(http)`：route pending、per-request SSE、admission、shutdown（P2b-B2） |
| `c84c3e6` | `feat(client)`：2026 discover、SSE、progress/MRTR/cancel（P2b-B3a） |
| `05df1a3` | `feat(subscriptions)`：listen/ack/filter、notify、echo_client（P2b-B3b） |
| `c32a6bf` | `fix(protocol)`：Gate 2b Critical/Major 修补 |
| `d7f2e39` | `fix(http)`：pre-send cancel、initialize resultType、媒体块校验 |

## 5. 已记录偏差

1. **D16/D17**：`IExecutor` 已落地（P2a）且契约冻结；默认 `executor == nullptr` 仍同步（P3 翻转）。服务端取消表按 `(route, id)` 注册，`stop()` 取消全部，route 关闭取消该路由；客户端超时/断流取消 handler 仍属 P3。
2. **POSIX close 限制**：`StdioTransport::close()` 依赖 peer EOF；Windows 用 `CancelSynchronousIo`。
3. **未实现开关一律 FATAL**：`HEADER_ONLY` / `USE_SYSTEM_DEPS` / `ENABLE_LEGACY_SSE` / `CONFORMANCE` 置 ON 显式失败。
4. **协议代际未分层**：dispatcher 单层 + 版本字符串，未拆 `IProtocolAdapter`；P3 前评估是否需要拆分。
5. **P2b 简化**：每类资源/提示词 provider 最多一个（provider 自管分页/URI 归属）；客户端返回 `Json`（强类型 outcome 留 M4）；`resources/subscribe` 方法未实现，capability 不声明。
6. **HTTP client per-send client**：为并行订阅/调用每次 send 新建 `httplib::Client`（无 keep-alive/mTLS 复用）；记录在 §11，M4/M5 重新评估。
7. **2025 fallback 仅协商层**：client `discover` 失败后调用 `initialize`，但 transport 仍以 2026 头发送；严格 2025 peer 需 P3 的版本重协商。
8. **订阅 sender detach**：client `close()` 对订阅 sender detach（`cancelPending` 在 Windows 不保证立即解除阻塞）；回调已改 `weak_ptr`，无 Impl UAF。
9. **MRTR 2025**：handler 返回 `InputRequired` 在 2025 下返回 `-32603 input_required_unsupported`；2025 server→client 路径属 P3。
10. **HTTP 超时**：请求超时返回 504，但不会取消服务端 handler（P3）；admission `Emergency` 已映射 `Priority::Emergency`，默认执行器仍为同步。
11. **conformance**：外部套件未接；当前以自建 2026/2025 向量与 loopback 覆盖。

## 6. 下一步优先级

1. **P3（M3/M4）**：2025 session（`MCP-Session-Id`）、POST/GET/DELETE、per-stream `Last-Event-ID` replay、client session/重放、server→client 请求（roots/sampling/elicitation）、HTTP 超时取消 handler、默认执行器翻转（D17 前置条件）。
2. **P4（M5/M6 + conformance）**：auth/authorizer/approval/audit hooks、legacy HTTP+SSE shim（默认关闭）、conformance harness、header-only/system-deps 构建形态评估。

## 7. 维护约定

- 每完成一个里程碑或产生一次功能提交后更新本表。
- 新增/删除公共头、CMake target、`TUGMCP_*` 开关时，同步更新 `designs/` 与本表。
- 「部分」状态必须同时写缺口；不允许只写百分比。
- 设计与实现冲突时，先改 `designs/` 再改代码，并回填本表。
