# 路线图（Roadmap）

> 状态快照：**2026-10-23**（M1 完成）
> 本文是**项目级进度视图**，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。
> 状态口径：`已完成` / `部分` / `未开始`；标「部分」时必须写清缺口。

## 1. 里程碑状态（design §22.1）

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | core 类型/错误模型/严格 JSON-RPC；stdio server + client；echo tool；真实管道 loopback；非法 JSON/EOF/超长行用例；默认构建 + vendored 哈希校验；unit 测试集 |
| **M1.5** schema / canonical / conformance harness | 部分 | 已完成：`ISchemaValidator` + jsoncons 1.10（2020-12）+ Valijson 1.1.3（draft-07）、fail-closed、canonical 子集、`tool_schema_hash`（含独立哈希锚点）、默认 OFF / ON 两套构建全绿。未完成：**conformance harness 未接**（`TUGMCP_CONFORMANCE` 仍 FATAL） |
| **M2** 2026 server + client（HTTP/TLS） | 部分 | 明文 Streamable HTTP 已跑通：`POST /mcp` JSON-RPC→JSON，415/413/400(-32700/-32600/-32020/-32022)/202/200/405/404，`tugmcp::http`，HTTP loopback 端到端。未完成：**TLS**（`TUGMCP_TLS` 仍 FATAL）、SSE/`subscriptions/listen`、MRTR、resources/prompts、2025 session/replay；`admission`/并发流限制字段已标注未接线 |
| **M3** 2025 server/session | 未开始 | 仅有 `initialize` 子集；无 session / POST/GET/DELETE / per-stream replay |
| **M4** 2025 client/replay | 未开始 | 客户端无 session、`Last-Event-ID` replay、server→client 请求处理、cancel 路由 |
| **M5** 宿主适配 | 部分 | `IExecutor` 已提前落地（P2a，契约见 ADR D17，默认仍为同步 dispatch）；缺 auth/approval/audit hooks、默认执行器切换（P3） |
| **M6** legacy shim | 未开始 | `TUGMCP_ENABLE_LEGACY_SSE` 置 ON 即 `FATAL_ERROR` |

## 2. 设计章节逐节状态

| 章节 | 状态 | 说明 |
|---|---|---|
| §1–§3 目标 / 非目标 / 原则 | 已完成 | 设计文档已固化（含脱敏） |
| §4 协议与传输矩阵 | 部分 | 仅 stdio；2026 只有 `server/discover` 骨架；无 HTTP / legacy |
| §5 组件架构 | 部分 | `ITransport` / `ITimeSource` / server / client 已实现；**`IExecutor` 缺**；未拆 `IProtocolAdapter` |
| §6 模块与目录 | 部分 | 公共头 15 个（含 `schema.hpp`、`schema/{null,jsoncons,valijson}.hpp`、`canonical.hpp`）；缺 `transport/http.hpp`、`detail/http_httplib.hpp`、`protocol/v2026.hpp`、`protocol/v2025.hpp` |
| §6.1 CMake targets | 部分 | 已有 `tugmcp::core`、`tugmcp::tests`、`tugmcp::schema_jsoncons`、`tugmcp::schema_valijson`、`tugmcp::http`；缺 `protocol/*` 与 header-only/install 形态 |
| §7 核心类型与错误模型 | 已完成 | `RequestId`（int64 规范化 + 跨形态相等 + `std::hash`）、`McpError`/`Result`/`Status`、`ProtocolVersion`/`ProtocolEra`、`Message`；§7.3 的 `resultType` envelope 未做 |
| §7.4 严格 JSON | 已完成 | UTF-8、任意层重复 key、深度上限、非有限数、batch 拒绝；含正例与负例矩阵 |
| §8 并发模型 | 部分 | 单 reader + `send` 互斥；无执行器、队列、背压 |
| §9 stdio 传输 | 已完成 | 行 framing、CRLF、EOF 尾行、超长恢复、stdout 纯净、Windows 二进制模式、可中断 `close()`（POSIX 依赖 peer EOF） |
| §10 Streamable HTTP server | 部分 | 已实现 POST `/mcp` JSON 请求/响应：415/413/400 错误码/202/200/405/404、`Mcp-Method`/`Mcp-Name` 校验、重复 in-flight id → 409；缺 SSE、session、admission、并发流限制 |
| §11 Streamable HTTP client | 部分 | 已实现同步 POST、版本头可配置（默认 2025-11-25）、错误映射；缺 SSE/`openSubscription`、cancel、replay |
| §12 legacy HTTP+SSE | 未开始 | — |
| §13 协议适配 | 部分 | 已实现 `initialize` / `initialized` / `ping` / `tools.list` / `tools.call` / `server.discover`；缺 2026 headers/`_meta`/`resultType`/`ttlMs`/`cacheScope`、MRTR、`subscriptions/listen`、分页/cache、版本协商 |
| §14 Server API | 部分 | `McpServer(options, transport)` / `run` / `stop` / `tools`；缺 `createExecutor`、resources/prompts/templates、auth/approve/audit hooks |
| §15 Client API | 部分 | `connect` / `call` / `listTools` / `callTool` / `close`；缺 resources/prompts、cancel、progress、completion |
| §16 线程 / 背压 / 关闭 | 部分 | 关闭顺序与 cv 收尾已实现；无队列背压、无跨线程取消路由 |
| §17 schema / canonical JSON | 部分 | schema 可插拔（jsoncons/Valijson/Null）、fail-closed、canonical 子集、`tool_schema_hash` 已完成；conformance harness 未接 |
| §18 安全 | 未开始 | 无 auth / approval / audit、无 Origin/Host/rate 限制 |
| §19 测试与一致性 | 部分 | unit / integration / ODR 已有；conformance、覆盖率、完整矩阵未接 |
| §20 构建 / 依赖 / ABI | 部分 | CMake + 静态库 + 2 个 vendored 依赖 + SHA256 校验；`HEADER_ONLY` / `USE_SYSTEM_DEPS` / TLS / install 未实现 |
| §21 宿主适配边界 | 部分 | 文档已完成；代码按设计属 M5 |
| §22 里程碑 / 风险 / 未决 | 部分 | M1 完成；D16 偏差已入档 |

