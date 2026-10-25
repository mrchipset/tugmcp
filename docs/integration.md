# 集成指南（Integration）

> 状态快照：**2026-10-25**（M1–M6 完成；单头发布产物已支持）。
>
> 本文说明宿主应用如何消费 `tugmcp`、如何把 server/client 嵌入进程，以及
> 线程、安全、schema 与部署边界。构建命令见
> [`docs/building.md`](./building.md)，公共 API 见
> [`docs/api.md`](./api.md)，单头产物细节见
> [`docs/amalgamation.md`](./amalgamation.md)。

## 1. 三种消费方式

| 方式 | 适用场景 | 说明 |
|---|---|---|
| 源码子目录（`add_subdirectory`） | 同仓库/私有源码集成，推荐 | 直接使用 `tugmcp::core`、`tugmcp::http` 等 ALIAS target；include、C++17、线程与平台库由 target 传播 |
| FetchContent | 独立仓库、固定版本的源码集成 | 拉取源码后同样 `add_subdirectory`；需要宿主自己提供仓库地址与 tag/commit |
| 单头产物 | 不方便引入 CMake 子目录，或需要少量文件分发 | 由 `tools/amalgamate.py` 生成；恰好一个 TU 定义 `TUGMCP_IMPLEMENTATION` |

**当前不提供 install/export 规则**，因此没有 `find_package(tugmcp)`、
`tugmcpConfig.cmake` 或 `install()` 目标。不要按已安装包的方式链接。

`include/` 只通过 `$<BUILD_INTERFACE:...>` 暴露，正好支持
`add_subdirectory`/FetchContent 源码集成。

## 2. CMake 静态库集成

最小宿主工程：

```cmake
cmake_minimum_required(VERSION 3.20)
project(host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 在 add_subdirectory 之前设置，避免把库的测试/示例带进宿主构建。
set(TUGMCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUGMCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# add_subdirectory(<tugmcp 源码目录> <构建目录名>)
add_subdirectory(third_party/tugmcp tugmcp)

add_executable(host main.cpp)
target_link_libraries(host PRIVATE tugmcp::core)

# 需要 Streamable HTTP 时再链接：
# target_link_libraries(host PRIVATE tugmcp::http)
```

传播关系：

- `tugmcp::core` PUBLIC 传播 `include/`、`cxx_std_17`、`Threads::Threads` 与
  `tugmcp_3rd_json`；宿主不需要自己 `find_package(Threads)` 或添加
  nlohmann/json include 路径。
- `tugmcp::http` PUBLIC 依赖 `tugmcp::core`；cpp-httplib、`ws2_32`、
  TLS 的 OpenSSL/`crypt32` 均为 PRIVATE（静态库通过 `$<LINK_ONLY:...>` 传给
  最终链接）。宿主照常链接 `tugmcp::http` 即可。
- schema target 是独立库，按需链接：
  `tugmcp::schema_jsoncons`、`tugmcp::schema_valijson`。

FetchContent 形式（仓库地址/标签替换为实际值）：

```cmake
include(FetchContent)
set(TUGMCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUGMCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tugmcp
  GIT_REPOSITORY <actual-tugmcp-remote>
  GIT_TAG <pinned-tag-or-commit>)
FetchContent_MakeAvailable(tugmcp)

add_executable(host main.cpp)
target_link_libraries(host PRIVATE tugmcp::core)
```

不要用 FetchContent 拉取 nlohmann/json、cpp-httplib 或 OpenSSL；依赖策略见
[`docs/building.md`](./building.md) §2。

## 3. 单头消费

三个产物：

