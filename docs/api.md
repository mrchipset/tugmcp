# tugmcp 公共 API 参考

> 状态：**M1–M6 完成**（默认静态库 + 单头发布产物）。
>
> 本文按当前 `include/tugmcp/` 的公共头逐项说明 API。设计动机与历史决策见
> [`designs/mcp-library-design.md`](../designs/mcp-library-design.md) §7/§13/§14/§15/§16/§18、
> [`designs/protocol-decisions.md`](../designs/protocol-decisions.md) D17–D20。
> 本文只记录“现在能编译、能观察到的行为”，不把设计草案中的目标 API 当作已实现。

## 0. 相关文档与边界

| 主题 | 入口 |
|---|---|
| 构建、测试、CMake 开关 | [`docs/building.md`](./building.md) |
| 宿主应用如何嵌入、执行器/安全边界 | [`docs/integration.md`](./integration.md) |
| 单头产物消费、链接、TLS/ODR | [`docs/amalgamation.md`](./amalgamation.md) |
| 协议决策 | [`designs/protocol-decisions.md`](../designs/protocol-decisions.md) |
| 单头可行性与限制 | [`designs/header-only.md`](../designs/header-only.md) |

本文中的“库”指 `mcp::v1` 公共 API；“宿主”指链接本库的应用。库不实现设备控制、业务语义、GUI、OAuth 授权服务器或硬件联锁。

---

## 1. 头文件、命名空间与快速索引

### 1.1 命名空间与基础约定

所有公共符号位于 `mcp::v1`（发行目录名是 `include/tugmcp/`，二者不同是有意的）。

```cpp
namespace mcp {
namespace v1 {
using Json = nlohmann::json;   // nlohmann/json 3.12.0
} // namespace v1
} // namespace mcp
```

- 语言下限：C++17。不要依赖 C++20 特性。
- `Json` 是 wire JSON 类型。公共头暴露它；宿主必须使用兼容的 nlohmann/json 版本。
- 错误用值返回，不靠异常穿过传输/线程边界：
  - `Result<T>`：成功值或 `McpError`；
  - `Status`：仅成功/失败，用于无返回值的操作；
  - 失败语义由 `McpError.code` + `message` + 可选 `data` 表达。
- 所有权：
  - `McpServer`/`McpClient` 持有传入的 `std::shared_ptr<ITransport>`；
  - `ServerOptions` 中的 executor、provider、security hook 由 server 持有；
  - provider/handler 由宿主实现，生命周期至少覆盖 server/client 使用期。
- 线程：
  - server handler 在 `IExecutor` 上运行（默认内部线程池；可 `useInlineExecutor` 退回 transport 线程）；
  - 多个请求可能并发进入同一 handler/provider，宿主实现需要自行线程安全；
  - client 的同步调用阻塞调用线程，回调在 transport reader 线程触发；
  - 回调不得阻塞 IO 线程，也不要在回调里做无限等待。

### 1.2 头文件索引

| 头文件 | 主要符号 | 备注 |
|---|---|---|
| `<tugmcp/core.hpp>` | `error.hpp`、`message.hpp`、`operation.hpp`、`prompt.hpp`、`resource.hpp`、`security.hpp`、`version.hpp` 的聚合 | 不含 `McpServer`/`McpClient`/transport |
| `<tugmcp/error.hpp>` | `ErrorCode`、`McpError`、`Result<T>`、`Status` | 基础错误模型 |
| `<tugmcp/message.hpp>` | `RequestId`、`Message`、`parseStrict`、`decode`、`encode` | JSON-RPC 编解码 |
| `<tugmcp/version.hpp>` | `ProtocolEra`、`Era`、`ProtocolVersion`、wire 常量 | 协议代际 |
| `<tugmcp/operation.hpp>` | `ResultType`、`OperationOutcome<T>`、`InputRequired`、`Progress`、`Page<T>`、`SubscriptionFilter` | 操作/分页/订阅基础 |
| `<tugmcp/tool.hpp>` | `ToolDescriptor`、`ContentBlock`、`ToolResult`、`IToolHandler`、`ToolRegistry` | Tool API |
| `<tugmcp/resource.hpp>` | `ResourceDescriptor`、`ResourceTemplate`、`ResourceContent`、`IResourceProvider` | Resource API |
| `<tugmcp/prompt.hpp>` | `PromptDescriptor`、`PromptResult`、`IPromptProvider`、`ICompletionProvider` | Prompt/Completion API |
| `<tugmcp/request_context.hpp>` | `CancellationToken`、`RequestContext` | 每请求上下文 |
| `<tugmcp/schema.hpp>` | `ValidationIssue`、`ValidationReport`、`SchemaOptions`、`ISchemaValidator` | 可插拔 schema |
| `<tugmcp/schema/jsoncons.hpp>` | `makeJsonconsSchemaValidator()` | 需 `TUGMCP_WITH_JSONCONS=ON` |
| `<tugmcp/schema/valijson.hpp>` | `makeValijsonSchemaValidator()` | 需 `TUGMCP_WITH_VALIJSON=ON` |
| `<tugmcp/schema/null.hpp>` | `makeNullSchemaValidator()` | **仅测试**：编译一切、校验一律失败 |
| `<tugmcp/canonical.hpp>` | `sha256Hex`、`canonicalize`、`toolSchemaHash` | canonical JSON |
| `<tugmcp/security.hpp>` | `Headers`、`AuthContext`、`IAuthenticator`、`IAuthorizer`、`IApprovalHook`、`IAuditHook` | 安全 hooks |
| `<tugmcp/executor.hpp>` | `IExecutor`、`Priority`、`ExecutorSubmitResult`、`ExecutorStats`、`makeThreadPoolExecutor` | 执行器 |
| `<tugmcp/server.hpp>` | `ServerOptions`、`McpServer` | 只含声明；transport 需另 include |
| `<tugmcp/client.hpp>` | `ClientOptions`、`CallOptions`、`CancelMode`、`McpClient` | 客户端 |
| `<tugmcp/transport.hpp>` | `ITransport`、`MessageMetadata`、`MessageHandlerEx` | 传输契约 |
| `<tugmcp/transport/stdio.hpp>` | `StdioOptions`、`StdioTransport`、`makeStdioTransport` | stdio |
| `<tugmcp/transport/http.hpp>` | HTTP server/client options、`TlsOptions`、factories | 不 include cpp-httplib |
| `<tugmcp/transport/legacy_sse.hpp>` | legacy 2024-11-05 server/client | 默认 OFF |
| `<tugmcp/time_source.hpp>` | `ITimeSource`、`SystemTimeSource`、`makeSystemTimeSource` | 可注入时钟 |

`<tugmcp/detail/http_httplib.hpp>` 是实现细节，不属于稳定公共 API。

### 1.3 快速索引

| 想做的事 | 入口 |
|---|---|
| 注册工具 | `McpServer::tools()` → `ToolRegistry::add(ToolDescriptor, ToolHandlerPtr)` |
| 注册资源/提示词/补全 | `ServerOptions::resources`、`prompts`、`completions`（每类最多一个） |
| 启动 server | 构造 `McpServer(options, transport)`，注册后 `run()` |
| 停止 server | 任意线程 `stop()`；`run()` 的清理阶段关闭 transport/executor |
| 做客户端 | 构造 `McpClient(options, transport)` → `connect()` → `callTool()` 等 |
| 订阅 2026 通知 | `McpClient::subscribe(SubscriptionFilter, callback)` |
| 2025 资源订阅 | `McpClient::call("resources/subscribe", ...)` + server 侧 `notifyResourceUpdated` |
| 推通知/发 server→client 请求 | `McpServer::sendNotification` / `requestClient`（需要 session 流） |
| 注入执行器 | `ServerOptions::executor = makeThreadPoolExecutor(...)` |
| 开关 schema 校验 | `ServerOptions::schemaValidator` + `allowUnvalidatedTools` |
| 写单头程序 | 一个 TU 定义 `TUGMCP_IMPLEMENTATION`，见 [`docs/amalgamation.md`](./amalgamation.md) |

---

## 2. 基础类型与错误

### 2.1 错误码

```cpp
enum class ErrorCode : int {
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,
  HeaderMismatch = -32020,
  MissingRequiredClientCapability = -32021,
  MissingCapability = MissingRequiredClientCapability,
  UnsupportedVersion = -32022
};
```

`MissingCapability` 是兼容别名；新代码建议写 `ErrorCode::MissingRequiredClientCapability`。应用域错误码从 `-32000` 起，库不占用。

```cpp
std::string toString(ErrorCode code);   // 诊断用名称
```

### 2.2 `McpError`、`Result<T>`、`Status`

```cpp
struct McpError {
  ErrorCode code = ErrorCode::InternalError;
  std::string message;
  Json data = nullptr;
  bool retryable = false;

  static McpError make(ErrorCode code, std::string message,
                       Json data = Json(), bool retryable = false);
};

Json errorToJson(const McpError& error);
Status errorFromJson(const Json& json, McpError& out);
```

| 约定 | 说明 |
|---|---|
| `data` 默认 | `nullptr`；`errorToJson` 只在 `data` 非 null 时输出 `data` |
| `retryable` | 仅 C++ 侧提示，**不序列化**到 wire |
| `code` 默认 | `InternalError` |
| 本地/传输配置错误 | 不进 JSON-RPC 响应；用 `InternalError` + `data.reason`（如 `tls_not_built`、`tls_not_configured`、`unsupported_scheme`、`tls_handshake_failed`、`option_invalid`） |
| 认证失败 | HTTP 用 `401`，**不用** `-32021`；`-32021` 专指缺少客户端能力 |

```cpp
template <class T>
class Result {
public:
  Result(T value);                                  // 隐式，可 `return value;`
  static Result failure(McpError error);

  bool hasValue() const noexcept;
  explicit operator bool() const noexcept;

  T& value();
  const T& value() const;
  McpError& error();
  const McpError& error() const;
};

struct Status {
  bool ok = true;
  McpError error{};
  static Status success() noexcept;
  static Status failure(McpError error);
  explicit operator bool() const noexcept;
};
```

- `Result<T>::value()`/`error()` 访问错误分支会抛 `std::logic_error`，这是**程序员错误**，不是协议错误。公共 API 本身不抛异常。
- `Result<McpError>` 被 `static_assert` 禁止（成功/失败无法区分）；这种场景用 `Status` + 出参。
- `Status` 默认 `ok = true`；失败用 `Status::failure(...)`。可以读 `.ok`，也可以 `if (status)`。
- `Result<T>` 没有默认构造，也没有 `ok` 成员；用 `hasValue()` 或 `if (result)`。

### 2.3 `RequestId` 与 `Message`

```cpp
struct RequestId {
  using Value = std::variant<std::string, std::int64_t, std::uint64_t>;
  Value value;

  static Result<RequestId> fromJson(const Json& json);
  Json toJson() const;

  friend bool operator==(const RequestId&, const RequestId&);
  friend bool operator!=(const RequestId&, const RequestId&);
};
```

