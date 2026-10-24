# 协议与架构决策（ADR）

> 这些是 `tugmcp` 的已定决策。变更必须先改本文件，再改实现。
>
> 访问/参考日期：2026-10-22。

## 决策总表

| ID | 决策 | 状态 |
|---|---|---|
| D1 | 协议以 2026-07-28 为主，2025-11-25 回退 | 已定 |
| D2 | HTTP 传输使用 Streamable HTTP，不使用旧 HTTP+SSE 作为主路径 | 已定 |
| D3 | 同时提供 stdio 与 Streamable HTTP（server + client） | 已定 |
| D4 | 不提供官方“RESTful MCP”；MCP 是 JSON-RPC | 已定 |
| D5 | Wire JSON 用 nlohmann/json 3.12.0 | 已定 |
| D6 | HTTP/SSE 用 cpp-httplib，固定 commit + SHA256 | 已定 |
| D7 | Schema 校验可插拔；2020-12 用 jsoncons，draft-07 用 Valijson | 已定 |
| D8 | Canonical JSON 用 RFC 8785 JCS 或文档化稳定子集 | 已定 |
| D9 | 默认构建产物为静态库；header-only 为显式 opt-in（`TUGMCP_HEADER_ONLY=ON`）；单头文件只作为生成的 amalgamation | 已定 |
| D10 | 库不拥有设备安全/任务/业务；auth/approval/audit/safety 由应用提供 | 已定 |
| D11 | C++17、无 GUI 依赖 core、CMake-only、静态库优先、`mcp::v1` | 已定 |
| D12 | 协议验收用 conformance 0.2.0-alpha.12（先验证 2026 覆盖）+ 自定义 vectors | 已定 |
| D13 | 纯 C++ 工具链：CMake ≥3.20 唯一构建系统；MSVC 2019 16.8+/GCC 9+/Clang 10+；不提供其它构建系统适配 | 已定 |
| D14 | TLS 为构建开关（`TUGMCP_TLS`，默认 OFF；OpenSSL ≥3.0）；宏只经 `tugmcp::http` 传播 | 已定 |
| D15 | 第三方依赖固定在 `3rd/`，手写 INTERFACE target；默认不用系统包、不用 FetchContent | 已定 |
| D16 | M1 临时：无 `IExecutor`，handler 在 transport reader 线程同步执行；M5 替换 | 部分取代（D17） |
| D17 | `IExecutor` 公共契约冻结；默认执行器在 P2b 保持 `nullptr`，P3 切换为内部线程池 | 已定 |

## D1 — 2026 主、2025 回退

**背景**：2026-07-28 是 stateless（无 initialize/session/GET SSE/Last-Event-ID/ping，新增 `server/discover`、`resultType`、`subscriptions/listen`、MRTR）；2025-11-25 仍有大量客户端。

**决策**：`supportedVersions = {2026-07-28, 2025-11-25}`；2026 为主，2025 为回退。2025-06-18 不单列 target，可在 2025 adapter 做显式 compat flag。

**后果**：
- 需要两个 `IProtocolAdapter` 实现；core 不写散落的版本分支。
- 2026 的能力（MRTR、`subscriptions/listen`、cache fields）不能假设 2025 客户端支持。
- 2025 的 session/GET/replay 不能泄漏到 2026 handler。

## D2 — Streamable HTTP 是主 HTTP 传输

**背景**：旧 HTTP+SSE（`GET /sse` + `POST /messages`）自 2025-03-26 起被 Streamable HTTP 取代，2026-07-28 正式 deprecated。

**决策**：主路径用 Streamable HTTP：单 `POST /mcp`，响应 JSON 或 `text/event-stream`；2026 无 GET/DELETE，2025 可选。

**后果**：
- SSE 作为响应形态仍保留；不要把它和旧双端点传输混为一谈。
- 2026 的资源更新走 `subscriptions/listen`，不是无请求的逐条推送。
- 旧传输只做可选 legacy adapter。