| 产物 | 能力 | 外部头 | 链接 |
|---|---|---|---|
| `tugmcp.hpp` | core + stdio | `<nlohmann/json.hpp>` | `Threads::Threads` |
| `tugmcp_http.hpp` | core + stdio + Streamable HTTP/SSE（非 TLS） | nlohmann/json、`<httplib.h>` | `Threads::Threads`；Windows `ws2_32` |
| `tugmcp_http_tls.hpp` | 同上 + TLS | nlohmann/json、`<httplib.h>` | `Threads::Threads`；OpenSSL ≥ 3.0；Windows `ws2_32` + `crypt32` |

规则：

- **恰好一个 TU** 在 include 前 `#define TUGMCP_IMPLEMENTATION`；其余 TU 只
  include。`TUGMCP_IMPLEMENTATION` 是宏，不是 CMake 开关。
- `TUGMCP_HEADER_ONLY` 是未实现的 split-header CMake 模式，**不要**打开；
  单头消费不需要它。
- 第三方头不内嵌，消费者提供 include 路径。
- TLS 宏必须对所有看到 `httplib.h` 的 TU 一致；同一程序不要混用 TLS 与非
  TLS 产物。

```cpp
// impl.cpp
#define TUGMCP_IMPLEMENTATION
#include "tugmcp_http.hpp"

// other.cpp
#include "tugmcp_http.hpp"
```

```cmake
add_executable(app impl.cpp other.cpp)
target_compile_features(app PRIVATE cxx_std_17)
target_include_directories(app PRIVATE
  "${TUGMCP_RELEASE_DIR}"       # 含 tugmcp_http.hpp
  "${NLOHMANN_JSON_INCLUDE}"    # 含 nlohmann/json.hpp
  "${CPP_HTTPLIB_INCLUDE}")     # 仅 HTTP/TLS 产物
target_link_libraries(app PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(app PRIVATE ws2_32)
endif()
# TLS 产物再链接 OpenSSL::SSL OpenSSL::Crypto；Windows 追加 crypt32。
```

单头**不包含** schema（jsoncons/Valijson）与 legacy HTTP+SSE；需要它们时用
编译库。完整发布/校验流程见 [`docs/amalgamation.md`](./amalgamation.md)。

## 4. 把 stdio server 嵌入宿主

`StdioTransport` 是换行分隔 JSON 的 C 流封装，**不拥有也不关闭**传入的
`FILE*`（默认 `stdin`/`stdout`）。宿主把 `McpServer` 与
`makeStdioTransport()` 组合，`run()` 启动传输并阻塞到 `stop()` 或传输关闭。

```cpp
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/stdio.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace mcp::v1;

class EchoTool final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&,
                             IProgressSink&) override {
    ToolResult result;
    ContentBlock text;
    text.type = ContentType::Text;
    text.text = arguments.value("text", std::string{});
    result.structuredContent = Json{{"text", text.text}};
    result.content.push_back(std::move(text));
    return ToolOutcome(std::move(result));
  }
};

} // namespace

int main() {
  using namespace mcp::v1;

  ServerOptions options;
  options.name = "host";
  options.version = "0.1.0";

  // 默认使用 stdin/stdout；库不会 fclose 它们。
  McpServer server(options, makeStdioTransport());

  ToolDescriptor echo;
  echo.name = "echo";
  echo.description = "Return the provided text unchanged";
  // 这里不声明 inputSchema；若声明，必须注入 schemaValidator（见 §9）。
  const Status added =
      server.tools().add(echo, std::make_shared<EchoTool>());
  if (!added) {
    std::cerr << "register echo failed: " << added.error.message << "\n";
    return 1;
  }

  const Status status = server.run(); // 启动传输并阻塞
  if (!status) {
    std::cerr << "server failed: " << status.error.message << "\n";
    return 1;
  }
  return 0;
}
```

要点：

- **stdout 只能写协议**。`StdioTransport` 默认把协议写 `stdout`；宿主日志、
  调试输出、进度提示必须走 `stderr`（或文件/日志系统），否则客户端会收到
  非法 JSON。
- 需要自定义流（管道、socket 包装）时设置
  `StdioOptions{.input = ..., .output = ...}`；仍由宿主负责生命周期。
