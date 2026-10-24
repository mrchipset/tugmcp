#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>
#include <tugmcp/schema/null.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>

#include "../../src/core/dispatcher.hpp"

using mcp::v1::ContentBlock;
using mcp::v1::Dispatcher;
using mcp::v1::DispatchConfig;
using mcp::v1::ErrorCode;
using mcp::v1::ISchemaValidator;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::makeNullSchemaValidator;
using mcp::v1::McpServer;
using mcp::v1::Message;
using mcp::v1::MessageKind;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::SchemaOptions;
using mcp::v1::SchemaValidatorPtr;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolRegistry;
using mcp::v1::ToolResult;
using mcp::v1::ValidationIssue;
using mcp::v1::ValidationReport;

namespace {

class ConstantHandler final : public IToolHandler {
public:
  Result<ToolResult> call(const Json&, RequestContext&) override {
    ToolResult result;
    result.content.push_back(ContentBlock{"text", "ok"});
    result.structuredContent = Json{{"value", 42}};
    return result;
  }
};

class PlainHandler final : public IToolHandler {
public:
  Result<ToolResult> call(const Json&, RequestContext&) override {
    ToolResult result;
    result.content.push_back(ContentBlock{"text", "ok"});
    return result;
  }
};

class FakeValidator final : public ISchemaValidator {
public:
  bool compileOk = true;
  bool inputOk = true;
  bool outputOk = true;
  bool unavailable = false;

  Status compile(std::string_view, const Json&, const SchemaOptions&) override {
    if (!compileOk) {
      return Status::failure(
          mcp::v1::McpError::make(ErrorCode::InvalidParams, "bad schema"));
    }
    return Status::success();
  }

  Result<ValidationReport> validate(std::string_view schemaId,
                                    const Json&) const override {
    if (unavailable) {
      return Result<ValidationReport>::failure(mcp::v1::McpError::make(
          ErrorCode::InternalError, "no validator",
          Json{{"reason", "schema_validator_unavailable"}}));
    }
    const bool output =
        std::string(schemaId).find("#output") != std::string::npos;
    ValidationReport report;
    report.ok = output ? outputOk : inputOk;
    if (!report.ok) {
      report.issues.push_back(
          ValidationIssue{"/value", "type", "expected integer"});
    }
    return report;
  }
};

ToolDescriptor descriptor(const std::string& name) {
  ToolDescriptor value;
  value.name = name;
  value.description = name;
  value.inputSchema = Json{{"type", "object"}};
  return value;
}

Message request(std::string method, Json params) {
  Message message;
  message.kind = MessageKind::Request;
  message.method = std::move(method);
  message.params = std::move(params);
  message.id.value = std::int64_t{1};
  return message;
}

} // namespace

TEST_CASE("NullSchemaValidator compiles but fails validation closed") {
  auto validator = makeNullSchemaValidator();
  REQUIRE(validator != nullptr);

  Status compiled =
      validator->compile("tool", Json{{"type", "object"}}, SchemaOptions{});
  REQUIRE(compiled.ok);

  auto report = validator->validate("tool", Json{{"value", 1}});
  REQUIRE_FALSE(report.hasValue());
  CHECK(static_cast<int>(report.error().code) ==
        static_cast<int>(ErrorCode::InternalError));
  REQUIRE(report.error().data.is_object());
  CHECK(report.error().data.at("reason") == "schema_validator_unavailable");
}

