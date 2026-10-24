#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>
#include <tugmcp/tool.hpp>

#include "../../src/core/dispatcher.hpp"

using mcp::v1::ContentBlock;
using mcp::v1::decode;
using mcp::v1::Dispatcher;
using mcp::v1::DispatchConfig;
using mcp::v1::encode;
using mcp::v1::ErrorCode;
using mcp::v1::IProgressSink;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::McpError;
using mcp::v1::Message;
using mcp::v1::MessageKind;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolRegistry;
using mcp::v1::ToolResult;

namespace {

using CallFn =
    std::function<Result<ToolOutcome>(const Json&, RequestContext&)>;

class FunctionHandler final : public IToolHandler {
public:
  explicit FunctionHandler(CallFn fn) : fn_(std::move(fn)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink&) override {
    return fn_(arguments, context);
  }

private:
  CallFn fn_;
};

Dispatcher makeDispatcher(ToolRegistry& registry, DispatchConfig config = {}) {
  return Dispatcher(registry, nullptr, nullptr, std::move(config));
}

Message request(std::string method, Json params = nullptr,
                std::int64_t id = 1) {
  Message message;
  message.kind = MessageKind::Request;
  message.method = std::move(method);
  message.params = std::move(params);
  message.id.value = id;
  return message;
}

Message notification(std::string method, Json params = nullptr) {
  Message message;
  message.kind = MessageKind::Notification;
  message.method = std::move(method);
  message.params = std::move(params);
  return message;
}

void addTool(ToolRegistry& registry, std::string name, CallFn fn,
             bool idempotent = false) {
  ToolDescriptor descriptor;
  descriptor.name = std::move(name);
  descriptor.title = descriptor.name + " title";
  descriptor.description = descriptor.name + " description";
  descriptor.inputSchema = Json{{"type", "object"}};
  descriptor.idempotent = idempotent;
  REQUIRE(registry
              .add(std::move(descriptor),
                   std::make_shared<FunctionHandler>(std::move(fn)))
              .ok);
}

ToolOutcome textResult(std::string text, bool is_error = false) {
  ToolResult result;
  result.isError = is_error;
  result.content.push_back(ContentBlock{"text", std::move(text)});
  return ToolOutcome(std::move(result));
}

} // namespace

TEST_CASE("dispatcher handles initialize") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{"test-server", "9.9.9"});

  auto response = dispatcher.dispatch(request("initialize"));
  REQUIRE(response.has_value());
  CHECK(response->kind == MessageKind::Response);

  const Json& result = response->result;
  CHECK(result.at("protocolVersion") == "2025-11-25");
  CHECK(result.at("capabilities").at("tools").is_object());
  CHECK(result.at("serverInfo").at("name") == "test-server");
  CHECK(result.at("serverInfo").at("version") == "9.9.9");
}

TEST_CASE("dispatcher handles server/discover") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{"discover-server", "1.2.3"});

  auto response = dispatcher.dispatch(request("server/discover"));
  REQUIRE(response.has_value());

  const Json& result = response->result;
  REQUIRE(result.at("supportedVersions").is_array());
  CHECK(result.at("supportedVersions").size() == 2);
  CHECK(result.at("supportedVersions").at(0) == "2026-07-28");
  CHECK(result.at("supportedVersions").at(1) == "2025-11-25");
  CHECK(result.at("serverInfo").at("name") == "discover-server");
}

TEST_CASE("dispatcher handles ping") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{});

  auto response = dispatcher.dispatch(request("ping"));
  REQUIRE(response.has_value());
  CHECK(response->result == Json::object());
}

TEST_CASE("dispatcher lists tools sorted by name") {
  ToolRegistry registry;
  addTool(registry, "zeta", [](const Json&, RequestContext&) {
    return Result<ToolOutcome>(textResult("zeta"));
  });
  addTool(registry, "alpha", [](const Json&, RequestContext&) {
    return Result<ToolOutcome>(textResult("alpha"));
  }, true);

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(request("tools/list"));
  REQUIRE(response.has_value());

  const Json& tools = response->result.at("tools");
  REQUIRE(tools.is_array());
  REQUIRE(tools.size() == 2);
  CHECK(tools.at(0).at("name") == "alpha");
  CHECK(tools.at(1).at("name") == "zeta");
  CHECK(tools.at(0).at("idempotent") == true);
  CHECK_FALSE(tools.at(1).contains("idempotent"));
  CHECK(tools.at(0).at("inputSchema").is_object());
}

TEST_CASE("dispatcher calls a tool successfully") {
  ToolRegistry registry;
  Json seen_arguments = nullptr;
  addTool(registry, "echo",
          [&seen_arguments](const Json& arguments, RequestContext& context) {
            seen_arguments = arguments;
            CHECK(context.method == "tools/call");
            ToolResult result;
            result.content.push_back(ContentBlock{
                "text", arguments.at("text").get<std::string>()});
            result.structuredContent = Json{{"text", arguments.at("text")}};
            return Result<ToolOutcome>(ToolOutcome(std::move(result)));
          });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(
      request("tools/call", Json{{"name", "echo"},
                                 {"arguments", Json{{"text", "hello"}}}}));
  REQUIRE(response.has_value());
  REQUIRE_FALSE(response->error.has_value());

  CHECK(seen_arguments == Json{{"text", "hello"}});
  CHECK(response->result.at("content").is_array());
  REQUIRE(response->result.at("content").size() == 1);
  CHECK(response->result.at("content").at(0).at("type") == "text");
  CHECK(response->result.at("content").at(0).at("text") == "hello");
  CHECK(response->result.at("structuredContent").at("text") == "hello");
}