- `run()` 内部会 `start()` 传输；宿主不必预先 start。若宿主自己先 start
  （例如等待就绪），`StdioTransport`/HTTP transport 的 start 是幂等的，但仍
  建议只在 `run()` 中启动。
- 需要从其他线程停止时：

```cpp
std::thread worker([&server] { (void)server.run(); });
// ... host loop ...
server.stop();   // 线程安全、幂等
worker.join();   // run() 返回并关闭传输、清理回调后再析构 server
```

- 宿主以 in-process 方式提供 MCP；**不要**为了让客户端连接而再启动第二个宿主
  进程，避免设备/单实例争用。

## 5. 把 Streamable HTTP server 嵌入宿主

```cpp
#include <tugmcp/server.hpp>
#include <tugmcp/transport/http.hpp>

#include <iostream>
#include <memory>
#include <thread>

using namespace mcp::v1;

int main() {
  ServerOptions options;
  options.name = "host";

  StreamableHttpServerOptions http_options;
  http_options.bind = "127.0.0.1"; // 安全默认；对外暴露需宿主自行决策
  http_options.port = 8080;
  http_options.maxBodyBytes = 1u << 20;
  http_options.maxConcurrentStreams = 8;

  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created.hasValue()) {
    std::cerr << created.error().message << "\n";
    return 1;
  }

  std::shared_ptr<ITransport> transport = created.value();
  McpServer server(options, transport);

  // ... 注册工具/资源/提示词 ...

  const Status started = transport->start();
  if (!started) {
    std::cerr << started.error.message << "\n";
    return 1;
  }

  if (auto concrete = std::dynamic_pointer_cast<HttpServerTransport>(transport)) {
    std::cerr << "listening on http://" << http_options.bind << ":"
              << concrete->boundPort() << "/mcp\n";
  }

  std::thread worker([&server] { (void)server.run(); });
  // ... 宿主事件循环；退出时：
  server.stop();
  worker.join();
  return 0;
}
```

`run()` 会再次调用 `start()`（幂等）并阻塞。手动先 start 的目的是在
`port = 0` 时读取实际 `boundPort()`；正常路径也可以直接 `server.run()`。

常用选项：

| 字段 | 默认 | 作用/限制 |
|---|---|---|
| `bind` | `127.0.0.1` | 监听地址；默认只绑定回环 |
| `port` | `0` | `0` 由系统分配，用 `boundPort()` 读取 |
| `maxBodyBytes` | `1 MiB` | 必须 > 0，否则创建失败 |
| `maxConcurrentStreams` | `8` | `0` 表示不限制；每条 SSE 流占用一个 cpp-httplib worker |
| `perStreamQueue` | `256` | `0` 表示不限制 |
| `maxSessions` | `128` | 2025 会话上限；超出返回 HTTP 429 |
| `sessionTimeout` | 30 分钟 | 由 `timeSource` 计时 |
| `allowedHosts` / `allowedOrigins` | 空 | 空表示仅回环；`"*"` 表示允许任意。不匹配返回 403 |
| `tls` | 关闭 | TLS 构建中需证书链/私钥；未启用 TLS 的构建中 `enabled = true` 创建即失败 |
| `admission` | 空 | `admit()` 返回 `Accept`/`Reject`/`Emergency`；`Reject` 返回 429 |
| `requestTimeout` | 30 s | 当前约束等待发送响应的时间，不是工具执行超时 |
| `supportedVersions` | 2026-07-28、2025-11-25 | 协议协商集合 |

协议差异：

- **2026-07-28**：无状态；每个请求携带 `_meta` 与
  `MCP-Protocol-Version`/`Mcp-Method`/`Mcp-Name`；无 session、无 GET 流、
  无 `Last-Event-ID`/ping。