- 只接受 JSON string、int64、uint64。无符号分支只用于大于 `INT64_MAX` 的值。
- 数值比较跨 `int64_t`/`uint64_t` 按值进行；`std::hash<RequestId>` 已提供，可作 `unordered_map` key。

```cpp
enum class MessageKind { Request, Notification, Response };

struct Message {
  MessageKind kind = MessageKind::Request;
  Json jsonrpc = "2.0";
  RequestId id{};                 // Request / Response
  std::string method;             // Request / Notification
  Json params = nullptr;          // Request / Notification: object|array|null
  Json result = nullptr;          // Response
  std::optional<McpError> error;  // Response
};
```

### 2.4 严格 JSON 与编解码

```cpp
Result<Json> parseStrict(std::string_view raw);
Result<Message> decode(std::string_view raw);
Result<Message> decode(const std::string& raw);
Result<Message> decode(const char* raw);
Result<Message> decode(const Json& document);
Result<std::string> encode(const Message& message);
```

| 规则 | 行为 |
|---|---|
| 顶层值 | `parseStrict` 接受任意 JSON 顶层值；`decode` 要求 JSON-RPC 对象 |
| UTF-8 | 校验；非法序列 → `ParseError` |
| 重复 key | 任意深度拒绝 → `ParseError` |
| NaN/Inf | 拒绝 |
| 整数精度 | 保留整数语义，不经过 `double` |
| batch | 顶层数组 → `InvalidRequest`（“batch is not supported”） |
| `id` 类型 | float/bool/null 等拒绝 |
| `encode` 失败 | 返回 `InternalError`（例如消息无法表示为严格 JSON） |

`decode(const Json&)` 假定传入值已经由严格解析器得到；它不会重新做文本级 UTF-8/重复 key 检查。

### 2.5 协议代际与版本

```cpp
enum class ProtocolEra { Legacy2024, V2025, V2026 };
using Era = ProtocolEra;

inline constexpr std::string_view kWire2025 = "2025-11-25";
inline constexpr std::string_view kWire2026 = "2026-07-28";
inline constexpr std::string_view kWireLegacy2024 = "2024-11-05";

std::string_view toWire(Era era) noexcept;
std::optional<Era> eraFromWire(std::string_view wire) noexcept;

struct ProtocolVersion {
  int year = 0;
  int month = 0;
  int day = 0;

  std::string toString() const;
  static std::optional<ProtocolVersion> parse(std::string_view text);
  bool operator==(const ProtocolVersion& other) const;
  bool operator!=(const ProtocolVersion& other) const;
};
```

注意：当前 `ServerOptions::supportedVersions`、`ClientOptions::protocolVersion`、HTTP options 用的是 **wire 字符串**（`std::string`/`std::vector<std::string>`），不是 `ProtocolVersion` 值类型。

### 2.6 时间源

```cpp
class ITimeSource {
public:
  virtual ~ITimeSource() = default;
  virtual std::chrono::steady_clock::time_point now() = 0;
};

class SystemTimeSource final : public ITimeSource {
public:
  std::chrono::steady_clock::time_point now() override;
};

inline std::shared_ptr<ITimeSource> makeSystemTimeSource();
```

HTTP session 过期、client 超时等使用注入的 `ITimeSource`；`nullptr` 表示系统时钟。`SystemTimeSource` 的 `now()` 是内联实现。

---

## 3. Tool API

### 3.1 描述、内容与结果

```cpp
enum class ContentType { Text, Image, Audio, ResourceLink, EmbeddedResource };
std::string_view toString(ContentType type) noexcept;

struct ContentBlock {
  ContentType type = ContentType::Text;
  std::string text;
  std::string name;
  std::string description;
  std::string mimeType;
  std::string dataBase64;
  std::string uri;
  Json annotations;
  Json resource;
};

struct ToolDescriptor {
  std::string name;
  std::string title;
  std::string description;
  Json inputSchema = Json::object();
  Json outputSchema;
  Json annotations;
  bool idempotent = false;
};

struct ToolResult {
  bool isError = false;
  ResultType resultType = ResultType::Complete;
  std::vector<ContentBlock> content;
  Json structuredContent;
  std::uint32_t ttlMs = 0;
  std::string cacheScope;
};
```

`ContentBlock` 是聚合体，可以写：

```cpp
ToolResult result;
result.content.push_back(ContentBlock{ContentType::Text, "hello"});
```

wire 上只输出当前 `type` 对应字段：

| `ContentType` | 必填字段 | wire 字段 |
|---|---|---|
| `Text` | `text` | `type:"text"`, `text` |
| `Image`/`Audio` | `dataBase64`, `mimeType` | `type:"image"/"audio"`, `data`, `mimeType`；缺失会返回 `InternalError` + `data.reason="invalid_content_block"` |
| `ResourceLink` | `uri`, `name` | `type:"resource_link"`, `uri`, `name`，可选 `description`/`mimeType` |
| `EmbeddedResource` | `resource` 必须是 JSON 对象 | `type:"resource"`, `resource`（只保留 `uri`/`mimeType`/`text`/`blob`） |

所有类型若 `annotations` 非 null 都会输出。

`ToolDescriptor` 规则：

- `name` 不得为空，且**不得包含 `#`**：输出 schema 的内部 id 是 `"<name>#output"`。
- `inputSchema = {}` 表示“未声明 schema”，不校验；显式 `null` 在注册时被拒绝。
- `outputSchema` 为 null 表示未声明；非 null 时会编译并用于校验 `structuredContent`。
- `idempotent = true` 会写入 `annotations.idempotent = true`；若 `annotations` 非对象则先新建对象。
- `title`/`description` 空字符串仍会出现在 `tools/list` 的 `title`/`description` 字段中（当前实现直接输出）。

### 3.2 `IToolHandler`

```cpp
using ToolOutcome = OperationOutcome<ToolResult>;
using ToolHandlerPtr = std::shared_ptr<IToolHandler>;

class IToolHandler {
public:
  virtual ~IToolHandler() = default;
  virtual Result<ToolOutcome> invoke(const Json& arguments,
                                     RequestContext& context,
                                     IProgressSink& progress) = 0;
};
```

调用语义：

- `arguments` 已规范化为对象；调用前 registry 会按 `inputSchema` 校验。
- `context` 含请求 id、method、取消 token、MRTR 字段、客户端能力、session、auth；生命周期覆盖本次 handler 调用。
- `progress` 只在请求带 `progressToken` 时绑定到 wire 通知；否则是空 sink。`report()` 不会抛，回调失败不影响结果。
- handler 抛出的异常由 dispatcher 捕获并转成 `InternalError`；不要依赖异常做业务错误。
- 业务失败应返回 `ToolResult{isError=true, ...}`，不要返回 JSON-RPC error。
- MRTR 必须返回 `ToolOutcome(InputRequired{...})`，不是设置 `ToolResult::resultType`。

传入 handler 的请求上下文：

```cpp
class CancellationToken {
public:
  void cancel() noexcept;
  bool cancelled() const noexcept;
};

struct RequestContext {
  RequestId id;
  std::string method;
  std::shared_ptr<CancellationToken> cancellation;
  Json inputResponses;
  std::optional<std::string> requestState;
  Json clientCapabilities;
  std::optional<RequestId> subscriptionId;
  std::string sessionId;
  AuthContext auth;
};
```

| 字段 | 语义 |
|---|---|
| `id` / `method` | 当前 JSON-RPC 请求 |
| `cancellation` | 请求生命周期内非空；handler 应周期性检查 `cancelled()` |
| `inputResponses` | MRTR 客户端输入（普通请求为 null/空） |
| `requestState` | MRTR 原样回传的服务端状态 |
| `clientCapabilities` | 客户端声明值；**未验证，不可作授权输入** |
| `subscriptionId` | 订阅流上下文（如有） |
| `sessionId` | 2025 session；2026/stdio 为空 |
| `auth` | transport 认证结果 |

### 3.3 `ToolRegistry`

```cpp
class ToolRegistry {
public:
  void setValidationPolicy(SchemaValidatorPtr validator,
                           bool allowUnvalidatedTools);

  Status add(ToolDescriptor descriptor, ToolHandlerPtr handler);

  std::optional<ToolDescriptor> find(std::string_view name) const;
  ToolHandlerPtr handler(std::string_view name) const;
  std::vector<ToolDescriptor> list() const;
  std::size_t size() const;

  Status validateInput(std::string_view name, const Json& arguments) const;
  Status validateOutput(std::string_view name, const Json& structured) const;
};
```

- registry 内部用互斥量保护，`add`/`find`/`handler`/`list`/`size` 可并发调用；返回 descriptor/vector 是值拷贝。
- 重复名、空名、含 `#`、null schema、null handler 都会让 `add` 返回 `InvalidParams`。
- `McpServer` 构造时会调用 `setValidationPolicy(options.schemaValidator, options.allowUnvalidatedTools)`；**独立使用 registry 时默认是宽松的**，直到你调用 `setValidationPolicy`。
- `McpServer` 的默认策略是 fail closed：工具声明了非空 `inputSchema`、没有 validator、且 `allowUnvalidatedTools=false` 时，`add` 失败。
- `validateInput`/`validateOutput` 在“无 schema”或“无 validator”时返回成功；校验失败返回 `InvalidParams`，`data.issues` 是 `ValidationIssue` 数组。
- 输入校验失败是 JSON-RPC `InvalidParams`；输出校验失败被转换成 **tool error**（`isError=true` + 文本 content），不是协议错误。
- 如果声明了 `outputSchema` 但 handler 返回的 `structuredContent` 为 null，也会变成 tool error。
- 当前 `tools/list` 由 dispatcher 以每页 100 条切片（内部 `pageSize=100`）；`ToolRegistry` 本身不分页。

### 3.4 schema 校验策略

| 场景 | 行为 |
|---|---|
| `inputSchema == {}` | 视为未声明，不校验 |
| `inputSchema == null` | 注册失败 |
| 无 validator + 有 inputSchema + `allowUnvalidatedTools=false` | 注册失败（server 默认） |
| 无 validator + `allowUnvalidatedTools=true` | 允许注册，不校验 |
| 有 validator | 注册时 `compile(name, inputSchema)`；输出 schema 编译为 `name + "#output"` |
| 编译失败 | `add` 返回 `InvalidParams` |
| 调用时输入不合法 | JSON-RPC `InvalidParams`，`data.issues` |
| 输出不合法/缺失 | `ToolResult.isError = true`，附加错误文本 |

生产环境应注入 `makeJsonconsSchemaValidator()` 或 `makeValijsonSchemaValidator()`；`makeNullSchemaValidator()` 只用于测试（它会拒绝所有校验）。

---

## 4. Operation、Resource、Prompt、Completion

### 4.1 `ResultType` 与 `OperationOutcome`

```cpp
enum class ResultType { Complete, InputRequired };
std::string_view toString(ResultType type) noexcept;   // "complete"/"input_required"

struct InputRequired {
  std::optional<Json> inputRequests;
  std::optional<std::string> requestState;
  bool valid() const noexcept;   // 两者都必须有值
};

template <class T>
struct OperationOutcome {
  std::variant<T, InputRequired> value;

  OperationOutcome(T complete);        // 隐式，可 `return result;`
  OperationOutcome(InputRequired input);

  bool isInputRequired() const noexcept;
  T& result();
  const T& result() const;
  InputRequired& inputRequired();
  const InputRequired& inputRequired() const;
};
```