## D3 — stdio 与 HTTP 并存

**背景**：stdio 是客户端启动子进程的本地传输；HTTP 适合已运行的应用/远程客户端。

**决策**：库同时实现 stdio 与 Streamable HTTP 的 server/client。

**后果**：
- stdio 需要 stdout 纯净、stderr 日志、Windows binary mode、EOF/背压。
- 对已运行的桌面 GUI 宿主应用，stdio 不是首选；用 loopback HTTP 或 stdio↔HTTP bridge，不启动第二个宿主实例。
- 通用库保留 stdio 以提高客户端覆盖。

## D4 — 不做 RESTful MCP

**背景**：MCP 是 JSON-RPC 2.0；`tools/list`、`resources/read` 不是 HTTP REST 资源。

**决策**：不提供官方 REST/OpenAPI 映射；如需要 REST，另做 facade 复用服务层。

**后果**：
- 不引入第二套语义/路由/错误模型。
- 第三方 REST↔MCP gateway 不属于本库。

## D5 — nlohmann/json 3.12.0

**背景**：header-only、C++11、API 现代、C++ MCP 生态常用；避免编译库 ABI。

**决策**：wire JSON 用 nlohmann/json **3.12.0**；公共 API 暴露 `Json`，接受版本/ABI 约束。

**后果**：
- 消费方必须使用兼容版本；DLL 边界不建议暴露 nlohmann 类型。
- 需要固定版本并做 MSVC/GCC/Clang 冒烟。

## D6 — cpp-httplib 固定 commit + SHA256

**背景**：cpp-httplib 是 header-only，但 API/行为随版本变化；当前稳定 v0.63.0。

**决策**：固定 commit + SHA256（当前稳定 v0.63.0，v0.62.0 作 fallback）。TLS 是构建开关（`TUGMCP_TLS`，默认 OFF），要求 OpenSSL ≥ 3.0；1.1.1 不受支持。

**后果**：
- 构建可复现；升级需回归 HTTP/SSE/超时/背压。
- 默认 loopback 明文；远程由外部 TLS 终止。
- 不要 `add_subdirectory` 上游 httplib：其 CMake 默认自动探测 OpenSSL 且生成 install 规则。

## D7 — Schema 校验可插拔

**背景**：MCP 要求校验 `inputSchema`；nlohmann 不自带 JSON Schema 校验。

**决策**：`ISchemaValidator` 可插拔；2020-12 用 jsoncons 1.10（可选 target）；draft-07 用 Valijson 1.1.3（显式声明）；无 validator 且未允许时 fail closed；`NullSchemaValidator` 仅测试。

**后果**：
- 不虚假宣称 `tugmcp::core` 内置 2020-12。
- `$ref`/dialect/keyword 子集需在选型时用目标 schema 验证。
- 工具注册时编译 schema，调用时校验。

## D8 — Canonical JSON

**背景**：`tool_schema_hash`、审计哈希需要稳定字节序列；raw `dump()` 受键序/空格/浮点格式影响。

**决策**：实现 RFC 8785 JCS 或文档化稳定子集；`tool_schema_hash` 不直接 hash `dump()`。

**后果**：
- 需要 canonicalizer 和测试（键排序、数字、UTF-8、无 NaN/Inf）。
- 子集必须文档化，不能声称完整 JCS。

## D9 — Header-only 策略

**背景**：目标是易嵌入、可复用；header-only 可消除库 ABI，但增加编译时间/暴露实现。默认静态库可保证编译隔离与稳定构建，header-only 作为显式可选项。

**决策**：**默认构建产物为静态库；同时支持以 header-only 方式消费（`TUGMCP_HEADER_ONLY=ON` 显式开启）**；核心/协议/stdio 可在 header-only 模式下使用；`transport/http.hpp` 仅声明（不 include cpp-httplib），header-only 实现位于 `detail/http_httplib.hpp`；单头文件 `tugmcp.hpp` 只作为生成的 amalgamation。