- **2025-11-25**：`initialize`/`initialized` 建立会话；`MCP-Session-Id`
  贯穿请求；支持 `GET /mcp` 流、`DELETE /mcp` 关闭会话、per-stream
  `Last-Event-ID` 重放。

`enable2025Session` 字段当前不参与判定；2025 行为由协商出的协议版本驱动。
`emergencyPath` 与 `dedicatedEmergencyListener` 字段尚未接线。

TLS：

```cpp
StreamableHttpServerOptions http_options;
http_options.tls.enabled = true;
http_options.tls.certChainPath = "server.pem";
http_options.tls.privateKeyPath = "server.key";
// 需要 mTLS 时再设置 clientCaFile 和 verifyPeer。
```

TLS 只在 `TUGMCP_TLS=ON` 的构建中可用；证书/私钥必须可读，否则创建失败。

## 6. 把 client 嵌入项目

### 6.1 stdio client

库**不负责启动子进程**。`makeStdioTransport()` 只包装宿主提供的
`FILE*`；如果要对端是子进程，宿主必须自己创建管道/进程，再把读写流交给
`StdioOptions`。

```cpp
StdioOptions io;
io.input = child_stdout_read;   // 宿主创建的 FILE*
io.output = child_stdin_write;  // 宿主创建的 FILE*
McpClient client(ClientOptions{}, makeStdioTransport(io));
const Status connected = client.connect();
```

### 6.2 Streamable HTTP client

```cpp
#include <tugmcp/client.hpp>
#include <tugmcp/transport/http.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace mcp::v1;

int main() {
  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = "http://127.0.0.1:8080";
  transport_options.protocolVersion = "2026-07-28";

  auto transport = makeStreamableHttpClientTransport(transport_options);
  if (!transport.hasValue()) {
    std::cerr << transport.error().message << "\n";
    return 1;
  }

  ClientOptions client_options;
  client_options.name = "host-client";
  client_options.version = "0.1.0";
  client_options.protocolVersion = "2026-07-28";
  client_options.timeoutMs = 30000;

  McpClient client(client_options, transport.value());
  const Status connected = client.connect();
  if (!connected) {
    std::cerr << connected.error.message << "\n";
    client.close();
    return 1;
  }

  CallOptions call;
  call.timeout = std::chrono::seconds(5);
  auto called = client.callTool("echo", Json{{"text", "hello"}}, call);
  if (!called.hasValue()) {
    std::cerr << called.error().message << "\n";
    client.close();
    return 1;
  }

  const Json& result = called.value();
  if (result.value("isError", false)) {
    std::cerr << "tool failed\n";
  } else {
    std::cout << result.at("content").at(0).value("text", "") << "\n";
  }
  client.close();
  return 0;
}
```

关键语义：

- `connect()` 启动传输。2026 路径调用 `server/discover`；失败且
  `allow2025Fallback = true` 时，对可回退错误切换到 2025 并执行
  `initialize`。2025 路径直接 `initialize`。
- `initialize()` 是显式的 **2025** 流程（发送 `initialize` +
  `notifications/initialized`，按 `autoOpenStream` 打开 GET 流）；不要用它
  代替 2026 的 `discover`/`connect`。
- **客户端与传输的 `protocolVersion` 要一致**。`ClientOptions` 默认
  2026-07-28，而 `StreamableHttpClientOptions` 默认 2025-11-25；不显式设置
  两者容易错配。
- HTTPS：`baseUrl` 必须与 `tls.enabled` 匹配（`https://` 对应
  `tls.enabled = true`）；非 TLS 构建中创建 `https://` 传输会失败。
- `listTools`/`callTool`/`readResource` 等返回原始 `Json`，不是强类型对象；
  工具失败看结果里的 `isError`，协议失败看 `Result::error()`。
- 会话/流（2025）：`sessionId()`、`openStream()`、`closeStream()`；
  2026 无 session/GET 流。