- 构造 `InputRequired` 时，`inputRequests` 与 `requestState` **必须同时存在**；否则适配层返回 `InternalError` + `data.reason="invalid_input_required"`。
- 访问错误的 `OperationOutcome` 分支（对 `input_required` 调 `result()`，或对 complete 调 `inputRequired()`）会抛 `std::logic_error`，与 `Result` 的访问器策略一致。
- MRTR 只在 2026-07-28 支持；2025 返回 `InternalError` + `data.reason="input_required_unsupported"`。
- 客户端 MRTR 重试是**新请求**：客户端选择新 JSON-RPC id，原样回传 `requestState`，并带 `inputResponses`。
- 注意：`ToolResult`/`ResourceContent`/`PromptResult` 里的 `resultType` 字段只是值类型的一部分；**当前适配层用 `OperationOutcome` 是否处于 `InputRequired` 分支来决定 wire 的 `resultType`**，不会读取该字段来触发 MRTR。

### 4.2 进度

```cpp
using ProgressToken = std::variant<std::string, std::int64_t>;

Result<ProgressToken> progressTokenFromJson(const Json& json);
Json progressTokenToJson(const ProgressToken& token);

struct Progress {
  ProgressToken token;
  double progress = 0.0;
  std::optional<double> total;
  std::string message;
};

class IProgressSink {
public:
  virtual ~IProgressSink() = default;
  virtual void report(double fraction, std::string_view message) = 0;
};
```

- token 只接受字符串或 int64；uint64 超过 `INT64_MAX`、bool、float、null、复合值都拒绝为 `InvalidParams`。
- handler 的 `IProgressSink::report()` 只在请求带 `progressToken` 时产生 `notifications/progress`；否则丢弃。
- 客户端同一个连接上重复使用仍活跃的 `progressToken` 会得到 `InvalidParams`（“progressToken already in use”）。

### 4.3 分页与列表

```cpp
struct ListParams {
  std::string cursor;
};

template <class T>
struct Page {
  std::vector<T> items;
  std::optional<std::string> nextCursor;
  std::uint32_t ttlMs = 0;
  std::string cacheScope;
};

enum class ListKind { Tools, Prompts, Resources };
using SubscriptionId = std::string;

struct SubscriptionFilter {
  std::vector<ListKind> listChanged;
  std::vector<std::string> resources;
};
```

- `cursor` 是不透明字符串；provider 负责语义。`nextCursor` 没有值表示没有下一页。
- 2026 的 list/read 结果带 `ttlMs`/`cacheScope`；`cacheScope` 为空时输出 `"private"`。2025 不输出这些字段。
- tool 列表的 cursor 由 dispatcher 解析为数字 offset；provider 列表的 cursor 完全由 provider 解释。
- `SubscriptionFilter::resources` 只适用于 2025 的 per-URI 订阅；2026 下非空会被拒绝。

### 4.4 Resource

```cpp
struct ResourceDescriptor {
  std::string uri;
  std::string name;
  std::string title;
  std::string description;
  std::string mimeType;
  std::uint64_t size = 0;
  Json annotations;
};

struct ResourceTemplate {
  std::string uriTemplate;
  std::string name;
  std::string description;
  std::string mimeType;
};

struct ResourceContent {
  std::string uri;
  std::string mimeType;
  Json text;
  Json blob;
  ResultType resultType = ResultType::Complete;
  std::uint32_t ttlMs = 0;
  std::string cacheScope;
};

using ResourceOutcome = OperationOutcome<ResourceContent>;

class IResourceProvider {
public:
  virtual ~IResourceProvider() = default;
  virtual Result<Page<ResourceDescriptor>> list(const ListParams& params,
                                                RequestContext& context) = 0;
  virtual Result<Page<ResourceTemplate>> templates(const ListParams& params,
                                                   RequestContext& context) = 0;
  virtual Result<ResourceOutcome> read(std::string_view uri,
                                       RequestContext& context) = 0;
};
```

- 一个 server 只支持**一个** resource provider；provider 拥有整个 URI 命名空间、分页语义和 URI 级授权。
- `ServerOptions::resources == nullptr` 时，`resources/list`、`resources/templates/list`、`resources/read` 返回 `MethodNotFound`。
- `read` 的 wire 结果是 `contents` 数组；当前实现每次只放一个元素。`text`/`blob` 为 null 时不输出。
- `ResourceDescriptor::size == 0` 时不输出 `size`；`title`/`description`/`mimeType` 空时不输出。
- `ResourceTemplate` 的 cache 字段在 `Page` envelope，不在模板项里。

### 4.5 Prompt 与 completion

```cpp
struct PromptDescriptor {
  std::string name;
  std::string description;
  Json arguments;
};

struct PromptResult {
  std::string description;
  Json messages;
  ResultType resultType = ResultType::Complete;
};

using PromptOutcome = OperationOutcome<PromptResult>;

class IPromptProvider {
public:
  virtual ~IPromptProvider() = default;
  virtual Result<Page<PromptDescriptor>> list(const ListParams& params,
                                              RequestContext& context) = 0;
  virtual Result<PromptOutcome> get(std::string_view name, const Json& args,
                                    RequestContext& context) = 0;
};

class ICompletionProvider {
public:
  virtual ~ICompletionProvider() = default;
  virtual Result<Json> complete(const Json& ref, const Json& argument,
                                RequestContext& context) = 0;
};
```

- 每类 provider 最多一个；`prompts == nullptr` 时 `prompts/list`/`prompts/get` 返回 `MethodNotFound`。
- `PromptResult::messages` 会原样写到 wire 的 `messages`；`description` 为空时不输出。
- `ICompletionProvider::complete` 返回**内层 completion 对象**，例如 `{"values":[...],"hasMore":true}`；dispatcher 把它包在结果的 `completion` 键下。
- `completions == nullptr` 时仍声明 `completions` capability，并返回 `{"values":[]}`。
- dispatcher 会先校验 `ref`/`argument`：`ref` 必须是 `ref/prompt`（带 string `name`）或 `ref/resource`（带 string `uri`），`argument` 必须有 string `name`/`value`；否则 `InvalidParams`。
- 2026 的结果会补 `resultType:"complete"`；2025 不补。

---

## 5. Server API

### 5.1 `ServerOptions`（全部字段）

```cpp
struct ServerOptions {
  std::string name = "tugmcp";
  std::string version = "0.1.0";
  std::string instructions;

  std::vector<std::string> supportedVersions{"2026-07-28",
                                               "2025-11-25"};
  std::shared_ptr<ITimeSource> timeSource;

  std::shared_ptr<IResourceProvider> resources;
  std::shared_ptr<IPromptProvider> prompts;
  std::shared_ptr<ICompletionProvider> completions;
  std::shared_ptr<IAuthenticator> auth;
  std::shared_ptr<IAuthorizer> authorizer;
  std::shared_ptr<IApprovalHook> approval;
  std::shared_ptr<IAuditHook> audit;
  std::chrono::milliseconds approvalTimeout{30000};

  SchemaValidatorPtr schemaValidator;
  bool allowUnvalidatedTools = false;

  std::shared_ptr<IExecutor> executor;
  bool useInlineExecutor = false;
};
```

| 字段 | 默认/语义 |
|---|---|
| `name` / `version` | `serverInfo` 与 discovery/initialize 响应 |
| `instructions` | 非空时写入 discovery/initialize 的 `instructions` |
| `supportedVersions` | wire 字符串；默认 2026 与 2025。HTTP 下由 `run()` 推送给 transport |
| `timeSource` | null = 系统时钟；用于 HTTP session 过期等 |
| `resources` / `prompts` / `completions` | 每类最多一个；null 时对应方法 `MethodNotFound`（completion 例外：返回空 values） |
| `auth` | 由 `run()` 推送到 transport；stdio 没有头，通常不启用 |
| `authorizer` | 在 `tools/call`、`resources/read`、`prompts/get`、2025 `resources/subscribe` 前调用 |
| `approval` | 只在 `tools/call`，授权通过后调用 |
| `audit` | 观察 security 事件；异常被库收敛 |
| `approvalTimeout` | 等待 `approve()` future 的上限，默认 30s |
| `schemaValidator` | 注册/调用时校验工具 schema；null 时按 fail-closed 规则处理 |
| `allowUnvalidatedTools` | 允许“声明了 schema 但没有 validator”的工具注册 |
| `executor` | 非 null 时**总是优先**；null 且 `useInlineExecutor=false` 时 `run()` 内部创建 4 worker/1024 队列线程池 |
| `useInlineExecutor` | 仅当 `executor == nullptr` 时生效：在 transport 线程同步执行 |

### 5.2 `McpServer` 全部 public 方法

```cpp
class McpServer {
public:
  McpServer(ServerOptions options, std::shared_ptr<ITransport> transport);
  ~McpServer();

  ToolRegistry& tools() noexcept;

  Status run();
  void stop() noexcept;

  Result<Json> requestClient(
      std::string_view sessionId,
      std::string_view method,
      Json params = Json::object(),
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  Status sendNotification(std::string_view sessionId,
                          std::string_view method,
                          Json params = Json::object());

  void notifyResourceUpdated(std::string_view uri);
  void notifyListChanged(ListKind kind);
  std::size_t activeSubscriptions() const;
  std::optional<std::string> logLevel(std::string_view sessionId) const;

  std::optional<McpError> lastError() const;
};
```

| 方法 | 语义/线程 |
|---|---|
| `tools()` | 返回 registry 引用，生命周期同 server。可在 `run()` 前后注册；建议启动前完成注册 |
| `run()` | 启动 transport（`start()` 幂等），阻塞到 `onClose`/`stop()`，然后做清理并关闭 transport |
| `stop()` | 线程安全、幂等；置停止标志、取消所有在途请求 token、清空 2026 订阅、唤醒 `run()`。**不**在调用线程关闭 transport；由 `run()` 清理阶段完成 |
| `requestClient(...)` | 发 server→client 请求并等待响应；需要 server 管理的 session 流（2025 HTTP）。超时返回 `InternalError` + `data.reason="timeout"`；无 session 流时返回 transport 错误 |
| `sendNotification(...)` | 通过 session 流发通知；需要 session（2025 HTTP） |
| `notifyResourceUpdated(uri)` | 向 2025 per-URI 订阅者（`resources/subscribe`）发 `notifications/resources/updated`；2026 没有 per-URI 订阅，只有 `notifyListChanged(ListKind::Resources)` 的列表变更通知。2025 发送失败只记录错误，不移除订阅 |
| `notifyListChanged(kind)` | 向匹配的 2026 `subscriptions/listen` 订阅发 `notifications/<kind>/list_changed`；不会自动因 `tools().add()` 触发 |
| `activeSubscriptions()` | 当前 2026 subscription 数量 |
| `logLevel(sessionId)` | 2025 `logging/setLevel` 保存的级别；未设置返回 `nullopt` |
| `lastError()` | 最近观察到的 transport/protocol 错误；诊断用 |