TEST_CASE("dispatcher defaults missing arguments to an empty object") {
  ToolRegistry registry;
  bool called = false;
  Json seen = nullptr;
  addTool(registry, "noargs",
          [&called, &seen](const Json& arguments, RequestContext&) {
            called = true;
            seen = arguments;
            return Result<ToolOutcome>(textResult("ok"));
          });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response =
      dispatcher.dispatch(request("tools/call", Json{{"name", "noargs"}}));
  REQUIRE(response.has_value());
  CHECK(called);
  CHECK(seen.is_object());
  CHECK(seen.empty());
}

TEST_CASE("dispatcher reports unknown methods with MethodNotFound") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{});

  auto response = dispatcher.dispatch(request("unknown/method"));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::MethodNotFound));
  CHECK(response->error->message.find("unknown/method") != std::string::npos);
}

TEST_CASE("dispatcher reports unknown tools with InvalidParams") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{});

  auto response = dispatcher.dispatch(
      request("tools/call", Json{{"name", "missing"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(response->error->message == "unknown tool: missing");
}

TEST_CASE("dispatcher rejects non-object tool arguments") {
  ToolRegistry registry;
  addTool(registry, "echo", [](const Json&, RequestContext&) {
    return Result<ToolOutcome>(textResult("unused"));
  });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(
      request("tools/call",
              Json{{"name", "echo"}, {"arguments", Json::array({1, 2})}}));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("dispatcher rejects non-object tools/call params") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{});

  auto response =
      dispatcher.dispatch(request("tools/call", Json::array({1, 2})));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("notifications execute without producing a response") {
  ToolRegistry registry;
  int calls = 0;
  addTool(registry, "echo",
          [&calls](const Json&, RequestContext&) {
            ++calls;
            return Result<ToolOutcome>(textResult("notified"));
          });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(notification(
      "tools/call",
      Json{{"name", "echo"}, {"arguments", Json::object()}}));
  CHECK_FALSE(response.has_value());
  CHECK(calls == 1);
}

TEST_CASE("error responses have the required JSON-RPC shape") {
  ToolRegistry registry;
  auto dispatcher = makeDispatcher(registry, DispatchConfig{});

  auto response = dispatcher.dispatch(request("no/such/method", nullptr, 42));
  REQUIRE(response.has_value());

  auto encoded = encode(*response);
  REQUIRE(encoded.hasValue());
  const Json wire = Json::parse(encoded.value());

  CHECK(wire.at("jsonrpc") == "2.0");
  CHECK(wire.at("id") == 42);
  CHECK(wire.at("error").is_object());
  CHECK(wire.at("error").at("code") == -32601);
  CHECK(wire.at("error").at("message").is_string());
}

TEST_CASE("handler failure is returned as an error response") {
  ToolRegistry registry;
  addTool(registry, "fails", [](const Json&, RequestContext&) {
    return Result<ToolOutcome>::failure(
        McpError::make(ErrorCode::InvalidParams, "handler says no",
                       Json{{"detail", "bad"}}));
  });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(
      request("tools/call", Json{{"name", "fails"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(response->error->message == "handler says no");
  CHECK(response->error->data == Json{{"detail", "bad"}});
}

TEST_CASE("handler exceptions are contained as InternalError") {
  ToolRegistry registry;
  addTool(registry, "throws", [](const Json&, RequestContext&) -> Result<ToolOutcome> {
    throw std::runtime_error("boom");
  });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(
      request("tools/call", Json{{"name", "throws"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InternalError));
}

TEST_CASE("tool isError results stay in the result envelope") {
  ToolRegistry registry;
  addTool(registry, "soft-error", [](const Json&, RequestContext&) {
    return Result<ToolOutcome>(textResult("tool failed", true));
  });

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(
      request("tools/call",
              Json{{"name", "soft-error"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  CHECK_FALSE(response->error.has_value());
  CHECK(response->result.at("isError") == true);
  CHECK(response->result.at("content").at(0).at("text") == "tool failed");
}

TEST_CASE("decoded request can be dispatched end-to-end") {
  ToolRegistry registry;
  addTool(registry, "echo", [](const Json& arguments, RequestContext&) {
    return Result<ToolOutcome>(
        textResult(arguments.at("text").get<std::string>()));
  });

  auto decoded = decode(
      R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"wire"}}})");
  REQUIRE(decoded.hasValue());

  auto dispatcher = makeDispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(decoded.value());
  REQUIRE(response.has_value());

  auto encoded = encode(*response);
  REQUIRE(encoded.hasValue());
  const Json wire = Json::parse(encoded.value());
  CHECK(wire.at("id") == 7);
  CHECK_FALSE(wire.at("result").contains("structuredContent"));
  CHECK(wire.at("result").at("content").at(0).at("text") == "wire");
}
