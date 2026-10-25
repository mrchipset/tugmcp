# 单头发布产物（Amalgamation）

> 状态快照：**2026-10-25**（M1–M6 完成；单头发布产物已支持）。
>
> 本文说明 `tugmcp` 的单头（single-header）发布产物、消费方式、链接要求与
> 发布流程。源码权威仍是 `include/` 的 split headers 与 `src/` 实现；单头只在
> 发布/验证时生成。

## 1. 结论与边界

- **已支持**：由 `tools/amalgamate.py` 生成的三个单头发布产物：

  | 产物 | Profile | Guard | 外部头 | 链接要求 |
  |---|---|---|---|---|
  | `tugmcp.hpp` | core + stdio | `TUGMCP_SINGLE_HEADER_CORE_HPP` | `<nlohmann/json.hpp>` | `Threads::Threads` |
  | `tugmcp_http.hpp` | core + stdio + Streamable HTTP/SSE（非 TLS） | `TUGMCP_SINGLE_HEADER_HTTP_HPP` | `<nlohmann/json.hpp>`、`<httplib.h>` | `Threads::Threads`；Windows `ws2_32` |
  | `tugmcp_http_tls.hpp` | core + stdio + Streamable HTTP/SSE（TLS） | `TUGMCP_SINGLE_HEADER_HTTP_TLS_HPP` | `<nlohmann/json.hpp>`、`<httplib.h>` | `Threads::Threads`；OpenSSL ≥ 3.0（`SSL`/`Crypto`）；Windows `ws2_32` + `crypt32` |

- **未实现**：split-header 的 `TUGMCP_HEADER_ONLY=ON` CMake INTERFACE 模式。
  根 `CMakeLists.txt` 对 `TUGMCP_HEADER_ONLY=ON` 直接 `FATAL_ERROR`。单头产物
  不依赖该开关；两者是不同形态。
- **不进入单头**：jsoncons/Valijson schema 后端与 legacy HTTP+SSE（2024-11-05）
  适配。schema/legacy 仍通过编译库 target 消费。
- 第三方头**不内嵌**：nlohmann/json 与 cpp-httplib 始终由消费者提供 include
  路径。单头只合并 `tugmcp` 自身代码。

## 2. 消费方式

单头遵循“恰好一个 TU 定义实现”的规则：

```cpp
// impl.cpp
#define TUGMCP_IMPLEMENTATION
#include "tugmcp_http.hpp"

// other.cpp
#include "tugmcp_http.hpp"   // 不定义 TUGMCP_IMPLEMENTATION
```

`TUGMCP_IMPLEMENTATION` 是**宏**，不是 CMake 开关；只能在一个 TU 中定义。
其余 TU 只 include。生成的实现段包在 `#ifdef TUGMCP_IMPLEMENTATION` 内。

最小 CMake 消费示例：

```cmake
add_executable(app impl.cpp other.cpp)
target_compile_features(app PRIVATE cxx_std_17)
target_include_directories(app PRIVATE
  ${TUGMCP_RELEASE_DIR}          # 含 tugmcp_http.hpp
  ${NLOHMANN_JSON_INCLUDE}       # 含 nlohmann/json.hpp
  ${CPP_HTTPLIB_INCLUDE})        # 仅 HTTP 产物需要，含 httplib.h
target_link_libraries(app PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(app PRIVATE ws2_32)
endif()
# TLS 产物额外链接 OpenSSL::SSL OpenSSL::Crypto；Windows 再追加 crypt32。
```

注意：

- 所有产物都要链接线程库（`Threads::Threads`）。
- `tugmcp.hpp` 不需要 cpp-httplib，也不需要 `ws2_32`。
- `tugmcp_http.hpp` 需要 cpp-httplib 外部头与 Windows `ws2_32`。
- `tugmcp_http_tls.hpp` 需要 OpenSSL ≥ 3.0、Windows `ws2_32` + `crypt32`。
- MSVC 合并大 TU 时建议 `/bigobj`；测试 consumer 已启用。

## 3. TLS 宏与 ODR 规则

TLS 产物在实现段 prelude 中、`<httplib.h>` 之前发出：

```cpp
#define TUGMCP_TLS_ENABLED 1
#define CPPHTTPLIB_OPENSSL_SUPPORT
```

规则：

- **每个看到 `httplib.h` 的 TU 必须使用一致的宏**。这包括直接 include
  cpp-httplib 的 TU，以及通过 `tugmcp_http_tls.hpp` 间接看到的 TU。
- 不要在同一个程序/进程中混用 TLS 与非 TLS 产物（`tugmcp_http_tls.hpp` vs
  `tugmcp_http.hpp`/`tugmcp.hpp`）。
- 宏不一致在链接期无法可靠检测，属于 ODR 违规；`CPPHTTPLIB_OPENSSL_SUPPORT`
  的差异会改变 cpp-httplib 的类布局与符号。
- 编译库模式下这些宏由 `tugmcp::http` target 以 PRIVATE 传播；单头模式下由
  产物自身定义，消费者必须整体采用同一产物。