### 5.3 `run()` 生命周期与关闭语义

1. 校验 transport 非 null。
2. 安装 dispatcher 配置和 transport 回调（消息、错误、关闭、route/session closed）。
3. 把 `supportedVersions`、`auth`、`audit` 推送给 transport。
4. `transport->start()`。
5. 阻塞直到 `stop()` 或 transport 关闭。
6. 清理（即使异常/提前返回）：
   - 置 `stopping_`；
   - 对 executor 调用 `drain()`（默认内部池或注入的 executor）；
   - 等待 in-flight 计数归零（inline 模式下 handler 仍在 transport 线程）；
   - `transport->close()`；
   - 清空回调、取消 token、2026 订阅、logging/资源订阅状态；
   - 结束所有 `requestClient` 等待（返回 `transport_closed`）。

注入 executor 时注意：`run()` 会 `drain()` 它（终止性，之后拒绝新任务）。如果宿主还与其他组件共享该 executor，需要宿主自行管理生命周期或提供独立 executor。

### 5.4 通知、订阅与 server→client 请求

- 2026 订阅由 `subscriptions/listen` 建立；首帧是 `notifications/subscriptions/acknowledged`，`params._meta["io.modelcontextprotocol/subscriptionId"]` 的值是 listen 请求 id 的**原始 JSON 类型**。
- 之后所有该流通知的 `params._meta["io.modelcontextprotocol/subscriptionId"]` 必须与它类型和值都一致。
- `notifyListChanged(ListKind::Tools/Prompts/Resources)` 只发给 `SubscriptionFilter::listChanged` 匹配者。
- 2025 没有 `subscriptions/listen`；per-URI 订阅通过 `resources/subscribe`/`resources/unsubscribe`，通知走 session 的 GET 流。
- 2026 没有 per-URI 订阅；不要依赖 `SubscriptionFilter::resources`。
- `requestClient`/`sendNotification` 依赖 `sendToSession`，因此主要面向 2025 HTTP session；2026 stateless 传输没有 session 流。
- 失败订阅的处理：发送失败（`route_not_found`）时移除订阅；其他错误记录到 `lastError()`。

---

## 6. Client API

### 6.1 `ClientOptions` 与 `CallOptions`

```cpp
enum class CancelMode { Auto, CloseStream, Notification };

struct ClientOptions {
  std::string name = "tugmcp-client";
  std::string version = "0.1.0";
  std::uint32_t timeoutMs = 30000;
  std::string protocolVersion = "2026-07-28";
  bool allow2025Fallback = true;
  bool autoOpenStream = true;
  Json capabilities = Json{{"roots", Json::object()},
                           {"sampling", Json::object()},
                           {"elicitation", Json::object()}};
  std::shared_ptr<ITimeSource> timeSource;
};

struct CallOptions {
  std::chrono::milliseconds timeout{0};
  std::optional<ProgressToken> progressToken;
  std::shared_ptr<CancellationToken> cancellation;
  std::optional<std::string> requestState;
  Json inputResponses;
  std::function<void(const Progress&)> onProgress;
  std::function<void(const Json&)> onNotification;
  RequestId* out_id = nullptr;
};
```

| 字段 | 语义 |
|---|---|
| `name`/`version` | `clientInfo`，2026 注入 `_meta["io.modelcontextprotocol/clientInfo"]`（SHOULD） |
| `timeoutMs` | 单次调用的默认超时；`CallOptions::timeout == 0` 时使用它 |
| `protocolVersion` | 客户端**认为**的版本；必须与 transport 的版本一致（HTTP client 默认是 2025，见 §7.3） |
| `allow2025Fallback` | 2026 `connect()` 失败且错误为 `UnsupportedVersion`/`MethodNotFound`/`InvalidParams` 时，切换到 2025 并 `initialize()` |
| `autoOpenStream` | 2025 `initialize()` 成功后自动打开 server→client 流（若 transport 支持） |
| `capabilities` | 作为 `clientCapabilities` 发送；**未经验证，不能作为授权输入** |
| `timeSource` | null = 系统时钟；用于超时/期限 |
| `CallOptions::progressToken` | 写入 `params._meta.progressToken`；要求 params 是对象 |
| `CallOptions::cancellation` | 调用线程轮询该 token；取消后返回 `data.reason="cancelled"`，并尝试传输层取消 |
| `CallOptions::requestState` | MRTR 原样回传 |
| `CallOptions::inputResponses` | MRTR 客户端输入；非 null 时写入 params |
| `CallOptions::onProgress` | 只在该请求收到 `notifications/progress` 时调用 |
| `CallOptions::onNotification` | 该请求上下文内的其他通知回调 |
| `CallOptions::out_id` | 发送前写入 JSON-RPC id，便于随后 `cancel()` |

超时/取消/关闭时，客户端返回 `Result<Json>` 失败；错误 `data.reason` 常见为 `timeout`、`cancelled`、`transport_closed`、`send_failed`。本地超时不会强杀服务端 handler；2025 会额外尝试发送 `notifications/cancelled`，2026 走关闭 stream 的路径。

### 6.2 `McpClient` 全部方法

```cpp
class McpClient {
public:
  McpClient(ClientOptions options, std::shared_ptr<ITransport> transport);
  ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  Status connect();
  Status initialize();
  Result<Json> discover();

  Result<Json> call(std::string_view method,
                    Json params = Json::object(),
                    const CallOptions& options = {});

  Result<Json> listTools(std::string_view cursor = {});
  Result<Json> callTool(std::string_view name,
                        Json arguments = Json::object(),
                        const CallOptions& options = {});

  Result<Json> listResources(std::string_view cursor = {});
  Result<Json> listResourceTemplates(std::string_view cursor = {});
  Result<Json> readResource(std::string_view uri,
                            const CallOptions& options = {});

  Result<Json> listPrompts(std::string_view cursor = {});
  Result<Json> getPrompt(std::string_view name,
                         Json arguments = Json::object(),
                         const CallOptions& options = {});

  std::string sessionId() const;

  Status openStream();
  void closeStream();

  void setRequestHandler(std::string method,
                         std::function<Result<Json>(const Json&)> handler);
  void setOnNotification(std::function<void(const Json&)> handler);

  Result<SubscriptionId> subscribe(
      const SubscriptionFilter& filter,
      std::function<void(const Json&)> onNotification);
  Status unsubscribe(const SubscriptionId& id);

  Status cancel(const RequestId& id,
                CancelMode mode = CancelMode::Auto);

  void close() noexcept;

  std::optional<McpError> lastTransportError() const;
};
```

### 6.3 `connect()` / `discover()` / `initialize()`

- `connect()`：
  1. `transport->start()`；
  2. 若 `options.protocolVersion == "2026-07-28"`，调用 `server/discover`；
  3. discover 失败且错误码属于 `UnsupportedVersion`/`MethodNotFound`/`InvalidParams`，并且 `allow2025Fallback=true`，则 `transport->setProtocolVersion("2025-11-25")`，改走 `initialize()`；
  4. 否则返回失败。
- `initialize()`：发送 2025 `initialize`（protocolVersion、capabilities、clientInfo），再发 `notifications/initialized`；如果 `autoOpenStream=true`、版本是 2025、transport 支持 server stream，则尝试 `openStream()`（`stream_not_supported` 被忽略）。
- `discover()`：发送 `server/discover`，返回原始 `Json`。
- **不要**在 2026 服务端上显式调用 `initialize()`：2026 已移除该方法，会得到 `MethodNotFound`。用 `connect()`。

### 6.4 调用与返回值

- 所有高层方法（`listTools`、`callTool`、`readResource` 等）返回 `Result<Json>`，**没有强类型包装**。调用方需要自己检查字段（例如 `content`、`structuredContent`、`isError`）。
- `call()` 在 2026 且 params 是对象时自动注入 `_meta` 的 protocolVersion/clientCapabilities/clientInfo；`progressToken`、`requestState`、`inputResponses` 也在 params 层。
- 如果给 2026 传非对象 params，不会注入 `_meta`，服务端通常会以 `InvalidParams` 拒绝；请用对象。
- 同一个 `progressToken` 不能同时用于两个未完成请求。
- 同步调用会创建一个后台 sender 线程并等待响应；不要从 transport 回调线程发起无限等待的同步调用。
- `close()` 幂等：结束 pending 调用（`transport_closed`）、取消订阅、关闭流和 transport。

### 6.5 session、stream、订阅与取消

| 方法 | 行为 |
|---|---|
| `sessionId()` | 返回 transport 当前 session id；2026/stdio 通常为空 |
| `openStream()` | 打开 server→client 流（2025 GET /mcp）；不支持时返回 `stream_not_supported` |
| `closeStream()` | 关闭该流；无返回值 |
| `setRequestHandler(method, handler)` | 处理 server→client 请求（例如 `roots/list`、`sampling/createMessage`、`elicitation/create`）。未注册时回 `MethodNotFound` |
| `setOnNotification(handler)` | 全局通知回调；与单次调用 `CallOptions::onNotification` 分开 |
| `subscribe(filter, cb)` | 发 `subscriptions/listen`，等待 `notifications/subscriptions/acknowledged`；返回 `SubscriptionId`（内部是 listen 请求 id 的 dump 字符串），callback 在该流的订阅通知上触发 |
| `unsubscribe(id)` | 本地注销 + `transport->cancelPending`；不发送额外的 wire 取消消息 |
| `cancel(id, mode)` | 本地结束 pending 调用；`CloseStream`/2026 `Auto` 调 `cancelPending`；`Notification`/2025 `Auto` 发 `notifications/cancelled` |
| `lastTransportError()` | 最近 transport 错误；**不会**终止 pending 调用，它们继续等响应/超时/关闭 |

订阅限制：

- `subscribe()` 当前发送的是 `subscriptions/listen`，面向 2026 流式 transport。
- `filter.resources` 非空直接返回 `InvalidParams`（“per-resource subscriptions are not supported in 2026”）。
- 2025 服务端对 `subscriptions/listen` 返回 `MethodNotFound`；2025 的 per-URI 订阅要用通用 `call()`。
- 非流式 transport（如 stdio）服务端会返回 `InternalError` + `data.reason="subscriptions_require_stream"`。

`CancelMode` 语义：

| 模式 | 2026 | 2025 |
|---|---|---|
| `Auto` | 关闭该请求的 SSE/流（`cancelPending`） | 发 `notifications/cancelled` |
| `CloseStream` | `cancelPending` | `cancelPending` |
| `Notification` | 返回 `InvalidParams`（reason `notification_cancel_unsupported`） | 发 `notifications/cancelled` |

`cancel()` 对未知 id 返回 `InternalError` + `data.reason="unknown_request"`；对已完成请求返回成功（幂等）。

---

## 7. Transport API

### 7.1 `ITransport` 契约