## 3. 关键设计要素

| 要素 | 状态 | 备注 |
|---|---|---|
| 静态库（默认产物） | 已完成 | `tugmcp::core` 为 STATIC |
| `IExecutor` | 已完成（P2a） | 三级优先、有界队列、`noexcept` 契约、`stats`/`waitIdle`、自我排除关闭；默认仍同步（P3 翻转，D17） |
| header-only 消费 | 未开始 | `TUGMCP_HEADER_ONLY=ON` → `FATAL_ERROR` |
| OpenSSL 可选编译 | 已完成 | `TUGMCP_TLS=ON`（OpenSSL ≥3.0）可构建并 loopback 通过；`TLS=OFF` 时 `https://` → `unsupported_scheme`。限制：server 侧无握手错误回调、SNI 由 URL host 决定 |
| `3rd/` vendoring | 部分 | nlohmann/json 3.12.0、doctest 2.5.3、cpp-httplib 0.63.0（单头哈希）；jsoncons 1.10.0、Valijson 1.1.3（逐文件 `MANIFEST.sha256`，177+49 文件） |
| `TUGMCP_USE_SYSTEM_DEPS` | 未开始 | → `FATAL_ERROR` |
| conformance 套件 | 未开始 | `TUGMCP_CONFORMANCE` → `FATAL_ERROR` |
| examples | 部分 | 仅 `echo_server`；`echo_client` 未做 |
| install / export | 未开始 | 设计 v1 明确不提供 |

## 4. 实现与验证证据

- **公共头（18）**：`core` / `version` / `error` / `message` / `transport` / `transport/stdio` / `transport/http` / `request_context` / `time_source` / `tool` / `server` / `client` / `executor` / `schema` / `canonical` / `schema/{null,jsoncons,valijson}`（另有实现专用 `detail/http_httplib.hpp`）
- **源码（16）**：`src/core/{version,error,message,codec,canonical,registry,dispatcher,server,executor,client}.cpp`、`src/core/dispatcher.hpp`、`src/transport/{stdio,http_server,http_client}.cpp`、`src/schema/{jsoncons,valijson}_validator.cpp`
- **测试**：11 个 unit 文件（含 `executor_test.cpp`）、4 个 integration 文件、双 TU ODR、`tests/helpers/fake_transport.hpp`
- **ctest**：默认 `TLS=OFF` **168/168**；`TLS=ON` **169/169**（含 2 个 TLS loopback 用例）
- **构建**：MSVC `/W4 /permissive- /utf-8` 无告警；configure 阶段校验单头 SHA256 + jsoncons/Valijson 逐文件清单（177+49）