- 订阅：`subscribe(SubscriptionFilter, callback)` 返回
  `Result<SubscriptionId>`；`unsubscribe(id)`。2026 不支持按 URI 订阅，
  `filter.resources` 非空会返回 `InvalidParams`；列表变更用
  `filter.listChanged`。
- 取消：`CallOptions::out_id` 拿到请求 id，另一线程调用
  `client.cancel(id, CancelMode::Auto)`；也可在 `CallOptions::cancellation`
  放入 `std::make_shared<CancellationToken>()`。
- 超时：`ClientOptions::timeoutMs` 是默认值；`CallOptions::timeout` 为 0
  时沿用默认值。
- `close()` 幂等；析构也会关闭。

## 7. Executor、线程与关闭

| 配置 | 行为 |
|---|---|
| 不设置 `ServerOptions::executor`，`useInlineExecutor = false` | `run()` 创建内部线程池：**4 workers / 1024 队列**，`run()` 退出时 drain |
| `useInlineExecutor = true` | 在传输线程同步执行 handler；无内部池。非空 `executor` 优先于此开关 |
| 注入 `ServerOptions::executor` | 使用宿主的 `IExecutor`；适用于共享容量/自定义优先级 |

注意事项：

- `run()` 是阻塞调用：启动传输、等待 `stop()`/传输关闭，然后关闭传输、清理
  回调并 drain executor。
- **注入的 executor 也会在 `run()` 退出时被 `drain()`**。`drain()` 是终态：
  之后提交会被拒绝。不要把一个需要继续服务其他组件的全局 executor 直接注入
  给 server，除非接受它在 server 停止时被 drain；需要隔离时使用
  `makeThreadPoolExecutor()` 单独创建。
- `makeThreadPoolExecutor(workers, maxQueue)`：`maxQueue == 0` 表示无界；
  worker 数为 0 会被夹到 1。`Emergency` 提交绕过容量上限，但仍会在
  drain/stop 后拒绝。
- `stop()` 线程安全、幂等：置停止标志、取消进行中的请求 token、清空订阅并
  唤醒 `run()`。宿主应 `stop()` 后 `join()` 运行线程，再析构 server/client。
- 传输与 server 的回调在 `run()` 清理阶段被置空；停止后不再回调。不要在回调
  里析构拥有它的 server；回调是同步/非拥有语义。
- handler 运行在 executor 线程（或 inline 时的传输线程）；不要共享可变
  `Json`，不要从 handler 内部析构 executor。

## 8. 安全 hooks

所有 hook 通过 `ServerOptions` 注入；HTTP 传输在 `run()` 时安装
authenticator/audit。

| Hook | 接口 | 语义 |
|---|---|---|
| 认证 | `IAuthenticator::authenticate(const Headers&)` → `Result<AuthContext>` | HTTP 请求头（名称已小写）认证；失败返回 HTTP **401**，并触发 `auth_failed` 审计 |
| 授权 | `IAuthorizer::authorize(auth, toolOrResource)` → `Status` | 每个 tool 调用、resource read、prompt get 前检查；拒绝返回 `forbidden`，HTTP 传输映射为 **403** |
| 审批 | `IApprovalHook::approve(ApprovalRequest)` → `std::future<ApprovalResult>` | 工具执行前等待审批；默认超时 `approvalTimeout = 30s`；拒绝/超时/取消统一按审批失败处理（HTTP 403） |
| 审计 | `IAuditHook::onEvent(auth, event, detail)` | 观察授权/审批/工具/资源/提示词事件；**异常被吞掉**，不改变授权或执行结果，只通过错误回调/`lastError()` 报告 `audit_failed` |