## 4. 生成与发布

### 直接生成

```bash
python tools/amalgamate.py --profile core --out build/amalgamation/tugmcp.hpp
python tools/amalgamate.py --profile http --out build/amalgamation/tugmcp_http.hpp
python tools/amalgamate.py --profile http --tls --out build/amalgamation/tugmcp_http_tls.hpp
```

### CMake target

已配置的构建树里：

```bash
cmake --build build --target amalgamate          # 仅生成三个头
cmake --build build --target amalgamate-release  # 生成并打包发布目录
```

`amalgamate` 不进入 `ALL`，正常构建不受影响，也不需要网络。

### 发布目录

`amalgamate-release` 调用 `tools/package_amalgamation.py`，默认输出到
`${CMAKE_BINARY_DIR}/amalgamation/release`；可用 cache 变量覆盖：

```bash
cmake -S . -B build -DTUGMCP_AMALGAMATION_RELEASE_DIR=out/release
```

发布目录内容：

```text
release/
  tugmcp.hpp
  tugmcp_http.hpp
  tugmcp_http_tls.hpp
  SHA256SUMS
  NOTICE
  README.md
  LICENSES/
    nlohmann-json-MIT.txt
    cpp-httplib-MIT.txt
```

`SHA256SUMS` 记录三个头与两个许可证文件的 SHA256，格式为
`<sha256>  <相对路径>`，按路径排序。`NOTICE`/`README.md` 记录项目版本
`0.1.0`、nlohmann/json `3.12.0`、cpp-httplib `0.63.0` 以及 artifact matrix。
输出不含时间戳；同一输入重复运行必须逐字节一致。

## 5. 哈希与许可证政策

- 记录哈希位于 `tests/amalgamation/`：

  | profile | 文件 | 产物 |
  |---|---|---|
  | core | `core.sha256` | `tugmcp.hpp` |
  | http | `http.sha256` | `tugmcp_http.hpp` |
  | http --tls | `http_tls.sha256` | `tugmcp_http_tls.hpp` |

  打包脚本默认校验生成结果与记录哈希一致；故意变更产物时必须同步更新对应
  `.sha256`，否则 `package_amalgamation.py` 与测试都会失败。
- 许可证文本从仓库 `LICENSES/` 原样复制，不重写、不重新排版。
- 第三方依赖版本/commit：nlohmann/json `3.12.0`
  （commit `55f93686c01528224f448c19128836e7df245f72`）；cpp-httplib
  `0.63.0`（`3rd/README.md` 未登记其上游 commit，使用 vendored header 的
  SHA256 `9021001a719768893a8a6e4051c56107690e8b8e49b41e5f903534fa19ae9afb`
  作为可复现身份）。
- cpp-httplib 与 nlohmann/json 的许可证已经聚合在 `LICENSES/`；发布包只带
  与单头产物直接相关的这两份文本。schema 后端不在单头内，故不随包分发。

## 6. 验证命令

```bash
# 生成、记录哈希、双 TU/ODR、stdio 行为（无需 npx）
python tests/amalgamation/run_tests.py

# 打包目录、哈希、许可证、版本、确定性再生成（无需 npx）
python tests/amalgamation/run_release.py

# 外部验证：MCP Inspector + 官方 conformance（需要 Node/npx）
python tests/amalgamation/run_external.py
```

对应的 ctest（label `amalgamation`）：

```bash
ctest --test-dir build -L amalgamation --output-on-failure
# amalgamation.core / amalgamation.release / amalgamation.http
```

`amalgamation.http` 默认要求外部工具存在（`TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=ON`），
缺失时 fail loud，不会静默跳过；显式关闭该选项才会移除该测试。

外部验证覆盖：

- 两个 TU 同时 include 单头（一个 TU 定义实现）的链接/ODR；
- HTTP 声明-only TU 不包含 cpp-httplib 仍可链接；
- TLS 变体编译/链接/ODR；
- 单头 stdio / HTTP echo server 与 conformance fixture；
- MCP Inspector 2.12 的 `initialize`/`tools/list`/`tools/call`；
- 官方 conformance：2026-07-28（121 required checks）与 2025-11-25（73）。

## 7. 限制与后续

- **第三方头外置**：消费者必须自行提供 nlohmann/json 与 cpp-httplib（TLS 还需
  OpenSSL）。单头不是“零依赖”。
- **schema/legacy 排除**：需要 jsoncons/Valijson 或 legacy HTTP+SSE 时必须使用
  编译库 target，而不是单头。
- **split-header `TUGMCP_HEADER_ONLY` 未实现**：CMake 直接 FATAL；不要把它
  当成单头产物的开关。单头消费不需要该开关。
- **稳定 ABI/DLL 场景**：单头暴露实现，每个消费 TU 都编译实现代码；需要隐藏
  实现或稳定 ABI 时优先静态库。
- **`TUGMCP_USE_SYSTEM_DEPS`、install/export、CI 矩阵**仍属后续 backlog；详见
  `docs/roadmap.md`。