TEST_CASE("registry fails closed when a schema has no validator") {
  ToolRegistry registry;
  registry.setValidationPolicy(nullptr, false);

  Status added =
      registry.add(descriptor("unvalidated"), std::make_shared<ConstantHandler>());
  REQUIRE_FALSE(added.ok);
  CHECK(static_cast<int>(added.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(added.error.message.find("unvalidated") != std::string::npos);
  CHECK(registry.size() == 0);
}

TEST_CASE("null inputSchema is rejected at registration") {
  ToolRegistry registry;
  ToolDescriptor tool = descriptor("null-schema");
  tool.inputSchema = nullptr;

  Status added = registry.add(tool, std::make_shared<ConstantHandler>());
  REQUIRE_FALSE(added.ok);
  CHECK(static_cast<int>(added.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(added.error.message ==
        "inputSchema must be an object; omit the field to declare no schema");
}

TEST_CASE("empty inputSchema declares no schema and skips validation") {
  ToolRegistry registry;
  registry.setValidationPolicy(nullptr, false);
  ToolDescriptor tool = descriptor("empty-schema");
  tool.inputSchema = Json::object();

  REQUIRE(registry.add(tool, std::make_shared<ConstantHandler>()).ok);
  CHECK(registry.validateInput("empty-schema", Json::object()).ok);
}

TEST_CASE("tool names containing '#' are rejected") {
  ToolRegistry registry;
  Status added =
      registry.add(descriptor("bad#name"), std::make_shared<ConstantHandler>());
  REQUIRE_FALSE(added.ok);
  CHECK(static_cast<int>(added.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(added.error.message == "tool name must not contain '#'");
}

TEST_CASE("allowUnvalidatedTools permits schemas without a validator") {
  ToolRegistry registry;
  registry.setValidationPolicy(nullptr, true);

  REQUIRE(registry
              .add(descriptor("loose"), std::make_shared<ConstantHandler>())
              .ok);
  CHECK(registry.validateInput("loose", Json{{"anything", true}}).ok);
  CHECK(registry.validateOutput("loose", Json{{"anything", true}}).ok);
}

TEST_CASE("registry rejects schemas the validator cannot compile") {
  auto fake = std::make_shared<FakeValidator>();
  fake->compileOk = false;

  ToolRegistry registry;
  registry.setValidationPolicy(fake, false);

  Status added =
      registry.add(descriptor("broken"), std::make_shared<ConstantHandler>());
  REQUIRE_FALSE(added.ok);
  CHECK(static_cast<int>(added.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(added.error.message.find("broken") != std::string::npos);
}

TEST_CASE("registry surfaces validation issues into InvalidParams") {
  auto fake = std::make_shared<FakeValidator>();
  fake->inputOk = false;

  ToolRegistry registry;
  registry.setValidationPolicy(fake, false);
  REQUIRE(registry
              .add(descriptor("strict"), std::make_shared<ConstantHandler>())
              .ok);

  Status status = registry.validateInput("strict", Json{{"value", "text"}});
  REQUIRE_FALSE(status.ok);
  CHECK(static_cast<int>(status.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  REQUIRE(status.error.data.is_object());
  REQUIRE(status.error.data.at("issues").is_array());
  CHECK(status.error.data.at("issues").at(0).at("keyword") == "type");
}

TEST_CASE("ServerOptions default fails closed for schema tools") {
  McpServer server(ServerOptions{}, nullptr);
  Status added =
      server.tools().add(descriptor("server-tool"),
                         std::make_shared<ConstantHandler>());
  CHECK_FALSE(added.ok);
  CHECK(added.error.message.find("server-tool") != std::string::npos);
}

TEST_CASE("ServerOptions allowUnvalidatedTools is wired to the registry") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  McpServer server(options, nullptr);

  CHECK(server
            .tools()
            .add(descriptor("server-tool"), std::make_shared<ConstantHandler>())
            .ok);
}

TEST_CASE("ServerOptions schemaValidator is wired to the registry") {
  auto fake = std::make_shared<FakeValidator>();
  fake->compileOk = false;

  ServerOptions options;
  options.schemaValidator = fake;
  McpServer server(options, nullptr);

  CHECK_FALSE(server
                  .tools()
                  .add(descriptor("server-tool"),
                       std::make_shared<ConstantHandler>())
                  .ok);
}

TEST_CASE("dispatcher reports input validation failure as InvalidParams") {
  auto fake = std::make_shared<FakeValidator>();
  fake->inputOk = false;

  ToolRegistry registry;
  registry.setValidationPolicy(fake, false);
  REQUIRE(registry
              .add(descriptor("strict"), std::make_shared<ConstantHandler>())
              .ok);

  Dispatcher dispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(request(
      "tools/call",
      Json{{"name", "strict"}, {"arguments", Json{{"value", "text"}}}}));
  REQUIRE(response.has_value());
  REQUIRE(response->error.has_value());
  CHECK(static_cast<int>(response->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  REQUIRE(response->error->data.is_object());
  CHECK(response->error->data.at("issues").is_array());
}

TEST_CASE("output validation failure becomes a tool error") {
  auto fake = std::make_shared<FakeValidator>();
  fake->outputOk = false;

  ToolRegistry registry;
  registry.setValidationPolicy(fake, false);
  ToolDescriptor tool = descriptor("out");
  tool.outputSchema = Json{{"type", "object"}};
  REQUIRE(registry.add(tool, std::make_shared<ConstantHandler>()).ok);

  Dispatcher dispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(request(
      "tools/call", Json{{"name", "out"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  CHECK_FALSE(response->error.has_value());
  CHECK(response->result.at("isError") == true);
  REQUIRE(response->result.at("content").size() == 2);
  const std::string note =
      response->result.at("content").at(1).at("text").get<std::string>();
  CHECK(note.find("output validation failed") != std::string::npos);
  CHECK(note.find("/value") != std::string::npos);
  CHECK(note.find("type") != std::string::npos);
}

TEST_CASE("declared output schema requires structuredContent") {
  ToolRegistry registry;
  registry.setValidationPolicy(nullptr, true);
  ToolDescriptor tool = descriptor("plain");
  tool.outputSchema = Json{{"type", "object"}};
  REQUIRE(registry.add(tool, std::make_shared<PlainHandler>()).ok);

  Dispatcher dispatcher(registry, DispatchConfig{});
  auto response = dispatcher.dispatch(request(
      "tools/call", Json{{"name", "plain"}, {"arguments", Json::object()}}));
  REQUIRE(response.has_value());
  CHECK_FALSE(response->error.has_value());
  CHECK(response->result.at("isError") == true);
  REQUIRE(response->result.at("content").size() == 2);
  CHECK(response->result.at("content").at(1).at("text") ==
        "output schema is declared but structuredContent is missing");
}
