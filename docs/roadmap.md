# 路线图（Roadmap）

> 状态快照：**2026-10-24**（Gate 3 通过，M3/M4 完成）
> 本文是**项目级进度视图**，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。
> 状态口径：`已完成` / `部分` / `未开始`；标「部分」时必须写清缺口。

## 1. 里程碑状态（design §22.1）

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | core 类型/错误模型/严格 JSON-RPC；stdio server + client；echo tool；真实管道 loopback；非法 JSON/EOF/超长行用例；vendored 哈希校验 |
| **M1.5** schema / canonical / conformance harness | 部分 | jsoncons 1.10 + Valijson 1.1.3、fail-closed、canonical 子集、`tool_schema_hash`。缺口：**外部 conformance harness 未接**（`TUGMCP_CONFORMANCE` 仍 FATAL） |
| **M2** 2026 server + client（HTTP/TLS） | **已完成（P2b，Gate 2b）** | 2026 headers/`_meta`、`resultType`/`ttlMs`/`cacheScope`、progress、resources/prompts/templates、分页、MRTR、`subscriptions/listen`+ack/filter+推送、per-request SSE、route 隔离、admission/并发流限制、discover/2025 回退、client SSE/progress/MRTR/cancel/订阅、`echo_client` |
| **M3** 2025 server/session | **已完成（P3，Gate 3）** | `initialize` + `MCP-Session-Id`、POST 校验/expiry/429、GET SSE + 256 条 replay + `Last-Event-ID`、DELETE 204、`requestClient`/`sendNotification`、session 级取消 |
| **M4** 2025 client/replay | **已完成（P3，Gate 3）** | session 捕获/附加、GET 流管理/退避重连/replay、server→client 请求处理、全局通知、timeout/cancel 的 `notifications/cancelled`、2026→2025 回退 |
| **M5** 宿主适配 | 部分 | `IExecutor` 已完成且**默认内部线程池**（4/1024，D17；`useInlineExecutor` 可退回同步）；缺 auth/approval/audit hooks（P4） |
| **M6** legacy shim | 未开始 | `TUGMCP_ENABLE_LEGACY_SSE` 置 ON 即 `FATAL_ERROR`（P4） |

## 2. 设计章节逐节状态

| 章节 | 状态 | 说明 |
|---|---|---|
| §1–§3 目标 / 非目标 / 原则 | 已完成 | 设计已固化 |
| §4 协议与传输矩阵 | 已完成（legacy 除外） | stdio + Streamable HTTP（JSON/SSE/2025 session）；legacy 2024 未做 |
| §5 组件架构 | 部分 | `ITransport`/`ITimeSource`/`IExecutor`/server/client；未拆 `IProtocolAdapter`（版本行为集中在 dispatcher） |
| §6 模块与目录 | 已完成（公共头） | 21 个公共头 |
| §6.1 CMake targets | 部分 | core/tests/executor_tests/schema_*/http；header-only/install 未做 |
| §7 核心类型与错误模型 | 已完成 | `resultType`/`OperationOutcome`/`Page`/`Progress`/`Subscription*` |
| §7.4 严格 JSON | 已完成 | UTF-8、重复 key、深度、非有限数、batch 拒绝 |
| §8 并发模型 | 已完成（M3/M4 范围） | 单 reader + 默认执行器 + 有界队列 + 取消注册；route/session 级取消 |
| §9 stdio 传输 | 已完成 | 行 framing、CRLF、EOF、超长恢复、Windows 中断 |
| §10 Streamable HTTP server | 已完成 | JSON/SSE、route 隔离、admission 429、并发流限制、2025 session/GET/DELETE/replay |
| §11 Streamable HTTP client | 部分 | SSE、2025 session、GET 流/replay、cancel；**per-send client**（无 keep-alive），2026 cancel 无 session 作用域 |
| §12 legacy HTTP+SSE | 未开始 | P4 |
| §13 协议适配 | 已完成（M2–M4） | 2026/2025 envelope、MRTR、订阅、session/replay、server→client 请求 |
| §14 Server API | 部分 | tools/providers/notify/requestClient/sendNotification/run/stop；缺 auth/approval/audit hooks |
| §15 Client API | 部分 | discover/initialize/list*/read/get/callTool/subscribe/cancel/stream/request handlers；返回 `Json`（强类型 outcome 留后续） |
| §16 线程 / 背压 / 关闭 | 部分 | 执行器/队列/SSE 背压/route-session 清理；客户端流错误异步上报 |
| §17 schema / canonical JSON | 部分 | conformance harness 未接 |
| §18 安全 | 未开始 | auth/approval/audit、Origin/Host/rate（P4） |
| §19 测试与一致性 | 部分 | unit/integration/ODR 237/238；conformance/覆盖率未接 |
| §20 构建 / 依赖 / ABI | 部分 | 静态库 + vendored + SHA256；header-only/system-deps/install 未做 |
| §21 宿主适配边界 | 部分 | 文档完成；hooks 属 P4 |
| §22 里程碑 / 风险 / 未决 | 部分 | M2–M4 完成；D17/P3 偏差已入档 |

## 3. 关键设计要素