```cpp
struct MessageMetadata {
  std::string protocolVersion;
  std::string route;
  std::string sessionId;
  Json clientCapabilities;
  AuthContext auth;
  bool emergency = false;
};

using MessageHandlerEx = std::function<void(Json, const MessageMetadata&)>;

class ITransport {
public:
  virtual ~ITransport() = default;

  virtual Status start() = 0;
  virtual Status send(const Json& message) = 0;

  virtual Status sendTo(std::string_view route, const Json& message);
  virtual Status sendToSession(std::string_view sessionId,
                               const Json& message);
  virtual void cancelPending(const RequestId&);

  virtual void setAuthenticator(std::shared_ptr<IAuthenticator>);
  virtual void setAuditHook(std::shared_ptr<IAuditHook>);
  virtual void setSupportedVersions(std::vector<std::string>);
  virtual bool supportsStreaming() const;

  virtual Status openStream();
  virtual void closeStream();
  virtual std::string sessionId() const;
  virtual bool supportsServerStream() const;

  virtual Status setProtocolVersion(std::string_view);

  virtual void setOnSessionClosed(std::function<void(const std::string&)>);
  virtual void setOnRouteClosed(std::function<void(const std::string&)>);

  virtual Status sendRaw(std::string_view raw);

  virtual void setOnMessage(std::function<void(Json)> handler) = 0;
  virtual void setOnMessageEx(MessageHandlerEx handler);

  virtual void setOnError(std::function<void(McpError)> handler) = 0;
  virtual void setOnClose(std::function<void()> handler) = 0;
  virtual void close() = 0;
};
```

| 方法 | 契约 |
|---|---|
| `start()` | 启动接收侧；幂等。第二次调用（未 close）返回成功且无副作用 |
| `send()` | 线程安全；close 后失败 |
| `sendTo(route, ...)` | route 是 `MessageMetadata::route` 给的不透明 token；单路由 transport 默认退回 `send()` |
| `sendToSession(sessionId, ...)` | 2025 session 流；默认返回 `InternalError` + `data.reason="session_not_supported"` |
| `cancelPending(id)` | best-effort，不阻塞；不能中断的 transport 可为 no-op |
| `setAuthenticator`/`setAuditHook` | server transport 安装安全 hook；默认 no-op |
| `setSupportedVersions` | server transport 版本协商；默认 no-op |
| `supportsStreaming()` | 是否支持 per-request server stream（订阅）；默认 false |
| `openStream()`/`closeStream()`/`sessionId()`/`supportsServerStream()` | 2025 GET 流；默认不支持/空 |
| `setProtocolVersion()` | client transport 更新后续发送使用的版本；默认成功 no-op |
| `setOnSessionClosed`/`setOnRouteClosed` | 可选生命周期通知；单路由 transport 可不用 |
| `sendRaw(raw)` | 默认先 `parseStrict` 再 `send`；stdio 用它保留原始字节 |
| `setOnMessage` | 纯虚；必须在 `start()` 前设置 |
| `setOnMessageEx` | 带元数据的接收回调；默认适配到 `setOnMessage`，元数据为空 |
| `setOnError` | 传输/framing 错误，不是协议 dispatch 错误 |
| `setOnClose` | EOF/close 时调用一次 |
| `close()` | 幂等；返回后不再有 onMessage/onError（除一次 onClose） |

`MessageMetadata` 字段：

- `protocolVersion` 空表示 transport 没有观察到的显式版本（stdio 或 legacy client）。
- `route` 空表示单逻辑路由，`sendTo` 回落 `send`。
- `sessionId` 仅 session transport 有值。
- `clientCapabilities` 是客户端声明值（2026 来自 `_meta`；2025 来自 initialize），**未验证，不可作为授权输入**。
- `auth` 由 transport 认证结果填充。
- `emergency` 由 HTTP admission hook 标记，server 投递到 executor 时用 `Priority::Emergency`。

### 7.2 stdio

```cpp
struct StdioOptions {
  std::FILE* input = stdin;
  std::FILE* output = stdout;
  std::size_t maxMessageBytes = 4u * 1024u * 1024u;
  bool setBinaryMode = true;
};

class StdioTransport final : public ITransport {
public:
  explicit StdioTransport(StdioOptions options = {});
  ~StdioTransport() override;

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  Status start() override;
  Status send(const Json& message) override;
  Status sendRaw(std::string_view raw) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void close() override;
};

std::shared_ptr<ITransport> makeStdioTransport(StdioOptions options = {});
```

- newline-delimited JSON；流**不归 transport 所有**，不会 `fclose(input/output)`。
- 单路由、非流式：不支持 session/订阅流/认证头。
- `maxMessageBytes` 限制单条消息；`setBinaryMode` 在 Windows 上设置二进制模式（默认 true）。
- stdio 没有版本元数据，dispatcher 把空 `protocolVersion` 当作 **2025-11-25**。因此 stdio server 的 `supportedVersions` 必须包含 `"2025-11-25"`（默认包含）；stdio 上的 2026 语义不会完整生效。

### 7.3 Streamable HTTP

#### TLS 选项

```cpp
struct TlsOptions {
  bool enabled = false;
  std::string certChainPath;
  std::string privateKeyPath;
  std::string privateKeyPassword;   // 尚未接线
  std::string clientCaFile;
  bool verifyPeer = true;
  std::string caBundlePath;
  std::string serverName;           // client-only，尚未接线（SNI 使用 URL host）
  std::string minVersion = "TLS1.2"; // client-only，尚未接线
};
```

- TLS 需要构建时 `TUGMCP_TLS=ON` 且 OpenSSL ≥ 3.0；未构建时启用 TLS 会在 transport 创建期返回 `InternalError` + `data.reason="tls_not_built"`。
- server 启用 TLS 缺 cert/key → `tls_not_configured`；路径/字段组合非法 → `option_invalid`。
- client `https://` 但未启用 TLS → `unsupported_scheme`，不会发网络请求。
- 握手/证书失败：client 在首个请求的 `Result` 上报；server 在连接级经 `onError`，不产生 JSON-RPC 响应。

#### server 选项与工厂

```cpp
enum class AdmissionDecision { Accept, Reject, Emergency };

class IAdmissionHook {
public:
  virtual ~IAdmissionHook() = default;
  virtual AdmissionDecision admit(const RequestContext& context,
                                  const Json& request) = 0;
};

struct StreamableHttpServerOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  std::size_t maxBodyBytes = 1u << 20;
  std::size_t maxConcurrentStreams = 8;   // 0 = unbounded
  std::size_t perStreamQueue = 256;       // 0 = unbounded
  bool enable2025Session = true;          // 头文件标注尚未接线
  std::shared_ptr<IAdmissionHook> admission;
  std::string emergencyPath = "/emergency";          // 尚未接线
  bool dedicatedEmergencyListener = false;            // 尚未接线
  TlsOptions tls;
  std::shared_ptr<ITimeSource> timeSource;
  std::chrono::milliseconds requestTimeout{30000};
  std::size_t maxSessions = 128;
  std::chrono::milliseconds sessionTimeout{std::chrono::minutes(30)};
  std::vector<std::string> supportedVersions{"2026-07-28",
                                             "2025-11-25"};
  std::vector<std::string> allowedHosts;
  std::vector<std::string> allowedOrigins;
};

class HttpServerTransport final : public ITransport {
public:
  ~HttpServerTransport() override;

  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  Status sendToSession(std::string_view sessionId,
                       const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void setOnRouteClosed(
      std::function<void(const std::string&)> handler) override;
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override;
  bool supportsStreaming() const override;
  void setSupportedVersions(std::vector<std::string> versions) override;
  void setAuthenticator(std::shared_ptr<IAuthenticator> authenticator) override;
  void setAuditHook(std::shared_ptr<IAuditHook> audit) override;
  void close() override;

  int boundPort() const;
};

Result<std::shared_ptr<ITransport>> makeStreamableHttpServerTransport(
    const StreamableHttpServerOptions& options);
```

| 字段 | 语义 |
|---|---|
| `bind` | 默认 `127.0.0.1`；LAN 暴露必须显式改并承担风险 |
| `port` | `0` = 系统分配；用 `boundPort()` 读取 |
| `maxBodyBytes` | 请求体上限，超限拒绝 |
| `maxConcurrentStreams` | 同时 SSE 流上限；0 = 不限 |
| `perStreamQueue` | 每流排队帧上限；0 = 不限 |
| `enable2025Session` | 头文件标注尚未接线；当前实现不读取该字段（2025 session 逻辑本身仍存在，见下） |
| `admission` | 在 body 解析后调用；`Reject` → HTTP 429；抛异常 → HTTP 500；`Emergency` → executor `Priority::Emergency` |
| `emergencyPath` / `dedicatedEmergencyListener` | 头文件标注尚未接线（P3） |
| `tls` | 见上 |
| `timeSource` | session 过期时钟 |
| `requestTimeout` | HTTP 请求等待消息处理完成（含 handler 执行）的上限；超时返回 HTTP 504 |
| `maxSessions` | 新 session 超限 → HTTP 429 |
| `sessionTimeout` | 2025 session 过期 |
| `supportedVersions` | 服务器接受/宣告的 wire 版本；`McpServer::run()` 会用 `ServerOptions` 覆盖推送 |
| `allowedHosts`/`allowedOrigins` | 空 = 仅 loopback；`"*"` = 允许任意。Host 大小写不敏感、去端口；非法 → 403 |

HTTP 行为要点：

- `HttpServerTransport` 的构造函数是私有的；必须用工厂。
- `boundPort()` 在 `start()` 后才有意义。
- 2026 的 POST 需要 `MCP-Protocol-Version`、`_meta`、`Mcp-Method`；`tools/call` 还需要 `Mcp-Name`。
- 2026 `GET /mcp` 返回 405；`initialize` 等已移除方法返回 404 + `-32601`。
- 2025 session：`initialize` 响应带 `MCP-Session-Id`；后续 POST 必须带；`GET /mcp` 是 server→client 流，支持 `Last-Event-ID` 重放；`DELETE /mcp` 关闭 session；缺失/未知 session 分别 400/404；超限 429。
- 每个打开的 SSE 流会占用一个 cpp-httplib worker 线程；worker 池按流上限扩容，避免 JSON 请求被饿死。

#### client 选项与工厂

```cpp
struct StreamableHttpClientOptions {
  std::string baseUrl;
  std::chrono::milliseconds requestTimeout{30000};
  std::size_t maxSseQueue = 256;
  TlsOptions tls;
  std::shared_ptr<ITimeSource> timeSource;
  std::string protocolVersion = "2025-11-25";
};

Result<std::shared_ptr<ITransport>> makeStreamableHttpClientTransport(
    const StreamableHttpClientOptions& options);
```

- `baseUrl` 必填；`https://` 需要 TLS 构建 + `tls.enabled`。
- `protocolVersion` 默认是 **2025-11-25**，与 `ClientOptions` 默认的 2026 不同；同时使用时必须显式对齐（通常两者都设为 `"2026-07-28"` 或都设为 `"2025-11-25"`）。
- `Mcp-Method`/`Mcp-Name` 只对 2026 请求附加。
- `maxSseQueue` 控制客户端 SSE 队列；`requestTimeout` 是 HTTP 层请求超时。

示例：见 §12.3。

### 7.4 Legacy HTTP+SSE（2024-11-05，默认 OFF）

