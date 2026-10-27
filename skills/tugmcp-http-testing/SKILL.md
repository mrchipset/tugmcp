---
name: tugmcp-http-testing
description: tugmcp Streamable HTTP 测试指南。Use when adding or running HTTP loopback, raw httplib, SSE, session, security, TLS, legacy, amalgamation or MCP conformance tests in tugmcp.
---

# tugmcp Streamable HTTP 测试

## 0. 测试分层（以当前 CMake 为准）

| Label | Target | 内容 |
|---|---|---|
| `unit` | `tugmcp_unit_tests`、`tugmcp_odr_tests` | codec、error、registry、dispatcher、canonical、schema、ODR |
| `integration` | `tugmcp_executor_tests`、`tugmcp_integration_tests` | stdio loopback、HTTP loopback/SSE/session、client 2025/2026、security、TLS（开启时）、legacy（开启时） |
| `amalgamation` | `amalgamation.core`、`amalgamation.release`、`amalgamation.http` | 单头生成、哈希、消费端 ODR、外部 Inspector/conformance |
| `conformance` | `conformance.external`、`conformance.external.2025` | 官方 `@modelcontextprotocol/conformance` |

- HTTP 测试全部编入 `tugmcp_integration_tests`；当前**没有**单独的 `tls` /
  `slow` / `win-only` CTest label（`designs/testing.md` 里的部分 target 是
  规划，实际见 `tests/CMakeLists.txt`）。
- TLS 用例以 `TLS` 开头；legacy 用例以 `legacy SSE` 开头；其余以
  `integration.` 为 CTest 前缀。
- 默认 Debug 基线：267 个测试。用 `ctest -N` 核对当前数量，不要硬编码。

## 1. HTTP 测试地图

| 文件 | 覆盖 |
|---|---|
| `tests/integration/http_loopback_test.cpp` | `McpClient`+`McpServer` round-trip；raw HTTP：2026 GET/DELETE 405、malformed JSON 400、unsupported version `-32022`、`Mcp-Method` mismatch `-32020`、413、并发 route、close/shutdown、client 版本控制 header、admission、TLS-not-built |
| `tests/integration/http_sse_test.cpp` | SSE progress/final、JSON 请求丢 progress、admission 429、`maxConcurrentStreams`、同 id 并发隔离、`stream_overflow`、`_meta` 校验、removed methods 404、Host/Origin、2025 unknown method 200 |
| `tests/integration/http_session_test.cpp` | 2025 initialize/session/POST/GET replay/DELETE、`requestClient`、2026 GET/DELETE 405、`maxSessions`、expired session、logging、resource subscriptions |
| `tests/integration/client_2026_loopback_test.cpp` | 2026 client：discover、listTools/callTool、progress、MRTR、resources/prompts、timeout、cancel、header shape |
| `tests/integration/client_2025_loopback_test.cpp` | 2025 client：initialize session、server→client requests、notifications、replay、cancel、timeout |
| `tests/integration/subscriptions_test.cpp` | 2026 `subscriptions/listen`、ack、filter、notification `_meta.subscriptionId` |
| `tests/integration/security_test.cpp` | 401/403、authorizer、approval deny/timeout、audit、2025 auth、GET/DELETE auth |
| `tests/integration/tls_loopback_test.cpp` | TLS round-trip、untrusted CA `tls_handshake_failed`、2025 TLS smoke（`TUGMCP_TLS=ON`） |
| `tests/integration/legacy_sse_test.cpp` | `/sse`+`/messages`、batch、session 错误、body limit、auth、Host/Origin、client loopback（`TUGMCP_ENABLE_LEGACY_SSE=ON`） |

常用精确用例名（`ctest -N` 可列出全部）：

- `integration.HTTP client and server round-trip discover, listTools and callTool`
- `integration.2026 GET and DELETE on /mcp return 405`
- `integration.Mcp-Method mismatch returns -32020`
- `integration.2026 missing _meta returns 400 InvalidParams with the id`
- `integration.2026 removed methods return 404 MethodNotFound with the id`
- `integration.2025 initialize creates a session and authenticated POST works`
- `integration.GET stream delivers notifications and replays by Last-Event-ID`
- `integration.SSE tool call streams progress and a final response`
- `integration.perStreamQueue overflow ends with a stream_overflow error frame`
- `integration.Host and Origin rebinding attempts return 403`
- `integration.TLS client and server round-trip discover, listTools and callTool`

## 2. 命令

### 2.1 配置与构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target tugmcp_integration_tests tugmcp_echo_server
```

Ninja/Makefiles：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target tugmcp_integration_tests tugmcp_echo_server
```

### 2.2 快线 / 全量

