#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tugmcp/schema/jsoncons.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>

#include "../helpers/fake_transport.hpp"

using mcp::v1::ContentBlock;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::makeJsonconsSchemaValidator;
using mcp::v1::McpServer;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::SchemaOptions;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolRegistry;
using mcp::v1::ToolResult;
using mcp::v1::ITransport;
using mcp::v1::McpError;

namespace {

Json textObjectSchema() {
  return Json::parse(R"({
    "type": "object",
    "properties": {"text": {"type": "string"}},
    "required": ["text"]
  })");
}

class TextHandler final : public IToolHandler {
public:
  Result<ToolResult> call(const Json& arguments, RequestContext&) override {
    ToolResult result;
    result.content.push_back(
        ContentBlock{"text", arguments.value("text", std::string{})});
    return result;
  }
};

ToolDescriptor textTool() {
  ToolDescriptor tool;
  tool.name = "echo";
  tool.inputSchema = textObjectSchema();
  return tool;
}

// Delivers one deterministic request and then closes; mirrors the integration
// test helper so no sleeps or polling are needed.
class ScriptedTransport final : public ITransport {
public:
  ScriptedTransport(std::shared_ptr<mcp::v1::test::FakeTransport> inner,
                    Json request)
      : inner_(std::move(inner)), request_(std::move(request)) {}

  Status start() override {
    Status status = inner_->start();
    if (!status) {
      return status;
    }
    inner_->deliver(request_);
    inner_->emitClose();
    return status;
  }

  Status send(const Json& message) override {
    sent_.push_back(message);
    return inner_->send(message);
  }

  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }
  void setOnError(std::function<void(McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }
  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
  }
  void close() override { inner_->close(); }

  const std::vector<Json>& sent() const noexcept { return sent_; }

private:
  std::shared_ptr<mcp::v1::test::FakeTransport> inner_;
  Json request_;
  std::vector<Json> sent_;
};

bool referencesText(const Json& issue) {
  return issue.value("path", std::string{}).find("text") != std::string::npos ||
         issue.value("message", std::string{}).find("text") !=
             std::string::npos;
}

Json objectSchema() {
  return Json::parse(R"({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "type": "object",
    "required": ["name"],
    "properties": {
      "name": {"type": "string"},
      "age": {"type": "integer"}
    },
    "additionalProperties": false
  })");
}

} // namespace

TEST_CASE("jsoncons validator accepts a conforming instance") {
  auto validator = makeJsonconsSchemaValidator();
  REQUIRE(validator != nullptr);
  REQUIRE(validator->compile("echo", objectSchema(), SchemaOptions{}).ok);

  auto report = validator->validate(
      "echo", Json{{"name", "tug"}, {"age", 3}});
  REQUIRE(report.hasValue());
  CHECK(report.value().ok);
  CHECK(report.value().issues.empty());
}

TEST_CASE("jsoncons validator reports required violations") {
  auto validator = makeJsonconsSchemaValidator();
  REQUIRE(validator->compile("echo", objectSchema(), SchemaOptions{}).ok);

  auto report = validator->validate("echo", Json{{"age", 3}});
  REQUIRE(report.hasValue());
  CHECK_FALSE(report.value().ok);
  REQUIRE_FALSE(report.value().issues.empty());
  CHECK(report.value().issues.front().keyword == "required");
}

TEST_CASE("jsoncons validator reports type violations") {
  auto validator = makeJsonconsSchemaValidator();
  REQUIRE(validator->compile("echo", objectSchema(), SchemaOptions{}).ok);

  auto report = validator->validate("echo", Json{{"name", 7}});
  REQUIRE(report.hasValue());
  CHECK_FALSE(report.value().ok);
  REQUIRE_FALSE(report.value().issues.empty());
  CHECK(report.value().issues.front().keyword == "type");
}

TEST_CASE("jsoncons validator rejects remote refs by default") {
  auto validator = makeJsonconsSchemaValidator();
  const Json schema =
      Json::parse(R"({"$ref":"https://example.com/schema.json"})");
  auto status = validator->compile("remote", schema, SchemaOptions{});
  CHECK_FALSE(status.ok);
}

TEST_CASE("jsoncons validator reports uncompiled schemas") {
  auto validator = makeJsonconsSchemaValidator();
  auto report = validator->validate("missing", Json::object());
  REQUIRE_FALSE(report.hasValue());
  CHECK(report.error().data.at("reason") == "schema_not_compiled");
}

TEST_CASE("jsoncons validator integrates with ToolRegistry") {
  ToolRegistry registry;
  registry.setValidationPolicy(makeJsonconsSchemaValidator(), false);
  REQUIRE(registry.add(textTool(), std::make_shared<TextHandler>()).ok);

  Status status = registry.validateInput("echo", Json::object());
  REQUIRE_FALSE(status.ok);
  REQUIRE(status.error.data.is_object());
  const Json& issues = status.error.data.at("issues");
  REQUIRE(issues.is_array());
  REQUIRE_FALSE(issues.empty());
  bool mentions_text = false;
  for (const auto& issue : issues) {
    mentions_text = mentions_text || referencesText(issue);
  }
  CHECK(mentions_text);
}

TEST_CASE("jsoncons validator rejects an invalid schema at registration") {
  ToolRegistry registry;
  registry.setValidationPolicy(makeJsonconsSchemaValidator(), false);

  ToolDescriptor tool;
  tool.name = "invalid";
  tool.inputSchema = Json{{"type", 5}};
  Status added = registry.add(tool, std::make_shared<TextHandler>());
  REQUIRE_FALSE(added.ok);
  CHECK(static_cast<int>(added.error.code) ==
        static_cast<int>(mcp::v1::ErrorCode::InvalidParams));
}

TEST_CASE("jsoncons validator rejects missing arguments end to end") {
  auto inner = std::make_shared<mcp::v1::test::FakeTransport>();
  auto transport = std::make_shared<ScriptedTransport>(
      inner, Json{{"jsonrpc", "2.0"},
                  {"id", 1},
                  {"method", "tools/call"},
                  {"params",
                   Json{{"name", "echo"}, {"arguments", Json::object()}}}});

  ServerOptions options;
  options.schemaValidator = makeJsonconsSchemaValidator();
  McpServer server(options, transport);
  REQUIRE(server.tools().add(textTool(), std::make_shared<TextHandler>()).ok);

  REQUIRE(server.run().ok);
  REQUIRE(transport->sent().size() == 1);
  const Json& response = transport->sent().front();
  REQUIRE(response.contains("error"));
  CHECK(response.at("error").at("code") == -32602);
  REQUIRE(response.at("error").at("data").at("issues").is_array());
  CHECK_FALSE(response.at("error").at("data").at("issues").empty());
}