```cpp
struct LegacySseServerOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  std::size_t maxBodyBytes = 1024 * 1024;
  std::size_t maxSessions = 128;
  std::size_t perStreamQueue = 256;
  std::vector<std::string> allowedHosts;
  std::vector<std::string> allowedOrigins;
  TlsOptions tls;
  std::shared_ptr<IAuthenticator> auth;
  std::shared_ptr<IAuditHook> audit;
  std::shared_ptr<ITimeSource> timeSource;
};

struct LegacySseClientOptions {
  std::string baseUrl;
  int connectTimeoutMs = 10000;
  int readTimeoutMs = 30000;
  TlsOptions tls;
};

class LegacySseServerTransport final : public ITransport {
public:
  explicit LegacySseServerTransport(LegacySseServerOptions options);
  ~LegacySseServerTransport() override;

  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void setAuthenticator(std::shared_ptr<IAuthenticator> auth) override;
  void setAuditHook(std::shared_ptr<IAuditHook> audit) override;
  void setSupportedVersions(std::vector<std::string> versions) override;
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override;
  Status sendToSession(std::string_view sessionId,
                       const Json& message) override;
  void close() override;

  int boundPort() const;
};

class LegacySseClientTransport final : public ITransport {
public:
  explicit LegacySseClientTransport(LegacySseClientOptions options);
  ~LegacySseClientTransport() override;

  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void close() override;
};

Result<std::shared_ptr<ITransport>> makeLegacySseServerTransport(
    const LegacySseServerOptions& options);
Result<std::shared_ptr<ITransport>> makeLegacySseClientTransport(
    const LegacySseClientOptions& options);
```

- 只有 `TUGMCP_ENABLE_LEGACY_SSE=ON` 构建可用；默认 OFF。
- 不进入任何单头产物；只能用编译库。
- server 有 `boundPort()`；server/client 都直接暴露构造函数（public）。
- 语义是 2024-11-05：`GET /sse` + `POST /messages`，不是 2026/2025 的 Streamable HTTP。

---

## 8. Executor API

```cpp
enum class Priority { Normal, High, Emergency };

struct ExecutorSubmitResult {
  enum class Kind { Accepted, RejectedQueueFull, RejectedStopped };
  Kind kind = Kind::Accepted;
  explicit operator bool() const noexcept;
};

struct ExecutorStats {
  std::size_t queued = 0;
  std::size_t active = 0;
  std::size_t capacity = 0;   // 0 == unbounded
};

class IExecutor {
public:
  virtual ~IExecutor() = default;

  virtual ExecutorSubmitResult post(
      std::function<void()> task,
      Priority priority = Priority::Normal) noexcept = 0;

  virtual void drain() noexcept = 0;
  virtual void stop() noexcept = 0;
  virtual std::size_t workerCount() const noexcept = 0;

  virtual ExecutorStats stats() const noexcept;
  virtual bool waitIdle(std::chrono::milliseconds timeout) noexcept;
};

std::shared_ptr<IExecutor> makeThreadPoolExecutor(
    std::size_t workers,
    std::size_t maxQueue);
```

| 项 | 语义 |
|---|---|
| `post` | 线程安全、非阻塞、不抛异常；同优先级 FIFO；`High` 先于 `Normal`，`Emergency` 先于 `High` |
| 容量 | 只统计**排队**任务，不含运行中；`maxQueue == 0` 无界 |
| `Emergency` | 绕过容量上限（准入/安全路径）；`drain()`/`stop()` 之后仍返回 `RejectedStopped` |
| 任务异常 | worker 捕获，异常不穿线程边界 |
| `drain` | 终止性：拒绝新任务，等待排队 + 运行任务完成；从自身任务调用时排除调用任务 |
| `stop` | 终止性：拒绝新任务、丢弃排队任务、等待运行任务（自我排除）、join 可 join 的 worker；幂等 |
| `workerCount` | 当前实现的工作线程数 |
| `stats` | 默认返回全 0；线程池实现返回快照 |
| `waitIdle(timeout)` | 非终止性观察等待；默认返回 false（基类无法观察空闲） |
| 析构限制 | 在自身任务中销毁 executor（释放最后 owner）不受支持；在任务中调用 `stop()` 支持 |

`makeThreadPoolExecutor(workers, maxQueue)`：

- `workers == 0` 被钳制为 1，避免立即死锁；
- `maxQueue == 0` 无界；
- 分配失败/容量满返回 `RejectedQueueFull`。

server 默认执行器：

- `ServerOptions::executor` 为 null 且 `useInlineExecutor == false` → `run()` 创建 **4 workers / 1024 队列**内部池；
- `executor` 非 null → 总是优先，`useInlineExecutor` 被忽略；
- `useInlineExecutor == true` 且无 executor → handler 直接在 transport 线程执行（测试/单线程宿主）；
- `run()` 退出时对 executor 调用 `drain()`。

---

## 9. Security API

```cpp
using Headers = std::map<std::string, std::string>;  // 名称已小写

struct AuthContext {
  bool authenticated = false;
  std::string principal;
  std::vector<std::string> roles;
  Json attributes;
};

class IAuthenticator {
public:
  virtual ~IAuthenticator() = default;
  virtual Result<AuthContext> authenticate(const Headers& headers) = 0;
};

class IAuthorizer {
public:
  virtual ~IAuthorizer() = default;
  virtual Status authorize(const AuthContext& auth,
                           std::string_view toolOrResource) = 0;
};

struct ApprovalRequest {
  AuthContext auth;
  std::string tool;
  Json arguments;
  RequestId id;
  std::chrono::steady_clock::time_point deadline{};
  std::shared_ptr<CancellationToken> cancellation;
};

struct ApprovalResult {
  bool approved = false;
  std::string reason;
};

class IApprovalHook {
public:
  virtual ~IApprovalHook() = default;
  virtual std::future<ApprovalResult> approve(const ApprovalRequest&) = 0;
};

class IAuditHook {
public:
  virtual ~IAuditHook() = default;
  virtual void onEvent(const AuthContext& auth, std::string_view event,
                       const Json& detail) = 0;
};
```

### 9.1 调用位置与失败语义

| hook | 调用位置 | 失败结果 |
|---|---|---|
| `IAuthenticator` | HTTP transport，解析 body 前（GET/POST/DELETE）；stdio 无头，通常不启用 | HTTP 401 + JSON-RPC `InternalError` + `data.reason="unauthorized"`（保留可解析的 id） |
| `IAuthorizer` | `tools/call`、`resources/read`、`prompts/get`、2025 `resources/subscribe` | JSON-RPC `InternalError` + `data.reason="forbidden"`；HTTP 403 |
| `IApprovalHook` | 仅在 `tools/call`，授权之后 | 拒绝 → `data.reason="approval_denied"`；超时 → `"approval_timeout"`；HTTP 403 |
| `IAuditHook` | 安全事件观察（tool_call、resource_read、prompt_get、authorization_denied、approval_denied、approval_timeout、auth_failed 等） | **不影响结果**；异常被库收敛为 `InternalError` + `data.reason="audit_failed"`，经 `onError` 上报，请求继续 |

要点：

- `AuthContext::authenticated` 不会被库强制检查；authenticator 返回成功即视为通过。应用自行决定 principal/roles 语义。
- `Headers` 的 key 已小写（例如 `authorization`、`origin`）。
- 认证失败不用 `-32021`；`-32021` 表示缺少客户端声明的能力，由 handler/authorizer 主动返回，`error.data.requiredCapabilities` 应是 ClientCapabilities 对象。
- **client-declared capabilities 不是授权输入**：`RequestContext::clientCapabilities` 来自客户端，未验证；只能用于能力 gating，不能用来决定权限。
- **list 不是逐项授权**：`tools/list`、`resources/list`、`resources/templates/list`、`prompts/list` 是公开枚举，库不调用 `IAuthorizer` 过滤。宿主不得把未授权可见的 descriptor 注册/暴露出来。
- 审批等待受 `ServerOptions::approvalTimeout` 与 `CancellationToken` 约束；`ApprovalRequest::deadline` 是同一个期限。
- 审计是观测，不是鉴权；审计失败不得改变放行/拒绝结果。

---

## 10. Schema 与 Canonical JSON

### 10.1 Schema 接口

```cpp
struct ValidationIssue {
  std::string path;
  std::string keyword;
  std::string message;
};

struct ValidationReport {
  bool ok = false;
  std::vector<ValidationIssue> issues;
};

struct SchemaOptions {
  std::string baseUri;
  bool allowRemoteRefs = false;
  std::vector<std::string> keywords;
};

class ISchemaValidator {
public:
  virtual ~ISchemaValidator() = default;

  virtual Status compile(std::string_view schemaId, const Json& schema,
                         const SchemaOptions& options) = 0;

  virtual Result<ValidationReport> validate(std::string_view schemaId,
                                            const Json& instance) const = 0;
};

using SchemaValidatorPtr = std::shared_ptr<ISchemaValidator>;

SchemaValidatorPtr makeJsonconsSchemaValidator();   // 需 TUGMCP_WITH_JSONCONS=ON
SchemaValidatorPtr makeValijsonSchemaValidator();   // 需 TUGMCP_WITH_VALIJSON=ON
SchemaValidatorPtr makeNullSchemaValidator();       // 仅测试
```

- `compile()` 必须支持“编译一次、并发 `validate()`”；实现应线程安全。
- `baseUri` 解析相对 `$ref`；远程 `$ref` 默认关闭；不能获取远程引用的实现应在编译期拒绝，而不是静默忽略。
- `keywords` 为空表示实现默认关键字集；未支持的关键字不得静默忽略，应 fail closed 或编译期拒绝。
- 可用的编译 target：`tugmcp::schema_jsoncons`（JSON Schema 2020-12）与 `tugmcp::schema_valijson`（draft-07）。默认 `TUGMCP_WITH_JSONCONS=OFF`、`TUGMCP_WITH_VALIJSON=OFF`。
- `NullSchemaValidator` 编译一切、校验一律失败，只用于测试接线。

### 10.2 Canonical JSON 与 `tool_schema_hash`

```cpp
std::string sha256Hex(std::string_view data);
Result<std::string> canonicalize(const Json& value);
Result<std::string> toolSchemaHash(const ToolDescriptor& tool);
```

- `sha256Hex`：小写十六进制 SHA-256，输入是 `data` 的原始字节（通常 UTF-8）。
- `canonicalize` 是 RFC 8785 的**文档化子集**，不是完整 JCS：
  - 对象键按 UTF-8 字节序排序；
  - 数组保持顺序；
  - 字符串使用标准 JSON 转义，非 ASCII 输出 UTF-8 字节；
  - 整数十进制输出，保留 int64/uint64 精度；
  - 浮点用最短往返表示，`-0` 归一为 `"0"`；
  - 拒绝 NaN/Inf 和非法 UTF-8。
  - **不**声称精确复刻 ECMAScript 数字格式；只保证给定输入/工具链稳定。
- `toolSchemaHash` 的定义：

```text
sha256("mcp-tool-v1\n" + canonical({name, inputSchema, outputSchema, annotations}))
```

