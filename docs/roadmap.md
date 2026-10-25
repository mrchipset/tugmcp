# 路线图（Roadmap）

> 状态快照：**2026-10-25**（M1–M6 完成；Gate 2a/2b/3/4 关闭）
> 本文是项目进度视图，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。

## 1. 里程碑状态

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | strict JSON-RPC、stdio server/client、loopback、vendored 依赖 |
| **M1.5** schema / canonical / conformance | **已完成** | jsoncons/Valijson、canonical、`tool_schema_hash`；官方 conformance fixture+runner；2026 121/121、2025 73/73，0 failure / 0 unexpected skip；Inspector 2.12 stdio/HTTP 冒烟通过 |
| **M2** 2026 server + client | **已完成（Gate 2b）** | envelope/分页/resources/prompts/MRTR/progress/订阅/SSE/route 隔离/admission；D18 对齐官方 wire |
| **M3** 2025 server/session | **已完成（Gate 3）** | session/POST/GET/DELETE/replay/server→client；logging/setLevel、resources subscribe/unsubscribe（D19） |
| **M4** 2025 client/replay | **已完成（Gate 3）** | session、GET 流/replay、请求处理、cancel；2026 fallback |
| **M5** 宿主适配 | **已完成（Gate 4）** | `IExecutor`（默认内部池）+ `IAuthenticator`/`IAuthorizer`/`IApprovalHook`/`IAuditHook`（D20）；401/403、审批超时/取消、审计异常收敛 |
| **M6** legacy HTTP+SSE | **已完成（Gate 4）** | `TUGMCP_ENABLE_LEGACY_SSE`（默认 OFF）；server+client、endpoint/messages、batch 仅 legacy、auth/Host-Origin/上限 |
| **Amalgamation / release artifact** | **已完成（H3）** | `tugmcp.hpp` / `tugmcp_http.hpp` / `tugmcp_http_tls.hpp`；记录哈希、`SHA256SUMS`、`NOTICE`、`LICENSES/`；`amalgamate` / `amalgamate-release` target；双 TU/ODR、Inspector 2.12 与官方 conformance 外部验证 |

## 2. 最终验证（2026-10-25）

| 构建 | 结果 |
|---|---|
| Default Debug | **267/267**（H3 前 266/266；新增 `amalgamation.release`） |
| TLS (`TUGMCP_TLS=ON`) | **269/269**（H3 前 268/268） |
| Legacy (`TUGMCP_ENABLE_LEGACY_SSE=ON`) | **275/275**（H3 前 274/274） |
| Schema (`jsoncons+valijson`, `TUGMCP_WITH_HTTP=OFF`, `TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF`) | **198/198** 全量；`unit\|integration` **196/196**（H3 前口径） |
| Conformance (`ctest -R conformance`) | **2/2**：2026 121 required checks / 2025 73，均 0 failure / 0 unexpected skip |
| Amalgamation (`ctest -L amalgamation`) | **3/3**（default/TLS/legacy）：`amalgamation.core` / `amalgamation.release` / `amalgamation.http`；schema 构建为 2/2（外部 HTTP 检查显式关闭） |
| Inspector 2.12 | stdio/HTTP `initialize` + `tools/list` + `tools/call` 通过 |

告警：MSVC `/W4 /permissive- /utf-8` 无新增；仅 vendored `httplib.h` C4702。

## 3. 关键提交

| commit | 内容 |
|---|---|
| `2f359e0` / `9426a6d` | P2a executor + Gate 2a |
| `f20220f` … `05df1a3` | P2b：协议面/SSE/client/订阅 |
| `130df65` … `805bbab` | P3：2025 server/client/默认执行器 |
| `4c7d150` | echo server HTTP 模式 |
| `b01ff32` … `02b4587` | conformance 报告、D18/D19、fixture/runner、2026/2025 清零 |
| `270e94f` | roadmap：Gate 3 关闭 |
| `effd6db` / `a43720f` | M5 security hooks / M6 legacy + TLS 2025 |
| `6d6386f` | Gate 4 修补（legacy auth 隔离、审批超时、订阅授权、legacy 上限/Host-Origin） |
| `675d77c` | 单头生成器（core）与 consumer 测试 |
| `d3a272a` | 单头 profile 完整性、stdio/client 覆盖 |
| `605e7f3` | HTTP 单头产物与外部检查 |
| `1d37c5c` | TLS 单头产物、HTTP ODR、严格外部 gating |
| H3（本工作区） | 发布打包（`package_amalgamation.py` / `amalgamate-release`）、哈希/许可证/NOTICE、`amalgamation.release`、文档收口 |

## 4. 明确不在 v1 / 后续 backlog

1. **2026 `_meta.logLevel` 与自动 list_changed**：延后；当前 2025 logging 已实现，2026 `logging/setLevel` 明确 404。
2. **单头发布产物**：已完成（core/HTTP/TLS、哈希、许可证、外部验证）；**split-header `TUGMCP_HEADER_ONLY` INTERFACE 模式**仍未实现，ON 即 FATAL。
3. **`TUGMCP_USE_SYSTEM_DEPS`**：未实现。
4. **install/export**：设计 v1 明确不提供。
5. **HTTP client 连接复用**：per-send `httplib::Client`；无 keep-alive/mTLS 复用。
6. **客户端强类型 outcome**：client 仍返回 `Json`；typed API 留后续。
7. **legacy client reconnect**：legacy 无重连；POST 在全量 close 后表现为连接失败（410 路径仅服务未停时有效）。
8. **list 枚举不逐项过滤**：D20 明确列表公开；`IAuthorizer` 覆盖操作与 `resources/subscribe` URI，不覆盖 list。
9. **审计事件范围**：当前为 pre-execution 事件（`tool_call`/`resource_read`/`prompt_get`/deny/timeout/auth_failed）；结果/错误事件后续可加。
10. **认证错误码**：HTTP 401 + JSON-RPC `-32603` `data.reason="unauthorized"`；403 由 reason `forbidden`/`approval_denied`/`approval_timeout` 映射。
11. **CI/质量门禁**：sanitizer、覆盖率、GCC/Clang 矩阵、perf/soak 未接入。
12. **部署风险**：split-header `TUGMCP_HEADER_ONLY`、install、legacy 部署风险见 `designs/` 对应章节与 [`docs/amalgamation.md`](./amalgamation.md)。

## 5. 设计权威索引

- `designs/protocol-decisions.md`：D16/D17（executor）、D18（2026 conformance wire）、D19（2025 logging/subscriptions）、D20（security hooks）。
- `designs/mcp-library-design.md`：§10–§18、§12 legacy、§18 安全。
- `docs/conformance-report.md`：外部测试首轮证据与复现命令。
- `docs/amalgamation.md`：单头产物矩阵、消费/链接、TLS 宏/ODR、发布与哈希/许可证政策。
- `.slim/deepwork/tugmcp-conformance-p4.md`：本轮执行状态（本地，gitignored）。

## 6. 维护约定

- 新增/修改公共头、wire 行为、CMake target 或 `TUGMCP_*` 开关时，同步更新 `designs/` 与本表。
- 设计与实现冲突时，先改 `designs/` 再改代码。
- “部分/延后”必须写清缺口；不允许只写百分比。