**后果**：
- `TUGMCP_HEADER_ONLY=ON` 只切换**库** target（`core`/`http`/`schema_*`）为 INTERFACE（不编译 `src/`）。
- `tugmcp::tests` 是 **INTERFACE helper 库**（fake transport / conformance helpers / fixture 头，INTERFACE 传播 `tugmcp_3rd_doctest`）；测试可执行文件（`tugmcp_unit_tests` 等）为独立 target，**始终编译**，不受 `TUGMCP_HEADER_ONLY` 影响。
- 需要 ODR/双 TU 测试、inline/constexpr 规范、Windows 链接标志。
- 编译时间/二进制大小需测量；不能盲目单头化。
- 详见 `header-only.md`。

## D10 — 安全边界

**背景**：设备安全、任务、参数语义属于应用；库无法替应用做安全结论。

**决策**：库只提供 `IAuthenticator`/`IAuthorizer`/`IApprovalHook`/`IAuditHook` 和 `RequestContext`；硬件联锁/审批/审计策略在应用/硬件层。

**后果**：
- 文档/API 不声称“库保证设备安全”。
- 审批必须有 server-side record（UI 或 MRTR），`confirmDangerous` 只是提示。
- 紧急请求走应用 admission，不承诺硬实时抢占。

## D11 — C++17 / 无 GUI 依赖 / CMake-only / static 优先

**背景**：目标是多项目复用；不绑定任何 GUI 工具包或特定编译器厂商。

**决策**：C++17；core 不依赖 GUI 框架；CMake（≥3.20）为唯一构建系统，不提供其它构建系统适配文件；静态库优先；命名空间 `mcp::v1`，target 前缀 `tugmcp::`。

**后果**：
- 消费者需 C++17 编译器（MSVC 2019 16.8+/GCC 9+/Clang 10+）。
- 不使用 C++20 特性。
- 不再有 GUI 工具链相关的构建约束。

## D12 — Conformance

**背景**：2026-07-28 较新，0.1.16 早于该版本，不能作为 2026 gate。

**决策**：使用 `@modelcontextprotocol/conformance` 0.2.0-alpha.12（先验证 2026 覆盖）+ 自定义 2026 vectors；Inspector 做冒烟。

**后果**：
- 需记录协议矩阵与版本；不能只跑 0.1.16。
- MRTR/subscriptions/templates/header mismatch/duplicate keys 需自定义用例。

## D13 — 纯 C++ 工具链与构建系统

**背景**：库需要被多项目复用，构建约束应只来自标准 C++ 工具链，而非某个 GUI 框架自带的编译器/构建系统。

**决策**：
- CMake ≥3.20 为唯一构建系统；支持 MSVC 2019 16.8+、GCC 9+、Clang 10+。
- Windows 链接 `ws2_32`（启用 TLS 时追加 `crypt32`）；线程统一用 `Threads::Threads`。
- 删除 GUI 工具链相关构建约束与测试框架依赖。

**后果**：
- CI 矩阵变为 3 编译器 × TLS on/off × header-only/编译模式。
- 消费者只需 C++17 编译器与 CMake ≥3.20，不引入 GUI 工具链依赖。

## D14 — TLS 可选编译

**背景**：TLS 依赖 OpenSSL，会增加链接/部署负担，且同一进程两套 OpenSSL 无法安全共存；不能默认强加给所有宿主。