```cpp
class StderrAudit final : public IAuditHook {
public:
  void onEvent(const AuthContext& auth, std::string_view event,
               const Json& detail) override {
    std::cerr << "audit " << event
              << " principal=" << auth.principal
              << " detail=" << detail.dump() << "\n";
  }
};

ServerOptions options;
options.auth = std::make_shared<MyAuthenticator>();
options.authorizer = std::make_shared<MyAuthorizer>();
options.approval = std::make_shared<MyApproval>();
options.audit = std::make_shared<StderrAudit>();
options.approvalTimeout = std::chrono::seconds(10);
```

边界：

- `AuthContext::authenticated` 与 `roles` 由宿主认证结果决定；库不提供
  OAuth/OIDC 授权服务器，也不内置用户库。
- **能力声明不是授权**：`RequestContext::clientCapabilities` 是客户端自述且
  未经验证，代码注释明确要求不得作为授权输入（D10）。
- `IAuthorizer` 不负责过滤 `tools/list`、`resources/list`、
  `prompts/list`；列表是显式公开枚举（D20）。需要隐藏条目时在 provider/
  宿主层处理，或不要注册。
- stdio 没有 HTTP 头，`IAuthenticator` 的 HTTP 路径不适用；进程边界认证由
  宿主负责。
- 审批 hook 必须在 `approvalTimeout` 内完成 future；取消会丢弃 future。
- 库不保证设备安全；硬件联锁、权限与审批策略属于宿主/应用层。

## 9. 资源、提示词、completion 与 schema

| 能力 | 注入点 | 约束 |
|---|---|---|
| Resources | `ServerOptions::resources`（`IResourceProvider`） | 每类最多一个 provider；provider 拥有命名空间与分页 cursor |
| Prompts | `ServerOptions::prompts`（`IPromptProvider`） | 每类最多一个；列表分页由 provider 负责 |
| Completion | `ServerOptions::completions`（`ICompletionProvider`） | 每类最多一个；返回内层 completion 对象，dispatcher 负责包到 `completion` 键 |
| Schema | `ServerOptions::schemaValidator`（`ISchemaValidator`） | 未配置 validator 时 fail closed：声明了 `inputSchema` 的工具注册失败 |

```cpp
#include <tugmcp/schema/jsoncons.hpp>
#include <tugmcp/server.hpp>

using namespace mcp::v1;

auto options = ServerOptions{};
options.resources = std::make_shared<MyResources>();
options.prompts = std::make_shared<MyPrompts>();
options.completions = std::make_shared<MyCompletions>();

// 需要 2020-12：链接 tugmcp::schema_jsoncons
options.schemaValidator = makeJsonconsSchemaValidator();

// 或 draft-07：链接 tugmcp::schema_valijson
// options.schemaValidator = makeValijsonSchemaValidator();
```

schema 语义：

- `ToolDescriptor::inputSchema` 默认是空对象，表示“未声明 schema”，此时无
  validator 也可以注册。
- 一旦声明了非空 `inputSchema` 而没有 validator，注册失败；除非显式设置
  `allowUnvalidatedTools = true`（仅建议 demo/测试；示例
  `tugmcp_echo_server` 正是这样做的）。
- 显式 `null` schema 在注册时被拒绝。
- 输入校验失败返回 `InvalidParams`（`data.issues`）；输出 schema 校验用于把
  协议层问题转成工具错误。
- `docs/amalgamation.md` 的单头产物不含 schema 后端；生产宿主应链接编译库
  schema target 或注入自己的 `ISchemaValidator`。

## 10. 完整最小宿主与客户端

### 10.1 宿主（stdio server）

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TUGMCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUGMCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/tugmcp tugmcp)

add_executable(host main.cpp)
target_link_libraries(host PRIVATE tugmcp::core)
```

`main.cpp`：使用 §4 的完整示例即可。构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/host   # stdout 是 MCP 协议流；日志请写 stderr
```

### 10.2 客户端

`CMakeLists.txt` 追加：

```cmake
add_executable(host_client client.cpp)
target_link_libraries(host_client PRIVATE tugmcp::http) # PUBLIC 传递 core
```

