# Header-only 可行性与工量

> 问题：`tugmcp` 是否也可以做成 header-only？是否要增加很多工作量？
>
> 结论：**可以**。默认构建产物为静态库；**单头发布产物已支持**（`tugmcp.hpp` / `tugmcp_http.hpp` / `tugmcp_http_tls.hpp`），而不是一开始就做一个巨大的单头文件。工作量增加有限（设计阶段约 +15–25%），主要成本在编译时间、ODR 纪律和 HTTP 依赖的传递，不在协议逻辑本身。
>
> **当前状态（2026-10-25）**：单头 release artifacts 已支持并有生成/打包/哈希/外部验证；**split-header `TUGMCP_HEADER_ONLY=ON` 的 CMake INTERFACE 模式未实现，根 `CMakeLists.txt` 直接 `FATAL_ERROR`**。不要把两者混为一谈：消费单头不需要、也不能开启 `TUGMCP_HEADER_ONLY`。详见 [`docs/amalgamation.md`](../docs/amalgamation.md)。

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

**默认构建产物为静态库。发布的 header-only 形态是生成的单头 artifact；split-header 的 `TUGMCP_HEADER_ONLY` CMake 模式尚未实现（FATAL）。**

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
- 单头 `tugmcp.hpp`（以及 `tugmcp_http.hpp`/`tugmcp_http_tls.hpp`）由脚本把需要的头按顺序合并生成，并做双 TU/ODR 测试；不要手工维护两份实现。
- `TUGMCP_HEADER_ONLY` 是 **CMake option，但 split-header 模式未实现**：当前 ON 即 `FATAL_ERROR`。单头 artifact 不读取此开关。
- `TUGMCP_IMPLEMENTATION` 是 **宏**：单头场景的单 TU 实现宏，不是编译模式开关。

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
| 测试矩阵 | MSVC/GCC/Clang、单 TU/多 TU、单头 artifact/编译模式、TLS on/off | 中 |

单头 artifact 下 `CPPHTTPLIB_OPENSSL_SUPPORT` 无法在链接期检测跨 TU 不一致，因此消费方必须对所有看到 `httplib.h` 的 TU 使用同一产物/宏；编译模式下该宏为 PRIVATE，由 `detail/http_httplib.hpp` 的 `#error` 守卫兜底。

`tugmcp::tests` 是 INTERFACE helper 库（fake transport / conformance helpers / fixture 头，传播 `tugmcp_3rd_doctest`）；测试可执行文件（`tugmcp_unit_tests` 等）是独立 target，**始终编译**。split-header 的 `TUGMCP_HEADER_ONLY` 模式尚未实现，因此目前不存在“只影响库 target”的 INTERFACE 切换。

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

1. **core/protocol/stdio**：单头发布友好，优先支持。
2. **HTTP/SSE**：独立 `transport/http.hpp`（仅声明，不含 cpp-httplib）；单头 artifact 通过 `detail/http_httplib.hpp` 合并实现，也可编译成 `tugmcp::http`；不要让 `core.hpp` 传递 cpp-httplib。TLS 是 `tugmcp::http` 的构建开关（`TUGMCP_TLS`），不新增 target。编译模式下 `CPPHTTPLIB_OPENSSL_SUPPORT`/`TUGMCP_TLS_ENABLED` 为 PRIVATE（OpenSSL 亦 PRIVATE，静态库经 `$<LINK_ONLY:>` 传递）；单头模式下这些定义由 TLS artifact 自身在实现段发出，消费者必须在所有 TU 使用同一 artifact/宏。编译模式 `src/transport/*.cpp` 直接 include `httplib.h`，`detail/http_httplib.hpp` 供单头生成使用；守卫含 `#ifndef TUGMCP_TLS_ENABLED` 检查。
3. **schema**：独立可选头（jsoncons/Valijson/null），按需包含。
4. **单头**：发布时生成 `tugmcp.hpp`/`tugmcp_http.hpp`/`tugmcp_http_tls.hpp`，附带 `SHA256SUMS`、`NOTICE`/`README` 与生成/打包脚本；开发时维护 split headers。split-header `TUGMCP_HEADER_ONLY` 模式仍延后。
5. **CI**：双 TU ODR 测试 + MSVC x64 / GCC / Clang + 编译模式 + TLS on/off 各一遍；单头 artifact 另有 Inspector/conformance 外部验证。

## 7. 验收标准

完整测试分层见 [`testing.md`](./testing.md)；本节只列 header-only 特有验收项。

- [x] 两个 TU 同时 include 单头，链接无重复符号（core/HTTP/TLS 均有测试）。
- [x] `core.hpp` 不 include cpp-httplib/GUI 框架头；`http.hpp` 不进入 core 编译单元。
- [x] MSVC x64 构建 echo server/client（TLS on/off 各一遍）。
- [ ] split-header `TUGMCP_HEADER_ONLY` 与编译模式行为一致（模式未实现，延后）。
- [x] 记录编译时间/二进制大小基线；单头生成可复现（`SHA256SUMS` + `amalgamation.release`）。
- [x] Windows 链接 `ws2_32`/`crypt32` 与 `TUGMCP_TLS` 开关的说明在 README/CMake target/发布 README 中可见。
- [x] 同一进程内所有 TU 的 `CPPHTTPLIB_OPENSSL_SUPPORT` 定义一致（TLS on/off 不混用）。
- [x] `transport/http.hpp` 不 include 任何第三方头；TLS/构建模式切换不改变其声明。
- [x] `TUGMCP_HEADER_ONLY=ON` + `TUGMCP_ENABLE_LEGACY_SSE=ON` 必须 configure 失败（`FATAL_ERROR`）；当前仅 `TUGMCP_HEADER_ONLY=ON` 本身即失败。

