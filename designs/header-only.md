# Header-only 可行性与工量

> 问题：`tugmcp` 是否也可以做成 header-only？是否要增加很多工作量？
>
> 结论：**可以**。默认构建产物为静态库，同时支持以 header-only 方式消费（`TUGMCP_HEADER_ONLY=ON` 显式开启），而不是一开始就做一个巨大的单头文件。工作量增加有限（设计阶段约 +15–25%），主要成本在编译时间、ODR 纪律和 HTTP 依赖的传递，不在协议逻辑本身。

## 1. 为什么可行

当前设计的主要依赖都支持 header-only：

| 依赖 | 形态 | 影响 |
|---|---|---|
| nlohmann/json | 单头 | 已天然 header-only |
| cpp-httplib | 单头 | HTTP/SSE 可 header-only，但会传递 ws2_32/crypt32（TLS）/Threads 链接要求 |
| jsoncons | 单头（可选） | schema 2020-12 可 header-only |
| Valijson | 单头（可选） | draft-07 可 header-only |
| 标准库 | — | thread/mutex/condition_variable 需要链接平台线程库（CMake 用 `Threads::Threads`） |

协议/编解码/registry/dispatcher/transport 抽象本身是普通 C++，没有必须放到 `.cpp` 的天然边界。

## 2. 推荐形态

**默认构建产物为静态库；同时支持以 header-only 方式消费（`TUGMCP_HEADER_ONLY=ON` 显式开启）。**

```text
include/tugmcp/
  core.hpp                 // 类型/错误/JSON-RPC/上下文（轻）
  server.hpp
  client.hpp
  transport/stdio.hpp      // 轻，仅 OS pipe/线程
  transport/http.hpp       // 仅声明：TlsOptions / StreamableHttp*/工厂；不含 cpp-httplib
  detail/http_httplib.hpp  // header-only 实现，唯一 include cpp-httplib 的实现头
  protocol/v2026.hpp
  protocol/v2025.hpp
  schema/jsoncons.hpp      // 可选
  schema/valijson.hpp      // 可选
  schema/null.hpp          // 测试
  tugmcp.hpp               // 生成的 amalgamation（发布用，不手写）
```

- 公共头只允许 include nlohmann/json（`Json` 暴露）与对应可选 schema target 的第三方头；`core.hpp`/`server.hpp`/`client.hpp`/`transport/http.hpp` 永不 include cpp-httplib/GUI 框架头。
- `detail/http_httplib.hpp` 才引入 cpp-httplib；不使用 HTTP 的消费者不付这份编译成本。
- 单头 `tugmcp.hpp` 由脚本把需要的头按顺序合并生成，并做双 TU/ODR 测试；不要手工维护两份实现。
- `TUGMCP_HEADER_ONLY` 是 **CMake option**：ON 时相关 target 变为 INTERFACE（不编译 `src/`）。
- `TUGMCP_IMPLEMENTATION` 是 **宏**：header-only / amalgamation 场景的单 TU 实现宏。

## 3. 需要额外做的工作

| 工作 | 说明 | 工量 |
|---|---|---|
| ODR/inline 纪律 | 非模板自由函数 `inline`；常量 `inline constexpr`；禁止头文件里的非 inline 全局对象 | 小，但必须 CI 检查 |
| 双 TU 测试 | 两个 `.cpp` 同时 include `tugmcp.hpp`/`core.hpp`，链接无重复符号 | 小 |
| 编译时间 | cpp-httplib + nlohmann + 模板会放大每个 TU 的编译成本 | 中；用 feature headers/编译模式缓解 |
| 二进制大小 | inline 代码在多 TU 重复；LTO/`-ffunction-sections` 可缓解 | 中 |
| 链接要求 | Windows `ws2_32`（TLS 时 `crypt32`）；线程用 `Threads::Threads`；TLS 需 OpenSSL ≥3.0；CMake target 必须写清 | 小 |
| 可选依赖/宏 | `TUGMCP_WITH_HTTP`、`TUGMCP_WITH_JSONCONS`、`TUGMCP_WITH_VALIJSON`、`TUGMCP_TLS` 的一致性 | 小–中 |
| ABI/实现隐藏 | header-only 暴露实现；DLL/插件场景不适合 | 设计取舍 |
| 测试矩阵 | MSVC/GCC/Clang、单 TU/多 TU、header-only/编译模式、TLS on/off | 中 |

header-only 下 `CPPHTTPLIB_OPENSSL_SUPPORT` 无法在链接期检测跨 TU 不一致，因此只支持经 `tugmcp::http` target 消费；编译模式下该宏为 PRIVATE，由 `detail/http_httplib.hpp` 的 `#error` 守卫兜底。

`tugmcp::tests` 是 INTERFACE helper 库（fake transport / conformance helpers / fixture 头，传播 `tugmcp_3rd_doctest`）；测试可执行文件（`tugmcp_unit_tests` 等）是独立 target，**始终编译**；`TUGMCP_HEADER_ONLY` 只影响库 target。

## 4. 工量评估

- **从设计开始就按 header-only 设计**：相对编译库，约 **+15–25%** 工作量。主要是头文件拆分、ODR/CI、编译时间调优，不是重写协议。
- **已有编译库再改 header-only**：约 **+30–50%**，因为要把实现移入头、处理静态状态/单例、调整依赖和测试；还可能出现编译时间/代码膨胀问题。
- **完整单头化（包括 HTTP/SSE）**：可行，但消费者每次编译都付 cpp-httplib/nlohmann 的成本；大项目可能难以接受。建议单头只作为发布产物，不作为开发主形态。
- **不会**因为 header-only 就自动获得“零依赖”：ws2_32/crypt32/Threads/OpenSSL（若 TLS）仍然要链接。