| commit | 内容 |
|---|---|
| `3f52356` | `feat(core)`：构建骨架、vendored 依赖、错误模型、严格 JSON-RPC codec |
| `2468603` | `feat(stdio)`：registry、dispatcher、stdio server、echo example |
| `efe6d1d` | `feat(client)`：`McpClient`、pending/超时、端到端 loopback、并发用例 |
| `b43961f` | `feat(schema)`：M1.5 schema 校验、canonical JSON、`tool_schema_hash` |
| `22f2fbb` | `feat(http)`：Streamable HTTP JSON 传输、server/client、loopback 用例 |
| `2f359e0` | `feat(executor)`：`IExecutor`、线程池、off-thread dispatch（P2a） |
| `9426a6d` | `fix(executor)`：冻结契约（`noexcept`/`stats`/`waitIdle`/Emergency 绕过）、自我排除关闭、ADR D17 |

## 5. 已记录偏差

1. **D16/D17**：`IExecutor` 已落地（P2a）且契约冻结；默认 `executor == nullptr` 仍走同步 dispatch（P3 切默认线程池）。`RequestContext.cancellation` 目前仍无跨线程置位来源，`RequestId → token` 取消路由属 P2b。见 `designs/protocol-decisions.md`。
2. **POSIX close 限制**：POSIX 下 `StdioTransport::close()` 依赖 peer EOF；Windows 已用 `CancelSynchronousIo` 中断等待。
3. **未实现开关一律 FATAL**：`HEADER_ONLY` / `USE_SYSTEM_DEPS` / `ENABLE_LEGACY_SSE` / `CONFORMANCE` 置 ON 均显式失败，不静默降级；`WITH_HTTP` / `WITH_JSONCONS` / `WITH_VALIJSON` / `TLS` 已实现。
4. **协议代际未分层**：当前 dispatcher 单层 + 版本字符串，未拆 `IProtocolAdapter`。
5. **已知技术债**（Gate 3）：一次 `call` 约 12 次解析/序列化；`McpServer` 实现寄居 `dispatcher.cpp`；ODR 仅覆盖 3/9 公共头；`FakeTransport::emitClose` 未遵守「close 后不回调」；`core.hpp` umbrella 未收录新头。
6. **P2-B 技术债（已完成）**：`decode(const Json&)`、`ITransport::sendRaw`、`src/core/server.cpp` 拆分、ODR 覆盖全部公共头、`FakeTransport` close 后不再回调。
7. **HTTP stateless 约束**：pending 按 `RequestId` 关联，**并发客户端 id 必须全局唯一**（冲突 → 409）；工具在 HTTP 线程同步执行，**无执行超时**；`admission`/并发流限制字段已标注未接线。
8. **TLS（已完成）**：`TUGMCP_TLS=ON` 已接线并通过 loopback（OpenSSL 3.0、宏/链接、SSLServer/SSLClient、工厂校验、自签 fixtures）。限制：cpp-httplib 无 server 侧握手错误回调；`serverName`/`minVersion`/`privateKeyPassword` 未接线，**传入非默认值会显式失败**（`option_invalid`）；`McpClient::connect()` 的 `protocolVersion` 参数仍硬编码 `2025-11-25`。
9. **P2–P4 未开始**：M2 协议面（SSE/MRTR/subscriptions/resources/prompts/admission）、M3/M4（2025 session/replay）、M5/M6（executor/hooks/legacy）、conformance。

## 6. 下一步优先级

1. **P2b（M2 协议面）**：`resultType`/`ttlMs`/`cacheScope`、progress、resources/prompts/templates、分页、MRTR、`subscriptions/listen` + 推送、per-request SSE、`admission`/并发流限制、`echo_client`；同时收掉 Gate 2a 记录的 P2b 项（`run()` 异常收尾、执行器拒绝响应、取消路由、HTTP pending 命名空间、注入执行器集成测试）。
2. **P3（M3/M4）**：2025 session、GET/DELETE、per-stream replay、client cancel、server→client 请求；切换默认执行器（D17 前置条件满足后）。
3. **P4（M5/M6 + conformance）**：auth/approval/audit hooks、legacy HTTP+SSE shim（默认关闭）、conformance harness。

## 7. 维护约定

- 每完成一个里程碑或产生一次功能提交后更新本表。
- 新增/删除公共头、CMake target、`TUGMCP_*` 开关时，同步更新 `designs/` 与本表。
- 「部分」状态必须同时写缺口；不允许只写百分比。
- 设计与实现冲突时，先改 `designs/` 再改代码，并回填本表。