| 要素 | 状态 | 备注 |
|---|---|---|
| 静态库（默认产物） | 已完成 | `tugmcp::core` 为 STATIC |
| `IExecutor` | 已完成（P3 默认为内部池） | 4 workers/1024 队列；`useInlineExecutor=true` 退回 transport 线程；`stats`/`waitIdle`/Emergency |
| 2026 协议面 | 已完成（P2b） | envelope、分页、resources/prompts、MRTR、progress、订阅 |
| 2025 session/replay | 已完成（P3） | 32-hex session、GET 单活流 + 256 replay、DELETE、server→client |
| SSE / 背压 / admission | 已完成 | per-request SSE、route 隔离、429、有界队列、池 `max(8, streams+4)` |
| HTTP client | 部分 | SSE/session/replay/cancel；per-send client、2026 cancel 无 session 作用域 |
| header-only 消费 | 未开始 | `TUGMCP_HEADER_ONLY=ON` → FATAL |
| OpenSSL 可选编译 | 已完成 | TLS=ON loopback；TLS=OFF 时 https 显式失败 |
| `3rd/` vendoring | 部分 | nlohmann/json 3.12.0、doctest 2.5.3、cpp-httplib 0.63.0、jsoncons 1.10.0、Valijson 1.1.3 |
| `TUGMCP_USE_SYSTEM_DEPS` | 未开始 | FATAL |
| conformance 套件 | 未开始 | FATAL（P4） |
| examples | 已完成 | `echo_server`（stdio）+ `echo_client`（HTTP 2026） |
| install / export | 未开始 | 设计 v1 明确不提供 |

## 4. 实现与验证证据

- **公共头（21）**：core/version/error/message/transport/transport/{stdio,http}/request_context/time_source/tool/operation/resource/prompt/server/client/executor/schema/canonical/schema/{null,jsoncons,valijson}
- **源码（17）**：`src/core/{version,error,message,codec,canonical,registry,dispatcher,server,executor,client,operation}.cpp`、`dispatcher.hpp`、`src/transport/{stdio,http_server,http_client}.cpp`、`src/schema/{jsoncons,valijson}_validator.cpp`
- **测试**：12 个 unit 文件 + 独立 `tugmcp_executor_tests`（integration，10s）+ 10 个 integration 文件 + 双 TU ODR
- **ctest**：默认 `TLS=OFF` **237/237**；`TLS=ON` **238/238**；schema 构建 **191/191**
- **构建**：MSVC `/W4 /permissive- /utf-8` 无新告警（仅 vendored httplib C4702）

| commit | 内容 |
|---|---|
| `3f52356` … `22f2fbb` | M1 / M1.5 / HTTP JSON 骨架 |
| `2f359e0` / `9426a6d` | P2a `IExecutor` + Gate 2a 修补 |
| `f20220f` / `d3c59f4` / `c84c3e6` / `05df1a3` | P2b：协议面、SSE/route、client 2026、订阅 |
| `c32a6bf` / `d7f2e39` | Gate 2b Critical/Major 修补 |
| `7351735` | roadmap：M2 完成 |
| `130df65` / `5465421` / `805bbab` | P3：2025 server session、client session、默认执行器 |
| `7592c2d` / `44021b6` | Gate 3 修补（session 作用域、replay 原子性、fallback、精确 cancel） |

## 5. 已记录偏差

1. **D16/D17**：默认执行器已切内部线程池（4/1024）；`useInlineExecutor=true` 保留同步；同一进程 handler 可并行于 4 线程。
2. **POSIX close**：`StdioTransport::close()` 依赖 peer EOF；Windows 用 `CancelSynchronousIo`。
3. **未实现开关一律 FATAL**：`HEADER_ONLY` / `USE_SYSTEM_DEPS` / `ENABLE_LEGACY_SSE` / `CONFORMANCE`。
4. **协议代际未分层**：dispatcher 单层 + 版本字符串，未拆 `IProtocolAdapter`。
5. **P2b/P3 简化**：每类 provider 最多一个；client 返回 `Json`；`resources/subscribe` 未实现；2026 无 session，`notifications/cancelled` 服务端按 id 尽力匹配且客户端不发。
6. **HTTP client per-send client**：无 keep-alive/mTLS 复用；连接复用留 M4/M5。
7. **2025 session wire**：32-hex id；缺失/未知 session → 400/404；`maxSessions=128`、30 分钟过期、超限 429；GET 单活流、256 条 replay、事件 id 单调；新 GET 无 Last-Event-ID 会重放整个窗口（文档化）；DELETE 204。
8. **server→client 请求**：`requestClient` 超时/关闭分别返回 `timeout`/`session_closed`；响应必须来自同一 session。
9. **TLS+2025 覆盖**：TLS loopback 仍走 2026；2025-over-TLS 的 session/stream 无独立用例（M4 补）。
10. **客户端流错误**：`openStream` 异步上报 404/405（`onError`），不返回同步失败；重连退避 100/250/1000ms。
11. **conformance**：外部套件未接，当前以自建 2026/2025 向量与 loopback 覆盖。
12. **MRTR 2025**：handler 返回 `InputRequired` 在 2025 返回 `-32603 input_required_unsupported`。

## 6. 下一步优先级（P4）

1. **M5 hooks**：`IAuthenticator`/`IAuthorizer`/`IApprovalHook`/`IAuditHook`，失败路径测试；auth 失败走 HTTP 401。
2. **M6 legacy**：HTTP+SSE（`GET /sse` + `POST /messages`）默认关闭；与非 legacy 路径隔离。
3. **conformance**：接 `TUGMCP_CONFORMANCE`，跑 MCP Inspector/官方套件 + 自定义 2026 向量。
4. **收尾**：`openStream` 同步错误、TLS+2025 用例、header-only/system-deps 形态评估、最终 roadmap。

## 7. 维护约定

- 每完成一个里程碑或产生一次功能提交后更新本表。
- 新增/删除公共头、CMake target、`TUGMCP_*` 开关时，同步更新 `designs/` 与本表。
- 「部分」状态必须同时写缺口；不允许只写百分比。
- 设计与实现冲突时，先改 `designs/` 再改代码，并回填本表。