**决策**：
- 项目级开关 `TUGMCP_TLS`（默认 OFF）+ `TUGMCP_TLS_BACKEND=openssl`。v1 仅支持 `openssl`；其它值在 configure 阶段直接失败。
- `TlsOptions` 字段无条件存在于公共 options；`transport/http.hpp` 永不 include cpp-httplib；header-only 实现仅在 `detail/http_httplib.hpp`。
- 宏作用域：编译模式下 `CPPHTTPLIB_OPENSSL_SUPPORT`/`TUGMCP_TLS_ENABLED` 为 **PRIVATE**（只作用于 `src/transport/*.cpp`），OpenSSL 为 PRIVATE（静态库经 `$<LINK_ONLY:>` 传给最终链接）；**header-only 模式才 INTERFACE**。禁止全局 `add_definitions`。
- `detail/http_httplib.hpp` 用 `#error` 守卫抓宏漂移（`#ifndef TUGMCP_TLS_ENABLED` 即失败，并校验 `TUGMCP_TLS_ENABLED` 与 `CPPHTTPLIB_SSL_ENABLED` 一致）。编译模式下 `src/transport/*.cpp` 直接 include `httplib.h`（不使用 detail 头），detail 头为 header-only 专用。header-only 下无法在链接期检测跨 TU 不一致，因此只支持经 `tugmcp::http` target 消费。
- 无 TLS 时客户端使用 `https://`：创建期失败，返回 `McpError`（`-32603`，`data.reason="unsupported_scheme"`），不进入 JSON-RPC 响应，也不发网络包。
- `certChainPath`/`privateKeyPath` 在 client 侧用于 mTLS（字段名相同，语义随角色）；`serverName`/`verifyPeer`/`caBundlePath` 为 client-only。
- OpenSSL 版本 ≥3.0；同一进程只允许一套 OpenSSL。

「条件 → reason」映射：

| 条件 | reason | 观测点 |
|---|---|---|
| `TlsOptions::enabled=true` 且构建 `TUGMCP_TLS=OFF` | `tls_not_built` | 传输创建返回 `Result` |
| server `enabled=true` 缺 cert/key | `tls_not_configured` | 传输创建返回 `Result` |
| cert/CA/SNI 路径非法或字段互斥 | `option_invalid` | 传输创建返回 `Result` |
| client `baseUrl` 为 `https://` 且无 TLS | `unsupported_scheme` | 传输创建返回 `Result`，不发网络包 |
| TLS 握手/证书校验失败 | `tls_handshake_failed` | **client**：首个请求的 `Result`；**server**：连接级，经 `IMessageSink::onError`，不产生 JSON-RPC 响应 |

**后果**：
- 必须遵守 ODR/ABI 规则：同一二进制内所有 TU 的 `CPPHTTPLIB_OPENSSL_SUPPORT` 定义一致。
- 回归矩阵需覆盖 TLS on/off，并验证无 TLS 构建下 `https://` 的创建期失败路径。

## D15 — 3rd/ 依赖引入

**背景**：构建需可复现、离线可用、不受系统包版本漂移影响；上游 httplib 的 CMake 默认自动探测 OpenSSL 并生成 install 规则，不适合直接嵌入。

**决策**：
- `3rd/` 内容 in-tree 提交（非 submodule），放源码 + 手写 INTERFACE target（`SYSTEM INTERFACE` include + `cxx_std_17`）：`tugmcp_3rd_json`、`tugmcp_3rd_httplib`、`tugmcp_3rd_jsoncons`、`tugmcp_3rd_valijson`、`tugmcp_3rd_doctest`。
- 不 `add_subdirectory` 上游 httplib。
- `TUGMCP_USE_SYSTEM_DEPS` 默认 OFF；system-deps 为**全局互斥**：ON 时完全不引入 `3rd/`，逐依赖 `find_package(... REQUIRED ...)`（版本 `EXACT`），失败即 FATAL，**不回退 vendored**；逐依赖映射见主设计 §20.2。
- `TUGMCP_BUILD_TESTS=ON` 时 doctest 必须可用（vendored 或 system），否则 FATAL。
- 不使用 FetchContent。
- `TUGMCP_VERIFY_3RD_HASHES` + `3rd/README.md` 清单（名称/版本/commit/SHA256/许可/URL）；system 模式忽略哈希校验。
- 根 `LICENSES/` 按 REUSE 风格聚合。
- v1 只保证静态库（不承诺 `BUILD_SHARED_LIBS` 可切换；`TUGMCP_API` 为未来共享库预留）。
- v1 只支持源码内嵌消费（宿主 `add_subdirectory(tugmcp)` 或包管理器），不提供 `install(EXPORT)`。