`description`、`title`、`idempotent` **不参与**哈希。它是审计/缓存/变更检测用的宿主侧哈希，不是协议字段。

---

## 11. 协议、错误映射与能力

### 11.1 2026-07-28 vs 2025-11-25

| 维度 | 2026-07-28 | 2025-11-25 |
|---|---|---|
| 生命周期 | 无状态；`server/discover` | `initialize` + `notifications/initialized` |
| session | 无 | `MCP-Session-Id`；GET 流；DELETE 关闭 |
| 版本协商 | HTTP `MCP-Protocol-Version` + `_meta` 必须一致 | initialize 参数 + header |
| 请求 `_meta` | 必须含 `io.modelcontextprotocol/protocolVersion`（string）与 `io.modelcontextprotocol/clientCapabilities`（object）；`clientInfo` SHOULD | 不要求 |
| HTTP headers | 每个 POST 还要 `Mcp-Method`；`tools/call` 还要 `Mcp-Name` | 无这些要求 |
| 结果元数据 | 结果带 `resultType`；`_meta` 带 `io.modelcontextprotocol/serverInfo`（SHOULD） | 无 |
| list/read 缓存 | 带 `ttlMs`/`cacheScope`（默认 `"private"`、0） | 不输出 |
| 订阅 | `subscriptions/listen` + `notifications/subscriptions/acknowledged` | `resources/subscribe`/`unsubscribe` + GET 流 |
| 订阅标识 | `params._meta["io.modelcontextprotocol/subscriptionId"]`，JSON 类型原样 | `(sessionId, uri)` |
| MRTR | `input_required` + `requestState` + `inputResponses` | 不支持（返回 `input_required_unsupported`） |
| logging | 无 `logging/setLevel`（移除） | `logging/setLevel` + session 级别 |
| server→client 请求 | 不发起该路径；用 MRTR 代替 | 按 client capability，经 GET 流 |
| 已移除方法 | `initialize`、`ping`、`logging/setLevel`、`resources/subscribe`、`resources/unsubscribe`；未知方法也 404 + `-32601` | 正常处理 |

### 11.2 2026 请求检查顺序

HTTP server 对 2026 POST 的固定顺序：

1. Host/Origin；
2. Content-Type / body 上限；
3. 版本解析/支持（不支持 → `-32022`，`data.requested`/`data.supported`）；
4. `_meta` 形状（`-32602`）；
5. header ↔ `_meta` 版本一致性（`-32020`）；
6. `Mcp-Method` / `Mcp-Name`（`-32020`）；
7. 方法存在性（`-32601`）。

2026 **通知**如果没有 id 且 `_meta` 非法，HTTP 返回 400 + 空 body（无法回 JSON-RPC error）。

### 11.3 2026 `_meta` 与客户端能力

- 所有 client→server 带 `params` 的消息都需要 `params._meta` 对象。
- `io.modelcontextprotocol/protocolVersion` 必须与 HTTP `MCP-Protocol-Version` 相同。
- `io.modelcontextprotocol/clientCapabilities` 为空对象表示不支持可选能力。
- `io.modelcontextprotocol/clientInfo` 是 SHOULD，服务端不得强制。
- `progressToken` 仍是普通键（在 `_meta` 内），日志级别键是 `io.modelcontextprotocol/logLevel`。
- `RequestContext::clientCapabilities` 是原样取出的对象；库不校验它与实际行为一致。

### 11.4 `resultType`、缓存与 MRTR

- 2026 成功结果默认补 `resultType:"complete"`；`OperationOutcome` 的 `InputRequired` 补 `resultType:"input_required"`。
- `ToolResult`/`Page`/`ResourceContent` 的 `ttlMs`/`cacheScope` 用于 2026 的 list/read/call 结果；`cacheScope` 空 → `"private"`。
- MRTR payload：
  - `inputRequests` 与 `requestState` 必须同时存在且有效；
  - 2026 才支持；2025 返回 `input_required_unsupported`；
  - 重试由客户端发起新请求，带新的 JSON-RPC id、原样 `requestState`、`inputResponses`；
  - 库只做结构性校验；完整性/签名/轮次由应用编码在 `requestState` 中自行校验。

### 11.5 订阅 wire

2026 `subscriptions/listen` 请求：

```json
{
  "notifications": {
    "toolsListChanged": true,
    "promptsListChanged": true,
    "resourcesListChanged": true
  }
}
```

- 字段是布尔；未知字段可拒绝或省略。
- `SubscriptionFilter::resources` 非空 → `InvalidParams`（2026 没有 per-URI 订阅）。
- 首帧 ack：

```json
{
  "method": "notifications/subscriptions/acknowledged",
  "params": {
    "notifications": { "...": true },
    "_meta": {
      "io.modelcontextprotocol/subscriptionId": <listen request id>
    }
  }
}
```

- 后续该流通知的 `_meta` subscriptionId 必须与 listen id **JSON 类型和值都一致**；不再使用顶层 `subscriptionId`/`requestId`。
- 非流式 transport（stdio）返回 `InternalError` + `data.reason="subscriptions_require_stream"`。
- 2025 对 `subscriptions/listen` 返回 `-32601`。

### 11.6 错误与 HTTP 状态映射

| 场景 | JSON-RPC | HTTP |
|---|---|---|
| 解析/解码失败 | `-32700`/`-32600` | 2026: 400；2025: 400（在版本分支前解码） |
| 参数/方法 dispatch 错误 | `-32600`/`-32602` | 2026: 400；2025: 200 |
| 未知方法 | `-32601` | 2026: 404；2025: 200 |
| header/`_meta` 版本不一致 | `-32020` | 2026: 400；2025: 200 |
| 缺少客户端能力 | `-32021` | 2026: 400（由 handler/authorizer 返回时）；不是认证失败 |
| 不支持版本 | `-32022` | 400（两代）；`data.requested`/`data.supported` |
| 处理超时 | 无/传输错误 | 504 |
| 认证失败 | 库用 `-32603` + `reason:"unauthorized"` | 401（不是 `-32021`） |
| 授权拒绝 | `-32603` + `reason:"forbidden"` | 403 |
| 审批拒绝/超时 | `-32603` + `reason:"approval_denied"`/`"approval_timeout"` | 403 |
| Host/Origin 非法 | 无 JSON-RPC | 403 |
| admission 拒绝 | 无 JSON-RPC | 429 |
| session 超限/流超限 | 无/传输错误 | 429 |
| 工具业务失败 | 无 JSON-RPC error；`ToolResult.isError=true` | 200 |
| 输出 schema 失败 | 无 JSON-RPC error；tool error（`isError=true`） | 200 |
| 2026 GET `/mcp` | 无 | 405 |
| 2025 session 缺失/未知 | 无/`missing_session` | 400 / 404 |

2025 的 JSON-RPC 错误通常仍是 HTTP 200，只有 session/流/认证/限流等传输层问题用非 200。

### 11.7 能力宣告

当前 `capabilities()` 的行为：

| capability | 条件 |
|---|---|
| `tools` | 始终（`{"listChanged": true}`） |
| `resources` | `ServerOptions::resources != nullptr`；2025 额外 `subscribe: true` |
| `prompts` | `ServerOptions::prompts != nullptr` |
| `completions` | 始终 `{}`（即使 provider 为 null） |
| `logging` | 仅 2025 |
| `subscriptions` | 仅 2026 且 transport `supportsStreaming()` |

`server/discover` 还返回 `supportedVersions`；`initialize` 返回协商后的 `protocolVersion`。

---

## 12. 最小可用示例

以下示例使用真实签名。为便于阅读，`main` 中省略了错误打印细节；实际代码应检查每个 `Status`/`Result`。

### 12.1 stdio server + 注册 tool

```cpp
#include <memory>
#include <string>
#include <utility>

#include <tugmcp/server.hpp>
#include <tugmcp/transport/stdio.hpp>

namespace {

using namespace mcp::v1;

class EchoHandler final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&,
                             IProgressSink&) override {
    std::string text;
    if (arguments.contains("text") && arguments.at("text").is_string()) {
      text = arguments.at("text").get<std::string>();
    }

    ToolResult result;
    result.content.push_back(ContentBlock{ContentType::Text, text});
    result.structuredContent = Json{{"text", text}};
    return ToolOutcome(std::move(result));   // OperationOutcome<ToolResult>
  }
};

} // namespace

int main() {
  using namespace mcp::v1;

  ServerOptions options;
  options.name = "demo-server";
  options.version = "1.0.0";
  // 演示用：不接 schema 目标。生产环境应注入 validator 并保持 false。
  options.allowUnvalidatedTools = true;

  McpServer server(options, makeStdioTransport());

  ToolDescriptor descriptor;
  descriptor.name = "echo";
  descriptor.title = "Echo";
  descriptor.description = "Return the provided text unchanged";
  descriptor.inputSchema = Json::parse(
      R"({"type":"object","properties":{"text":{"type":"string"}}})");

  const Status added =
      server.tools().add(descriptor, std::make_shared<EchoHandler>());
  if (!added) {
    return 1;
  }

  const Status status = server.run();   // 阻塞到 stop()/EOF
  return status ? 0 : 1;
}
```

### 12.2 HTTP server

```cpp
#include <iostream>
#include <memory>

#include <tugmcp/server.hpp>
#include <tugmcp/transport/http.hpp>

int main() {
  using namespace mcp::v1;

  StreamableHttpServerOptions http_options;
  http_options.bind = "127.0.0.1";
  http_options.port = 0;   // 系统分配

  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created) {
    std::cerr << created.error().message << "\n";
    return 1;
  }

  auto transport = created.value();
  auto concrete = std::dynamic_pointer_cast<HttpServerTransport>(transport);
  if (!concrete) {
    return 1;
  }

  ServerOptions options;
  options.name = "demo-http";
  options.allowUnvalidatedTools = true;   // 仅演示

  McpServer server(options, transport);
  // 在这里通过 server.tools().add(...) 注册工具。

  const Status started = transport->start();
  if (!started) {
    return 1;
  }
  std::cerr << "listening on http://127.0.0.1:" << concrete->boundPort()
            << "/mcp\n";

  return server.run() ? 0 : 1;
}
```

### 12.3 client connect + callTool

```cpp
#include <chrono>
#include <iostream>

#include <tugmcp/client.hpp>
#include <tugmcp/transport/http.hpp>

int main(int argc, char** argv) {
  using namespace mcp::v1;

  if (argc != 2) {
    return 2;
  }

  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = argv[1];
  transport_options.protocolVersion = "2026-07-28";

  auto transport = makeStreamableHttpClientTransport(transport_options);
  if (!transport) {
    std::cerr << transport.error().message << "\n";
    return 1;
  }

  ClientOptions client_options;
  client_options.protocolVersion = "2026-07-28";

  McpClient client(client_options, transport.value());
  const Status connected = client.connect();
  if (!connected) {
    std::cerr << connected.error.message << "\n";
    client.close();
    return 1;
  }

  auto listed = client.listTools();
  if (!listed) {
    std::cerr << listed.error().message << "\n";
    client.close();
    return 1;
  }

  CallOptions call_options;
  call_options.timeout = std::chrono::seconds(10);
  call_options.progressToken = std::string("progress-1");
  call_options.onProgress = [](const Progress& p) {
    std::cerr << "progress: " << p.progress << "\n";
  };
  call_options.onNotification = [](const Json& note) {
    std::cerr << "notification: " << note.value("method", "") << "\n";
  };

  auto called = client.callTool("echo", Json{{"text", "hello"}},
                                call_options);
  if (!called) {
    std::cerr << called.error().message << "\n";
    client.close();
    return 1;
  }

  std::cout << called.value().dump() << "\n";
  client.close();
  return 0;
}
```