## 5. 什么时候不要 header-only

- 需要稳定 ABI 的 DLL/插件边界；
- 消费者很多、编译时间敏感；
- 需要隐藏实现/减少公共头暴露；
- 需要可选依赖隔离（HTTP/TLS/schema）而不想用宏污染消费者；
- 需要 pimpl 控制二进制兼容。

这些情况下用静态库/编译库更合适；header-only 可作为附加分发形态。

## 6. 推荐决策

1. **core/protocol/stdio**：header-only 友好，优先支持。
2. **HTTP/SSE**：独立 `transport/http.hpp`（仅声明，不含 cpp-httplib），可 header-only（实现位于 `detail/http_httplib.hpp`），也可编译成 `tugmcp::http`；不要让 `core.hpp` 传递 cpp-httplib。TLS 是 `tugmcp::http` 的构建开关（`TUGMCP_TLS`），不新增 target。编译模式下 `CPPHTTPLIB_OPENSSL_SUPPORT`/`TUGMCP_TLS_ENABLED` 为 PRIVATE（OpenSSL 亦 PRIVATE，静态库经 `$<LINK_ONLY:>` 传递）；header-only 模式下这些定义才走 INTERFACE，且消费者必须在所有 TU 使用同一宏，只支持经 `tugmcp::http` target 消费。编译模式 `src/transport/*.cpp` 直接 include `httplib.h`，`detail/http_httplib.hpp` 为 header-only 专用；守卫含 `#ifndef TUGMCP_TLS_ENABLED` 检查。
3. **schema**：独立可选头（jsoncons/Valijson/null），按需包含。
4. **单头**：发布时生成 `tugmcp.hpp`，附带 SHA256 和生成脚本；开发时维护 split headers。
5. **CI**：双 TU ODR 测试 + MSVC x64 / GCC / Clang + header-only/编译模式 + TLS on/off 各一遍。

## 7. 验收标准

完整测试分层见 [`testing.md`](./testing.md)；本节只列 header-only 特有验收项。

- [ ] 两个 TU 同时 include 公共头，链接无重复符号。
- [ ] `core.hpp` 不 include cpp-httplib/GUI 框架头；`http.hpp` 不进入 core 编译单元。
- [ ] MSVC x64 与 GCC/Clang 各构建一个 echo server/client（TLS on/off 各一遍）。
- [ ] header-only 与编译模式行为一致（同一测试集）。
- [ ] 记录编译时间/二进制大小基线；单头生成可复现。
- [ ] Windows 链接 `ws2_32`/`crypt32` 与 `TUGMCP_TLS` 开关的说明在 README/CMake target 中可见。
- [ ] 同一进程内所有 TU 的 `CPPHTTPLIB_OPENSSL_SUPPORT` 定义一致（TLS on/off 不混用）。
- [ ] `transport/http.hpp` 不 include 任何第三方头；TLS/构建模式切换不改变其声明。
- [ ] `TUGMCP_HEADER_ONLY=ON` + `TUGMCP_ENABLE_LEGACY_SSE=ON` 必须 configure 失败（`FATAL_ERROR`）。

## 8. 与宿主应用的关系

- 宿主应用以静态库或 header-only 嵌入都可以；header-only 可避免 DLL ABI，但会增加宿主编译时间。
- 如果宿主只用一个 TU 包含 HTTP 头，header-only 可行；如果多个 TU 都包含，建议用编译版 `tugmcp::http` 或只在一个 TU 定义实现。
- 无论哪种形态，宿主的执行器/UI 调度/安全逻辑仍留在宿主。

## 9. 单头 artifact（发布用，与 `TUGMCP_HEADER_ONLY` 分离）

- **源码权威**：`include/` split headers + `src/` 实现；单头只在发布时由 `tools/amalgamate.py` 生成，不手写、不提交为第二份实现。
- **产物矩阵**：
  - `tugmcp.hpp`：core + stdio（nlohmann/json 作为外部依赖；Threads 链接）。
  - `tugmcp_http.hpp`：core + stdio + Streamable HTTP/SSE（cpp-httplib 外部依赖；TLS 变体按宏/单独产物；ws2_32/crypt32/OpenSSL）。
  - schema（jsoncons/Valijson）与 legacy **不进入单头**；`TUGMCP_HEADER_ONLY + LEGACY` 继续 FATAL。
- **消费方式**：恰好一个 TU `#define TUGMCP_IMPLEMENTATION` 后 include；其余 TU 只 include。`TUGMCP_HEADER_ONLY` CMake 模式仍不实现，单头 artifact 不依赖它。
- **生成规则**：按公共头依赖排序；去掉项目内 include/`#pragma once`，保留第三方 include；实现段由 `src/**` 合并到 `#ifdef TUGMCP_IMPLEMENTATION`；处理匿名命名空间重名；输出确定性（无时间戳）。
- **验收**：生成两次 SHA256 一致；双 TU ODR/链接；standalone consumer 行为测试；单头构建的 echo/fixture server 通过 MCP Inspector 与官方 conformance（2026/2025）；TLS 变体编译/链接 smoke。
- **发布**：CI 生成到 `build/amalgamation/` 或 release 附件，附 `SHA256SUMS` 与第三方许可证说明；不进入源码树。

