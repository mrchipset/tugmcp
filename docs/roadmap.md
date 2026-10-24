# 路线图（Roadmap）

> 状态快照：**2026-10-23**（M1 完成）
> 本文是**项目级进度视图**，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。
> 状态口径：`已完成` / `部分` / `未开始`；标「部分」时必须写清缺口。

## 1. 里程碑状态（design §22.1）

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | core 类型/错误模型/严格 JSON-RPC；stdio server + client；echo tool；真实管道 loopback；非法 JSON/EOF/超长行用例；默认构建 + vendored 哈希校验；unit 测试集 |
| **M1.5** schema / canonical / conformance harness | 部分 | 已完成：`ISchemaValidator` + jsoncons 1.10（2020-12）+ Valijson 1.1.3（draft-07）、fail-closed、canonical 子集、`tool_schema_hash`（含独立哈希锚点）、默认 OFF / ON 两套构建全绿。未完成：**conformance harness 未接**（`TUGMCP_CONFORMANCE` 仍 FATAL） |
| **M2** 2026 server + client（HTTP/TLS） | 未开始 | `TUGMCP_WITH_HTTP`、`TUGMCP_TLS` 置 ON 即 `FATAL_ERROR` |
| **M3** 2025 server/session | 未开始 | 仅有 `initialize` 子集；无 session / POST/GET/DELETE / per-stream replay |
| **M4** 2025 client/replay | 未开始 | 客户端无 session、`Last-Event-ID` replay、server→client 请求处理、cancel 路由 |
| **M5** 宿主适配 | 未开始 | 无 `IExecutor`、auth/approval/audit hooks（见 §5 偏差 1） |
| **M6** legacy shim | 未开始 | `TUGMCP_ENABLE_LEGACY_SSE` 置 ON 即 `FATAL_ERROR` |

## 2. 设计章节逐节状态

| 章节 | 状态 | 说明 |
|---|---|---|
| §1–§3 目标 / 非目标 / 原则 | 已完成 | 设计文档已固化（含脱敏） |
| §4 协议与传输矩阵 | 部分 | 仅 stdio；2026 只有 `server/discover` 骨架；无 HTTP / legacy |
| §5 组件架构 | 部分 | `ITransport` / `ITimeSource` / server / client 已实现；**`IExecutor` 缺**；未拆 `IProtocolAdapter` |
| §6 模块与目录 | 部分 | 公共头 15 个（含 `schema.hpp`、`schema/{null,jsoncons,valijson}.hpp`、`canonical.hpp`）；缺 `transport/http.hpp`、`detail/http_httplib.hpp`、`protocol/v2026.hpp`、`protocol/v2025.hpp` |
| §6.1 CMake targets | 部分 | 已有 `tugmcp::core`、`tugmcp::tests`、`tugmcp::schema_jsoncons`、`tugmcp::schema_valijson`；缺 `tugmcp::http` |
| §7 核心类型与错误模型 | 已完成 | `RequestId`（int64 规范化 + 跨形态相等 + `std::hash`）、`McpError`/`Result`/`Status`、`ProtocolVersion`/`ProtocolEra`、`Message`；§7.3 的 `resultType` envelope 未做 |
| §7.4 严格 JSON | 已完成 | UTF-8、任意层重复 key、深度上限、非有限数、batch 拒绝；含正例与负例矩阵 |
| §8 并发模型 | 部分 | 单 reader + `send` 互斥；无执行器、队列、背压 |
| §9 stdio 传输 | 已完成 | 行 framing、CRLF、EOF 尾行、超长恢复、stdout 纯净、Windows 二进制模式、可中断 `close()`（POSIX 依赖 peer EOF） |
| §10 Streamable HTTP server | 未开始 | — |
| §11 Streamable HTTP client | 未开始 | — |
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
| header-only 消费 | 未开始 | `TUGMCP_HEADER_ONLY=ON` → `FATAL_ERROR` |
| OpenSSL 可选编译 | 未开始 | `TUGMCP_TLS` → `FATAL_ERROR` |
| `3rd/` vendoring | 部分 | nlohmann/json 3.12.0、doctest 2.5.3（单头哈希）；jsoncons 1.10.0、Valijson 1.1.3（逐文件 `MANIFEST.sha256`，共 177+49 文件）；cpp-httplib 0.63.0 未引入 |
| `TUGMCP_USE_SYSTEM_DEPS` | 未开始 | → `FATAL_ERROR` |
| conformance 套件 | 未开始 | `TUGMCP_CONFORMANCE` → `FATAL_ERROR` |
| examples | 部分 | 仅 `echo_server`；`echo_client` 未做 |
| install / export | 未开始 | 设计 v1 明确不提供 |