## 8. 与宿主应用的关系

- 宿主应用以静态库或单头 artifact 嵌入都可以；单头可避免 DLL ABI，但会增加宿主编译时间。split-header `TUGMCP_HEADER_ONLY` 模式尚未实现。
- 如果宿主只用一个 TU 包含 HTTP 头，header-only 可行；如果多个 TU 都包含，建议用编译版 `tugmcp::http` 或只在一个 TU 定义实现。
- 无论哪种形态，宿主的执行器/UI 调度/安全逻辑仍留在宿主。

## 9. 单头 artifact（发布用，与 `TUGMCP_HEADER_ONLY` 分离）

> **状态**：单头 release artifacts **已支持**；split-header `TUGMCP_HEADER_ONLY` INTERFACE 模式**未实现（FATAL）**。单头消费不需要该开关。面向消费者的完整说明见 [`docs/amalgamation.md`](../docs/amalgamation.md)。

- **源码权威**：`include/` split headers + `src/` 实现；单头只在发布/验证时由 `tools/amalgamate.py` 生成，不手写、不提交为第二份实现。
- **产物矩阵（精确名称/guard）**：
  - `tugmcp.hpp` — core + stdio，guard `TUGMCP_SINGLE_HEADER_CORE_HPP`，输出到 `build/amalgamation/tugmcp.hpp`；nlohmann/json 外部，链接 Threads。
  - `tugmcp_http.hpp` — core + stdio + Streamable HTTP/SSE（非 TLS），guard `TUGMCP_SINGLE_HEADER_HTTP_HPP`，输出到 `build/amalgamation/tugmcp_http.hpp`；cpp-httplib 外部，Windows `ws2_32`。
  - `tugmcp_http_tls.hpp` — 同上 + TLS，guard `TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP`，输出到 `build/amalgamation/tugmcp_http_tls.hpp`；OpenSSL ≥ 3.0，Windows `ws2_32` + `crypt32`，实现段在 `<httplib.h>` 前发出 `TUGMCP_TLS_ENABLED=1` 与 `CPPHTTPLIB_OPENSSL_SUPPORT`。
  - schema（jsoncons/Valijson）与 legacy HTTP+SSE **不进入单头**。
- **消费方式**：恰好一个 TU `#define TUGMCP_IMPLEMENTATION` 后 include；其余 TU 只 include。`TUGMCP_HEADER_ONLY` CMake 模式未实现，单头 artifact 不依赖它。
- **生成规则**：按公共头依赖排序；去掉项目内 include/`#pragma once`，保留第三方 include；实现段由 `src/**` 合并到 `#ifdef TUGMCP_IMPLEMENTATION`；处理匿名命名空间/文件级 static/宏重名；输出确定性（无时间戳）。
- **打包 target**：`amalgamate` 生成三个头；`amalgamate-release` 调用 `tools/package_amalgamation.py`，默认输出 `${CMAKE_BINARY_DIR}/amalgamation/release`（`TUGMCP_AMALGAMATION_RELEASE_DIR` 可覆盖）。发布目录含三个头、`LICENSE`（MIT）、`SHA256SUMS`、`NOTICE`、`README.md`、`LICENSES/{nlohmann-json-MIT.txt,cpp-httplib-MIT.txt}`；项目版本从 `CMakeLists.txt` 读取（当前 `0.1.0`），nlohmann/json `3.12.0`，cpp-httplib 版本从 `httplib.h` 读取并校验为 `0.63.0`。打包先写临时目录再整体替换；重复运行逐字节一致。
- **哈希/许可证流程**：记录哈希在 `tests/amalgamation/{core,http,http_tls}.sha256`；打包脚本默认校验生成结果与记录哈希一致。项目 `LICENSE` 与第三方许可证从源码树原样复制。`SHA256SUMS` 覆盖 `LICENSE`、三个头与两份第三方许可证文本。第三方头始终外置，不内嵌。
- **验收覆盖**：
  - `tests/amalgamation/run_tests.py`（`amalgamation.core`）：core 生成确定性、记录哈希、负向用例、双 TU/ODR、stdio 行为。
  - `tests/amalgamation/run_release.py`（`amalgamation.release`）：发布文件、`SHA256SUMS`、许可证/NOTICE/README、版本串、逐字节再生成；不需要 npx。
  - `tests/amalgamation/run_external.py`（`amalgamation.http`）：stdlib/HTTP/TLS 双 TU ODR、standalone echo/fixture server、MCP Inspector 2.12、官方 conformance（2026-07-28 121 checks / 2025-11-25 73）。`TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=ON` 时缺工具 fail loud。
- **发布**：CI/本地用 `amalgamate-release` 生成到 `build/amalgamation/release` 或 release 附件；不进入源码树。

