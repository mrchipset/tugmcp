# 路线图（Roadmap）

> 状态快照：**2026-10-25**（Gate 3 通过；conformance 双版本全绿）
> 本文是项目进度视图，不是设计权威；与 `designs/` 冲突时以 `designs/` 为准。

## 1. 里程碑状态

| 里程碑 | 状态 | 落地情况 |
|---|---|---|
| **M1** core + stdio | **已完成** | strict JSON-RPC、stdio server/client、loopback、vendored 依赖 |
| **M1.5** schema / canonical / conformance | **已完成** | jsoncons/Valijson、canonical、`tool_schema_hash`；官方 conformance runner + HTTP fixture server；**2026-07-28 121/121，2025-11-25 73/73，0 failure / 0 unexpected skip**；Inspector 2.12 stdio/HTTP 冒烟通过 |
| **M2** 2026 server + client | **已完成（Gate 2b）** | envelope/分页/resources/prompts/MRTR/progress/订阅/SSE/route 隔离/admission；D18 对齐官方 2026 wire |
| **M3** 2025 server/session | **已完成（Gate 3）** | session/POST/GET/DELETE/replay/server→client；2025 logging/setLevel、resources subscribe/unsubscribe（D19） |
| **M4** 2025 client/replay | **已完成（Gate 3）** | session 捕获、GET 流/replay、请求处理、cancel；2026 fallback |
| **M5** 宿主适配 | 部分 | `IExecutor` 已完成（默认内部池）；缺 auth/authorizer/approval/audit hooks（P4） |
| **M6** legacy HTTP+SSE | 未开始 | P4 |

## 2. 关键要素

| 要素 | 状态 | 备注 |
|---|---|---|
| 静态库 / CMake targets | 已完成 | core/http/schema/tests/executor/conformance fixture |
| 2026 stateless wire | 已完成 | D18：namespaced `_meta`、404 removed methods、订阅 `_meta.subscriptionId`、completion、Host/Origin |
| 2025 session/logging/subscriptions | 已完成 | D19 |
| Conformance | 已完成 | `TUGMCP_CONFORMANCE=ON`；2026/2025 两个 ctest；fixture server 仅测试资产 |
| Inspector 外部验证 | 已完成 | stdio/HTTP initialize+tools/list+tools/call |
| header-only / 单头 | 未开始 | `TUGMCP_HEADER_ONLY=ON` → FATAL |
| system deps | 未开始 | FATAL |
| legacy SSE | 未开始 | P4 |
| auth/approval/audit hooks | 未开始 | P4 |
| install/export | 不提供 | 设计 v1 明确不提供 |
| TLS | 已完成（OpenSSL ≥3.0） | TLS=ON 测试通过；TLS+2025 session 专项待补 |
| 连接复用 | 未做 | client per-send client（M4/M5 复查） |

## 3. 验证证据

- `ctest --test-dir build -C Debug` → **255/255**
- `ctest --test-dir build-tls -C Debug` → **256/256**
- schema 构建 → **195/195**
- `ctest --test-dir build-conformance -C Debug -R conformance` → **2/2**
  - 2026-07-28：required 37，seen 50，121 checks，0 failure / 0 unexpected skip
  - 2025-11-25：required 31，seen 33，73 checks，0 failure / 0 unexpected skip
- 结果目录：`build/conformance/{2026-07-28,2025-11-25}`

## 4. 关键提交

| commit | 内容 |
|---|---|
| `2f359e0` / `9426a6d` | P2a executor + Gate 2a |
| `f20220f` … `05df1a3` | P2b：协议面/SSE/client/订阅 |
| `c32a6bf` / `d7f2e39` | Gate 2b 修补 |
| `130df65` … `805bbab` | P3：2025 server/client/默认执行器 |
| `7592c2d` / `44021b6` | Gate 3 修补 |
| `4c7d150` | echo server HTTP 模式 |
| `b01ff32` | conformance 报告 + D18 设计对齐 |
| `52e9f8c` / `62dfd03` | D18 实现 + Gate 2 修补 |
| `d974dcd` | fixture server + runner；2026/2025 清零 |
| `7b6e15d` / `02b4587` | Gate 3 修补 + pycache 忽略 |

## 5. 已记录偏差

1. **D17**：默认执行器为内部 4/1024 线程池；`useInlineExecutor` 可退回同步。
2. **D18**：2026 `_meta`/订阅/404/错误 id/版本错误/completion/Host-Origin 已对齐；`list_changed` 自动通知与 `_meta.logLevel` 延后 P4。
3. **D19**：2025 logging 与 per-URI 资源订阅已实现；2026 仍不支持 per-URI。
4. **conformance runner**：官方 `server` 仅 URL/HTTP；stdio 外部验证用 Inspector；fixture server 仅测试构建。
5. **per-send HTTP client**：无 keep-alive；订阅流独立 client。
6. **客户端返回 `Json`**：强类型 outcome 留后续。
7. **2025 replay**：每 session 单活流 + 256 条；不是多 per-stream。
8. **TLS+2025**：session/stream 无独立 TLS 用例（P4）。
9. **header-only / system deps / install**：未实现/不提供。

## 6. 下一步（P4）

1. **M5 hooks**：`IAuthenticator`/`IAuthorizer`/`IApprovalHook`/`IAuditHook`；401/403、失败路径测试；审计异常收敛。
2. **M6 legacy**：HTTP+SSE（`GET /sse` + `POST /messages`），默认关闭；batch 仅 legacy。
3. **收尾**：TLS+2025 session/stream 用例、2026 `_meta.logLevel`/自动 list_changed、header-only/system-deps 评估、最终验证。