```powershell
# PR 快线（默认 HTTP）
ctest --test-dir build -C Debug -L "unit|integration" --output-on-failure --parallel 4

# 只看 HTTP/集成
ctest --test-dir build -C Debug -L integration --output-on-failure
ctest --test-dir build -C Debug -R "integration\." --output-on-failure

# 列出测试
ctest --test-dir build -C Debug -N
```

### 2.3 单测

```powershell
ctest --test-dir build -C Debug -R "integration.Mcp-Method mismatch" --output-on-failure
ctest --test-dir build -C Debug -R "integration.2025 initialize" --output-on-failure
```

直接跑 doctest（更快定位）：

```powershell
build\tests\Debug\tugmcp_integration_tests.exe --list-test-cases
build\tests\Debug\tugmcp_integration_tests.exe --test-case="*_meta*"
build\tests\Debug\tugmcp_integration_tests.exe --test-case="*session*"
```

### 2.4 TLS

```powershell
cmake -S . -B build-tls -G "Visual Studio 17 2022" -A x64 `
  -DTUGMCP_TLS=ON -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL"
cmake --build build-tls --config Debug --target tugmcp_integration_tests
ctest --test-dir build-tls -C Debug -R "TLS" --output-on-failure
```

### 2.5 legacy

```powershell
cmake -S . -B build-legacy -G "Visual Studio 17 2022" -A x64 `
  -DTUGMCP_ENABLE_LEGACY_SSE=ON
cmake --build build-legacy --config Debug --target tugmcp_integration_tests
ctest --test-dir build-legacy -C Debug -R "legacy SSE" --output-on-failure
```

### 2.6 conformance

```powershell
cmake -S . -B build-conformance -G "Visual Studio 17 2022" -A x64 `
  -DTUGMCP_CONFORMANCE=ON -DTUGMCP_WITH_HTTP=ON -DTUGMCP_BUILD_TESTS=ON
cmake --build build-conformance --config Debug
ctest --test-dir build-conformance -C Debug -L conformance --output-on-failure
```

### 2.7 单头

```powershell
cmake --build build --target amalgamate
cmake --build build --target amalgamate-release
ctest --test-dir build -C Debug -L amalgamation --output-on-failure
```

## 3. 新增 HTTP 测试

### 3.1 注册位置

- 新 `.cpp` 加到 `tests/CMakeLists.txt` 的 `if(TUGMCP_WITH_HTTP)` 下的
  `target_sources(tugmcp_integration_tests PRIVATE ...)`。
- TLS 用例放 `tls_loopback_test.cpp` 或新增到 TLS 分支；legacy 放
  `legacy_sse_test.cpp`。
- 测试已链接 `tugmcp::http`、`tugmcp::core`、`tugmcp_3rd_httplib`；不要重复
  加 include 路径。

### 3.2 约定

```cpp
#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"  // raw HTTP 测试需要

#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

TEST_CASE("my HTTP behavior") {
  // 1. bind 127.0.0.1, port = 0
  // 2. makeStreamableHttpServerTransport + REQUIRE(created.hasValue())
  // 3. dynamic_pointer_cast<HttpServerTransport> 取 boundPort()
  // 4. 构造 McpServer / 注册 tool
  // 5. server.run() 放到线程；用 ready-signal wrapper 等待回调安装完成
  // 6. 断言 status/header/body/error.data.reason
  // 7. server.stop(); thread.join()
}
```

- **端口**：`bind="127.0.0.1"`、`port=0`，用 `boundPort()`；不要固定端口。
- **同步**：不要 `sleep_for`。复用 `tests/integration/http_session_test.cpp`
  的 `ReadyTransport` / `RunningServer`，或
  `http_loopback_test.cpp` 的 `SignalTransport`。
- **超时**：unit/integration 默认 2s；TLS/executor 10s；慢测试不要塞进
  快线。
- **断言失败路径**：不仅断言状态码，还断言 JSON-RPC `error.code`、
  `error.data.reason`、`error.data.status`（如适用）。
- **raw HTTP**：用 `httplib::Client` 直接发请求，验证 header/status/body；
  这是区分 transport 与 client 的最快方式。

```cpp
httplib::Client cli("127.0.0.1", port);
httplib::Headers headers = {
    {"Content-Type", "application/json"},
    {"Accept", "application/json, text/event-stream"},
    {"MCP-Protocol-Version", "2026-07-28"},
    {"Mcp-Method", "tools/list"},
};
const std::string body =
    R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})";