## 4. 实现与验证证据

- **公共头（11）**：`core` / `version` / `error` / `message` / `transport` / `transport/stdio` / `request_context` / `time_source` / `tool` / `server` / `client`
- **源码（9）**：`src/core/{version,error,message,codec,registry,dispatcher,client}.cpp`、`src/core/dispatcher.hpp`、`src/transport/stdio.cpp`
- **测试**：8 个 unit 文件、3 个 integration 文件、双 TU ODR、`tests/helpers/fake_transport.hpp`
- **ctest**：`105/105` 通过（unit 98 + integration 7，约 1.7s）
- **构建**：MSVC `/W4 /permissive- /utf-8` 无告警；configure 阶段校验 7 个 vendored 文件 SHA256

| commit | 内容 |
|---|---|
| `3f52356` | `feat(core)`：构建骨架、vendored 依赖、错误模型、严格 JSON-RPC codec |
| `2468603` | `feat(stdio)`：registry、dispatcher、stdio server、echo example |
| `efe6d1d` | `feat(client)`：`McpClient`、pending/超时、端到端 loopback、并发用例 |

## 5. 已记录偏差

1. **D16（临时）**：M1 无 `IExecutor`，handler 在 transport reader 线程同步执行；`RequestContext.cancellation` 恒非空但无跨线程置位来源。M5 替换。见 `designs/protocol-decisions.md`。
2. **POSIX close 限制**：POSIX 下 `StdioTransport::close()` 依赖 peer EOF；Windows 已用 `CancelSynchronousIo` 中断等待。
3. **未实现开关一律 FATAL**：`WITH_HTTP` / `TLS` / `HEADER_ONLY` / `USE_SYSTEM_DEPS` / `WITH_JSONCONS` / `WITH_VALIJSON` / `ENABLE_LEGACY_SSE` / `CONFORMANCE` 置 ON 均显式失败，不静默降级。
4. **协议代际未分层**：当前 dispatcher 单层 + 版本字符串，未拆 `IProtocolAdapter`。
5. **已知技术债**（Gate 3）：一次 `call` 约 12 次解析/序列化；`McpServer` 实现寄居 `dispatcher.cpp`；ODR 仅覆盖 3/9 公共头；`FakeTransport::emitClose` 未遵守「close 后不回调」；`core.hpp` umbrella 未收录新头。

## 6. 下一步优先级

1. **M2 前技术债（短阶段）**
   - 新增 `Result<Message> decode(const Json&)` 与 `ITransport::sendRaw(std::string_view)`，消除 dump/parse 往返。
   - 从 `dispatcher.cpp` 拆出 `src/core/server.cpp`。
   - ODR 覆盖全部公共头；`core.hpp` 收录新头；修正 `FakeTransport::emitClose` 契约。
2. **M1.5**：`schema/`（jsoncons / Valijson / null）、canonical JSON（JCS 或文档化子集）、`tool_schema_hash`、conformance harness 接线。
3. **M2**：Streamable HTTP server/client + `TUGMCP_TLS`（OpenSSL ≥ 3.0）+ 2026 headers/`_meta`/`resultType`/MRTT/订阅。
4. **M3 / M4**：2025 session、GET/DELETE、per-stream replay、server→client 请求。
5. **M5 / M6**：`IExecutor` 与宿主 hooks；legacy shim。

## 7. 维护约定

- 每完成一个里程碑或产生一次功能提交后更新本表。
- 新增/删除公共头、CMake target、`TUGMCP_*` 开关时，同步更新 `designs/` 与本表。
- 「部分」状态必须同时写缺口；不允许只写百分比。
- 设计与实现冲突时，先改 `designs/` 再改代码，并回填本表。