**后果**：
- 升级依赖需同时改 `3rd/` 与清单。
- 构建可复现、离线可用。

## D16 — M1 单线程执行（临时）

**背景**：M1 的目标是最小可用的 core + stdio。设计 §16 要求 handler 在 `IExecutor` 上执行、不得阻塞 transport 线程；但线程池与跨线程取消路由属于后续里程碑。

**决策**：M1 的 `McpServer` 在 transport reader 线程内同步调用 tool handler；`RequestContext.cancellation` 指向调用栈上的 `CancellationToken`（**恒非空**，但当前没有跨线程置位来源）。

**后果**：
- 慢工具会阻塞**该连接**的读侧（同一连接上的 ping/取消也会排队）；不同连接各有 reader，互不影响。
- 客户端超时只让本次调用返回 `timeout`，不会中断服务端 handler。
- 替换点明确：M5 引入 `IExecutor` 后，dispatcher 改为投递任务，`CancellationToken` 由 executor/取消路由置位。
- 这是临时状态：M1 之后不得在其它路径复制「IO 线程直接执行 handler」。

## D17 — `IExecutor` 契约冻结与默认执行器

**背景**：P2a 已提前引入 `IExecutor`（早于原 M5）。Gate 2a 评审要求在下游（SSE、订阅、2025 session）依赖它之前冻结接口形状；同时 `ServerOptions::executor` 默认仍为 `nullptr`，与主设计 §14「默认内部线程池」不一致。

**决策**：
- 接口（`include/tugmcp/executor.hpp`）冻结为：
  - `post(std::function<void()>, Priority) noexcept`：线程安全、非阻塞、不抛异常；同优先级 FIFO；`Emergency` 绕过容量上限（准入/安全路径），但 `drain`/`stop` 之后仍返回 `RejectedStopped`；容量只统计排队（不含运行中）。
  - `drain() noexcept`：**终止性**——此后拒绝新任务，等待排队与运行任务完成；从自身 worker 任务调用时排除调用任务（不自锁）。
  - `stop() noexcept`：**终止性**——拒绝新任务、丢弃排队任务、等待运行任务（自我排除）后 join；幂等。
  - `workerCount()`、`stats()`（queued/active/capacity）、`waitIdle(timeout)`；后两者为非纯虚函数，保持未来扩展的源码兼容。
  - `post` 捕获分配失败并返回 `RejectedQueueFull`；worker 捕获任务异常，异常不穿线程边界。
  - 在自身任务中销毁执行器（释放最后一个 owner）**不受支持**；在自身任务中调用 `stop()` 受支持。
- `ServerOptions::executor` 在 **P2b 期间保持默认 `nullptr`**（同步 dispatch，保持既有测试确定性），提供 `makeThreadPoolExecutor` 供宿主/示例显式注入。
- **P3 已切换默认值**为内部线程池（4 workers / 1024 队列）；`ServerOptions::useInlineExecutor = true` 可显式退回 transport 线程同步执行（测试/单线程宿主）。切换前置条件（取消路由、route 级 pending、拒绝响应）已满足。

**后果**：
- 下游（SSE/订阅/MRTR）可按上述语义设计；不得新增会抛异常的 `IExecutor` 实现。
- `McpServer::run()` 会 `drain()` 注入的执行器（终止性），宿主共享执行器时需自行管理生命周期。
- P2b/P3 必须补：注入执行器的集成测试、拒绝响应、取消路由、pending 命名空间。


## 变更流程

1. 提 ADR 变更：写清背景、替代方案、影响（API/wire/ABI/测试）。
2. 更新 `mcp-library-design.md` 对应章节。
3. 更新代码/测试/conformance。
4. 更新本表状态；不要只改代码。