`client.cpp` 使用 §6.2 的完整示例。构建后：

```bash
./build/host_client
```

若对端是 `tugmcp_echo_server --http --port 8080`，即得到一次
`connect → callTool` 的端到端调用。

## 11. 部署/边界检查清单

- [ ] 线程：server 的 `run()` 有明确的所有者；`stop()` 后 `join()` 再析构。
- [ ] stdout：stdio 模式下 stdout 只输出协议；所有日志/诊断走 stderr。
- [ ] TLS 宏：所有 TU（含直接 include `httplib.h` 的 TU）使用一致的
      `TUGMCP_TLS_ENABLED`/`CPPHTTPLIB_OPENSSL_SUPPORT`；不混用 TLS/非 TLS
      单头产物。
- [ ] 依赖：使用 `3rd/` vendored 依赖；未打开 `TUGMCP_USE_SYSTEM_DEPS`。
- [ ] 单头：只有一个 TU 定义 `TUGMCP_IMPLEMENTATION`；外部头/链接库齐全。
- [ ] schema：生产环境注入真实 validator；没有 schema 的工具要么不声明，
      要么明确接受 `allowUnvalidatedTools`。
- [ ] 安全：auth/authorizer/approval/audit 已接线；401/403；审计异常已演练；
      不把客户端 capability 当授权。
- [ ] 单实例：宿主只启动一个进程/一个设备所有权；不要让客户端另起宿主。
- [ ] 边界：设备控制、任务语义、GUI/UI、OAuth 服务器、硬件联锁没有进入库。
- [ ] 限制：legacy 默认关闭；单头不含 schema/legacy；无 install/export。

## 12. 故障排查与已知限制

| 现象 | 说明/处理 |
|---|---|
| configure 报 `TUGMCP_HEADER_ONLY is not implemented yet` | split-header 模式未实现，是预期失败；改用静态库或单头 artifact |
| `find_package(tugmcp)` 找不到 | 当前无 install/export；改用 `add_subdirectory`/FetchContent/单头 |
| `callTool` 返回值不是强类型 | 设计如此：返回 `Json`；检查 `isError`、`content`、`structuredContent` |
| 客户端 stdio 连不上子进程 | 库不会 spawn 进程；宿主必须自己创建管道/进程并传入 `FILE*` |
| 2025 server 没有 session | 确认客户端/transport 都使用 2025-11-25，且没有混用 2026 默认值 |
| `https://` 客户端创建失败 | 需要 `TUGMCP_TLS=ON`，且 `tls.enabled = true` 与 URL scheme 一致 |
| TLS 客户端证书校验/SNI 不生效 | `TlsOptions::serverName`、`minVersion` 当前未接线；SNI 使用 URL host，证书校验由 `verifyPeer`/`caBundlePath` 控制 |
| legacy 传输不可用 | 需 `TUGMCP_ENABLE_LEGACY_SSE=ON` 编译库；单头不提供 legacy |
| 单头里找不到 schema 后端 | 单头不包含 jsoncons/Valijson；改用编译库 target |
| 注入 executor 后其他组件提交失败 | server `run()` 退出会 drain 注入的 executor（终态）；改用独立 executor |
| Windows 链接错误（socket/TLS） | 单头消费者自行链接 `ws2_32`；TLS 再加 `crypt32`。编译模式由 `tugmcp::http` 传播 |
| HTTP 绑定到 `0.0.0.0` 才可访问 | 默认 `127.0.0.1` 是有意的安全默认；对外暴露需宿主自行评估 Host/Origin/TLS/admission |

其它限制：2026 是 stateless（无 session/GET/Last-Event-ID）；`enable2025Session`
字段未参与判定；`emergencyPath`/`dedicatedEmergencyListener` 未接线；
`requestTimeout` 不是工具执行超时。协议与决策以
[`designs/protocol-decisions.md`](../designs/protocol-decisions.md) 为准。