注意：2026 服务端要求 `Mcp-Method` 等 HTTP 头，HTTP transport 会自动添加；`callTool` 的 arguments 默认是对象。

### 12.4 Resource / Prompt / Completion provider

```cpp
#include <memory>
#include <string>
#include <utility>

#include <tugmcp/server.hpp>
#include <tugmcp/transport/stdio.hpp>

namespace {

using namespace mcp::v1;

class DemoResources final : public IResourceProvider {
public:
  Result<Page<ResourceDescriptor>> list(const ListParams&,
                                        RequestContext&) override {
    Page<ResourceDescriptor> page;
    ResourceDescriptor descriptor;
    descriptor.uri = "demo://hello";
    descriptor.name = "hello";
    descriptor.mimeType = "text/plain";
    page.items.push_back(std::move(descriptor));
    return page;
  }

  Result<Page<ResourceTemplate>> templates(const ListParams&,
                                           RequestContext&) override {
    return Page<ResourceTemplate>{};
  }

  Result<ResourceOutcome> read(std::string_view uri,
                               RequestContext&) override {
    if (uri != "demo://hello") {
      return Result<ResourceOutcome>::failure(McpError::make(
          ErrorCode::InvalidParams, "unknown resource"));
    }
    ResourceContent content;
    content.uri = std::string(uri);
    content.mimeType = "text/plain";
    content.text = std::string("hello from resource");
    return ResourceOutcome(std::move(content));
  }
};

class DemoPrompts final : public IPromptProvider {
public:
  Result<Page<PromptDescriptor>> list(const ListParams&,
                                      RequestContext&) override {
    Page<PromptDescriptor> page;
    PromptDescriptor descriptor;
    descriptor.name = "greet";
    descriptor.description = "Say hello";
    descriptor.arguments = Json::array(
        {Json{{"name", "who"}, {"required", true}}});
    page.items.push_back(std::move(descriptor));
    return page;
  }

  Result<PromptOutcome> get(std::string_view name, const Json& args,
                            RequestContext&) override {
    if (name != "greet") {
      return Result<PromptOutcome>::failure(McpError::make(
          ErrorCode::InvalidParams, "unknown prompt"));
    }
    const std::string who = args.value("who", std::string("world"));

    PromptResult result;
    result.description = "Greeting";
    result.messages = Json::array({Json{
        {"role", "user"},
        {"content", Json{{"type", "text"}, {"text", "Hello " + who}}}}});
    return PromptOutcome(std::move(result));
  }
};

class DemoCompletion final : public ICompletionProvider {
public:
  Result<Json> complete(const Json&, const Json& argument,
                        RequestContext&) override {
    const std::string prefix = argument.value("value", std::string{});
    return Json{{"values", Json::array({prefix + "a", prefix + "b"})}};
  }
};

} // namespace

int main() {
  using namespace mcp::v1;

  ServerOptions options;
  options.name = "demo-providers";
  options.allowUnvalidatedTools = true;
  options.resources = std::make_shared<DemoResources>();
  options.prompts = std::make_shared<DemoPrompts>();
  options.completions = std::make_shared<DemoCompletion>();

  McpServer server(options, makeStdioTransport());
  return server.run() ? 0 : 1;
}
```

类型错误或未知 URI/名称由 provider 返回 `McpError`；库会把它们作为 JSON-RPC error 发回。

### 12.5 注入 executor

```cpp
#include <tugmcp/executor.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/transport/stdio.hpp>

int main() {
  using namespace mcp::v1;

  auto executor = makeThreadPoolExecutor(4, 1024);

  ServerOptions options;
  options.executor = executor;
  // options.useInlineExecutor 此时被忽略：非 null executor 优先。

  McpServer server(options, makeStdioTransport());
  // 注册工具...
  return server.run() ? 0 : 1;
}
```

若 executor 同时被宿主其他组件共享，注意 `run()` 退出时会 `drain()` 它（终止性）。

### 12.6 auth / authorizer hook

```cpp
#include <algorithm>
#include <memory>
#include <string>

#include <tugmcp/server.hpp>
#include <tugmcp/transport/http.hpp>

namespace {

using namespace mcp::v1;

class BearerAuth final : public IAuthenticator {
public:
  Result<AuthContext> authenticate(const Headers& headers) override {
    const auto it = headers.find("authorization");
    if (it == headers.end() || it->second != "Bearer demo-token") {
      return Result<AuthContext>::failure(McpError::make(
          ErrorCode::InternalError, "unauthorized",
          Json{{"reason", "unauthorized"}}));
    }

    AuthContext context;
    context.authenticated = true;
    context.principal = "demo";
    context.roles = {"operator"};
    context.attributes = Json{{"tenant", "example"}};
    return context;
  }
};

class ToolAuthorizer final : public IAuthorizer {
public:
  Status authorize(const AuthContext& auth,
                   std::string_view operation) override {
    if (!auth.authenticated) {
      return Status::failure(McpError::make(
          ErrorCode::InternalError, "forbidden",
          Json{{"reason", "forbidden"}}));
    }
    if (operation == "dangerous" &&
        std::find(auth.roles.begin(), auth.roles.end(), "operator") ==
            auth.roles.end()) {
      return Status::failure(McpError::make(
          ErrorCode::InternalError, "forbidden",
          Json{{"reason", "forbidden"}}));
    }
    return Status::success();
  }
};

} // namespace

int main() {
  using namespace mcp::v1;

  StreamableHttpServerOptions http_options;
  http_options.bind = "127.0.0.1";
  http_options.port = 0;

  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created) {
    return 1;
  }

  ServerOptions options;
  options.name = "secure-demo";
  options.allowUnvalidatedTools = true;
  options.auth = std::make_shared<BearerAuth>();
  options.authorizer = std::make_shared<ToolAuthorizer>();

  McpServer server(options, created.value());
  // 注册工具...
  return server.run() ? 0 : 1;
}
```

### 12.7 单头消费

```cpp
// impl.cpp —— 整个程序恰好一个 TU 这样写
#define TUGMCP_IMPLEMENTATION
#include "tugmcp_http.hpp"
```

```cpp
// other.cpp
#include "tugmcp_http.hpp"   // 不再定义 TUGMCP_IMPLEMENTATION
```

单头产物不包含 schema 后端与 legacy SSE；链接依赖（`Threads::Threads`、Windows `ws2_32`、TLS 的 OpenSSL `crypt32`）见 [`docs/amalgamation.md`](./amalgamation.md)。

---

## 13. API 限制与稳定性

1. **Client 返回原始 `Json`。** `listTools`/`callTool`/`readResource`/`getPrompt` 等没有强类型 `ToolOutcome`/`Page<T>` 返回值。字段检查由调用方完成；未知字段应容忍。
2. **每类 provider 最多一个。** 资源、提示词、补全各一个；资源 URI 命名空间和分页由 provider 拥有。当前没有动态 add/remove provider。
3. **2026 没有 per-URI 订阅。** `SubscriptionFilter::resources` 非空直接拒绝；`notifyResourceUpdated` 的 per-URI 行为只服务 2025 session 订阅。
4. **list 不逐项授权。** `IAuthorizer` 不参与 list 过滤；不要注册不该被枚举的 descriptor。
5. **`resultType` 字段不触发 MRTR。** 必须返回 `OperationOutcome` 的 `InputRequired` 分支；仅设置 `ToolResult::resultType` 无效。
6. **stdio 没有版本元数据。** 服务端按 2025-11-25 解释；需要完整 2026 语义请用 HTTP。stdio 也不支持认证、session、订阅流。
7. **`requestClient`/`sendNotification` 依赖 session 流。** 主要用于 2025 HTTP；2026 stateless 没有 session。调用是阻塞的，避免在无限等待路径中使用。
8. **`notifyListChanged` 不自动发送。** `tools().add()` 不会触发通知；宿主注册/变更后显式调用。
9. **`McpClient::unsubscribe` 不发 wire 取消。** 它本地注销并 `cancelPending`；订阅生命周期由关闭流/连接决定。
10. **HTTP server 部分选项尚未接线。** `enable2025Session`、`emergencyPath`、`dedicatedEmergencyListener` 在头文件中标注尚未接线；`TlsOptions::privateKeyPassword`、`serverName`、`minVersion` 也尚未接线。
11. **`TUGMCP_HEADER_ONLY=ON` 是 `FATAL_ERROR`。** split-header INTERFACE 模式未实现。单头发布产物是另一套形态，使用 `TUGMCP_IMPLEMENTATION`，不要混淆。
12. **schema 后端与 legacy SSE 不进入单头。** 单头只有 `tugmcp.hpp`（core+stdio）、`tugmcp_http.hpp`（+HTTP）、`tugmcp_http_tls.hpp`（+TLS）三种 profile。legacy 只能用编译库（且 `TUGMCP_ENABLE_LEGACY_SSE=ON`）。
13. **TLS 不是默认能力。** 需要 `TUGMCP_TLS=ON`、OpenSSL ≥ 3.0；同一进程只允许一套 OpenSSL。TLS 开/关不改变 `transport/http.hpp` 的字段形状。
14. **Canonical JSON 是子集。** 不声称完整 JCS/ECMAScript 数字格式；跨工具链的哈希稳定性按项目文档化子集理解。
15. **库不保证设备/宿主安全。** auth/approval/audit 只是 hooks；设备联锁、单实例、业务授权必须在宿主/硬件层实现。
16. **当前 header 没有设计草案里的 `runAsync`/`addTool`/`removeTool`。** 实际 API 是 `tools()` + 阻塞 `run()`；`ToolRegistry` 也没有 remove。`ServerOptions` 没有 `enableLegacySse`；legacy 需要显式构造 `LegacySse*Transport`。不要按旧设计文档调用不存在的方法。

---

## 14. 相关链接汇总

- 构建/测试/开关：[`docs/building.md`](./building.md)
- 宿主集成：[`docs/integration.md`](./integration.md)
- 宿主适配边界：[`designs/host-adapter.md`](../designs/host-adapter.md)
- 主设计：[`designs/mcp-library-design.md`](../designs/mcp-library-design.md)
- 协议决策（D17–D20）：[`designs/protocol-decisions.md`](../designs/protocol-decisions.md)
- 单头产物：[`docs/amalgamation.md`](./amalgamation.md)
- 测试分层：[`designs/testing.md`](../designs/testing.md)
- 一致性报告：[`docs/conformance-report.md`](./conformance-report.md)
- 公共头目录：[`include/tugmcp/`](../include/tugmcp/)