auto response = cli.Post("/mcp", headers, body, "application/json");
REQUIRE(response);
CHECK(response->status == 200);
```

### 3.3 覆盖矩阵

| 改动 | 至少补 |
|---|---|
| header / `_meta` / 状态码 | raw HTTP 用例（`http_loopback_test.cpp` / `http_sse_test.cpp`） |
| 2026 removed methods | 404 + `-32601` + id 回显 |
| 2025 session/replay | `http_session_test.cpp`：initialize、缺 session、DELETE、过期、Last-Event-ID |
| client 2026 | `client_2026_loopback_test.cpp`：discover、header shape、timeout/cancel |
| client 2025 | `client_2025_loopback_test.cpp`：initialize、server→client、replay |
| 订阅 | `subscriptions_test.cpp`：ack、filter、`_meta.subscriptionId` |
| 安全 | `security_test.cpp`：401/403、approval、audit、Host/Origin |
| SSE/背压 | progress、`maxConcurrentStreams`、`perStreamQueue`、`stream_overflow` |
| TLS | `tls_loopback_test.cpp`：成功、CA 失败、`reason` |
| legacy | `legacy_sse_test.cpp`：endpoint、batch、auth、Host/Origin |
| shutdown | close 只触发一次 `onClose`；`run()` 返回；pending 清理 |

## 4. conformance 规则

- 固定 `@modelcontextprotocol/conformance@0.2.0-alpha.12`；Inspector 固定
  `@modelcontextprotocol/inspector@2.12.0`（Node ≥ 22.19）。
- configure 需要 Python3 与 `npx`；缺失时 `TUGMCP_CONFORMANCE=ON` 直接
  `FATAL_ERROR`，不是静默跳过。
- conformance 的 `server` 模式只支持 `--url`（HTTP/SSE），**不支持 stdio**；
  stdio 用 Inspector CLI。
- 仓库文档要求 `TUGMCP_BUILD_EXAMPLES=ON`；当前测试实际使用
  `tests/conformance/fixture_server.cpp` 构建的 `tugmcp_conformance_server`，
  不是生产 echo example。
- 2026 required 121、2025 required 73；要求 0 failure / 0 unexpected skip。
- `tests/conformance/expected_failures.yaml` 只登记**已知且已解释**的未计分项。
  已登记项重新通过 → runner 失败；未登记失败 → 失败。不要用它隐藏回归。
- fixture server（`tests/conformance/fixture_server.cpp`）只用于测试，不是
  生产 example；不要用 `tugmcp_echo_server` 代替。
- runner 输出只写 `build/conformance/<revision>/`；不要污染源码树。

## 5. 单头规则

- `amalgamation.core`：生成 + 双 TU ODR + stdio。
- `amalgamation.release`：三个产物 + `SHA256SUMS` + 许可证 + 可复现性。
- `amalgamation.http`：HTTP 双 TU ODR、Inspector、官方 conformance。
- 有意改公共头/HTTP 实现后：
  1. `cmake --build build --target amalgamate`
  2. 用 `tools/amalgamate.py --profile http --check ...` 或测试输出里的新
     hash 更新 `tests/amalgamation/http.sha256`（TLS 改 `http_tls.sha256`）
  3. 重跑 `ctest -L amalgamation`
- `TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION` 默认 ON：缺 Python/npx/Node/OpenSSL
  时测试仍注册并 **fail loud**。只有明确要跳过时才设为 OFF，并记录原因。

## 6. CI 口径

- PR 快线：`ctest -L "unit|integration" -LE "slow|tls|win-only"`，目标 < 60s。
  当前实际没有 `tls`/`slow`/`win-only` label，TLS 用例属于 `integration`。
- 夜线：全量 + TLS + legacy + conformance + amalgamation + sanitizers。
- `amalgamation.*` 是 `RUN_SERIAL`，不要用并行去压它。
- 失败必须能在 job/测试名/用例名定位；不要只报“整体失败”。

## 7. 反模式

- `sleep_for` 同步、固定端口、访问外部网络/DNS。
- 只测 happy path，不测 400/401/403/404/405/413/415/429/504。
- 用 `expected_failures.yaml` 或跳过测试来“变绿”。
- 失败重试而不是修根因；flaky 测试直接删。
- 在测试里复用全局可变状态；每个测试独立 scope、独立 server。
- 把 conformance fixture 当生产代码。

## 8. 验收清单

- [ ] 新行为有对应 HTTP 测试（成功 + 失败）。
- [ ] 状态码、`error.code`、`error.data.reason` 都被断言。
- [ ] 2026 与 2025 分支分别覆盖（如改动跨代）。
- [ ] `ctest -L integration` 通过；TLS/legacy 构建分别通过（如适用）。
- [ ] `ctest -L amalgamation` 通过；hash 已更新（如公共头变化）。
- [ ] conformance 未新增未登记失败；`expected_failures.yaml` 未被滥用。
- [ ] 测试无 `sleep_for`、固定端口、外部网络依赖。
