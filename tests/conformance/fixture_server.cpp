// External conformance fixture server (Streamable HTTP only).

#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include <chrono>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace mcp::v1;

constexpr const char* kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8"
    "z8BQDwAFgwJ/lZ0nSAAAAABJRU5ErkJggg==";
constexpr const char* kWav =
    "UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQAAAAA=";

ToolResult text_result(std::string text, bool is_error = false) {
  ToolResult result;
  result.isError = is_error;
  result.content.push_back(ContentBlock{ContentType::Text, std::move(text)});
  return result;
}

Json content_block(const char* type, Json value) {
  Json block = std::move(value);
  block["type"] = type;
  return block;
}

Json elicitation_request(std::string message, Json properties,
                         Json required) {
  return Json{{"method", "elicitation/create"},
              {"params",
               Json{{"message", std::move(message)},
                    {"requestedSchema",
                     Json{{"type", "object"},
                          {"properties", std::move(properties)},
                          {"required", std::move(required)}}}}}};
}

bool has_input(const RequestContext& context, const char* key) {
  return context.inputResponses.is_object() &&
         context.inputResponses.contains(key);
}

Json input_content(const RequestContext& context, const char* key) {
  if (!has_input(context, key)) {
    return nullptr;
  }
  const Json& value = context.inputResponses.at(key);
  return value.is_object() && value.contains("content") ? value.at("content")
                                                         : value;
}

class FixtureResources final : public IResourceProvider {
public:
  Result<Page<ResourceDescriptor>> list(const ListParams&,
                                        RequestContext&) override {
    Page<ResourceDescriptor> page;
    ResourceDescriptor text;
    text.uri = "test://static-text";
    text.name = "static-text";
    text.description = "Static text resource";
    text.mimeType = "text/plain";
    page.items.push_back(text);

    ResourceDescriptor binary;
    binary.uri = "test://static-binary";
    binary.name = "static-binary";
    binary.description = "Static binary resource";
    binary.mimeType = "image/png";
    page.items.push_back(binary);
    return page;
  }

  Result<Page<ResourceTemplate>> templates(const ListParams&,
                                           RequestContext&) override {
    Page<ResourceTemplate> page;
    ResourceTemplate value;
    value.uriTemplate = "test://template/{id}/data";
    value.name = "template-data";
    value.description = "Template data resource";
    value.mimeType = "application/json";
    page.items.push_back(value);
    return page;
  }

  Result<ResourceOutcome> read(std::string_view uri,
                               RequestContext&) override {
    ResourceContent content;
    content.uri = std::string(uri);
    if (uri == "test://static-text") {
      content.mimeType = "text/plain";
      content.text = "This is the content of the static text resource.";
      return ResourceOutcome(std::move(content));
    }
    if (uri == "test://static-binary") {
      content.mimeType = "image/png";
      content.blob = kPng;
      return ResourceOutcome(std::move(content));
    }
    if (uri.rfind("test://template/", 0) == 0 &&
        uri.find("/data") != std::string_view::npos) {
      content.mimeType = "application/json";
      content.text =
          R"({"id":"123","templateTest":true,"data":"Data for ID: 123"})";
      return ResourceOutcome(std::move(content));
    }
    return Result<ResourceOutcome>::failure(McpError::make(
        ErrorCode::InvalidParams, "Resource not found",
        Json{{"uri", std::string(uri)}}));
  }
};

class FixturePrompts final : public IPromptProvider {
public:
  Result<Page<PromptDescriptor>> list(const ListParams&,
                                      RequestContext&) override {
    Page<PromptDescriptor> page;
    auto add = [&](const char* name, const char* description,
                   Json arguments) {
      PromptDescriptor value;
      value.name = name;
      value.description = description;
      value.arguments = std::move(arguments);
      page.items.push_back(std::move(value));
    };
    add("test_simple_prompt", "Simple prompt", nullptr);
    add("test_prompt_with_arguments", "Prompt with arguments",
        Json::array({Json{{"name", "arg1"}, {"required", true}},
                     Json{{"name", "arg2"}, {"required", true}}}));
    add("test_prompt_with_embedded_resource",
        "Prompt with embedded resource",
        Json::array({Json{{"name", "resourceUri"}, {"required", true}}}));
    add("test_prompt_with_image", "Prompt with image", nullptr);
    add("test_input_required_result_prompt", "Prompt requiring input",
        nullptr);
    return page;
  }

  Result<PromptOutcome> get(std::string_view name, const Json& args,
                            RequestContext& context) override {
    PromptResult prompt;
    auto message = [](Json content) {
      return Json{{"role", "user"}, {"content", std::move(content)}};
    };

    if (name == "test_simple_prompt") {
      prompt.messages = Json::array(
          {message(Json{{"type", "text"},
                        {"text", "This is a simple prompt for testing."}})});
      return PromptOutcome(std::move(prompt));
    }
    if (name == "test_prompt_with_arguments") {
      const std::string arg1 = args.value("arg1", std::string{});
      const std::string arg2 = args.value("arg2", std::string{});
      prompt.messages = Json::array(
          {message(Json{{"type", "text"},
                        {"text", "Prompt with arguments: arg1='" + arg1 +
                                     "', arg2='" + arg2 + "'"}})});
      return PromptOutcome(std::move(prompt));
    }
    if (name == "test_prompt_with_embedded_resource") {
      const std::string uri = args.value("resourceUri", std::string{});
      prompt.messages = Json::array(
          {message(Json{{"type", "resource"},
                        {"resource",
                         Json{{"uri", uri},
                              {"mimeType", "text/plain"},
                              {"text",
                               "Embedded resource content for testing."}}}}),
           message(Json{{"type", "text"},
                         {"text",
                          "Please process the embedded resource above."}})});
      return PromptOutcome(std::move(prompt));
    }
    if (name == "test_prompt_with_image") {
      prompt.messages = Json::array(
          {message(Json{{"type", "image"},
                        {"data", kPng},
                        {"mimeType", "image/png"}}),
           message(Json{{"type", "text"},
                         {"text", "Please analyze the image above."}})});
      return PromptOutcome(std::move(prompt));
    }
    if (name == "test_input_required_result_prompt") {
      if (has_input(context, "user_context")) {
        prompt.messages = Json::array(
            {message(Json{{"type", "text"},
                          {"text",
                           "Prompt context: " +
                               input_content(context, "user_context")
                                   .value("context", std::string{})}})});
        return PromptOutcome(std::move(prompt));
      }
      InputRequired input;
      input.inputRequests =
          Json{{"user_context",
                elicitation_request(
                    "What context should the prompt use?",
                    Json{{"context", Json{{"type", "string"}}}},
                    Json::array({"context"}))}};
      input.requestState = "prompt-state";
      return PromptOutcome(std::move(input));
    }
    return Result<PromptOutcome>::failure(
        McpError::make(ErrorCode::InvalidParams, "unknown prompt"));
  }
};

class FixtureCompletion final : public ICompletionProvider {
public:
  Result<Json> complete(const Json&, const Json& argument,
                        RequestContext&) override {
    const std::string value = argument.value("value", std::string{});
    Json values = Json::array();
    if (value.empty() || std::string("paris").rfind(value, 0) == 0) {
      values.push_back("paris");
    }
    if (value.empty() || std::string("park").rfind(value, 0) == 0) {
      values.push_back("park");
    }
    if (value.empty() || std::string("party").rfind(value, 0) == 0) {
      values.push_back("party");
    }
    return Json{{"values", std::move(values)}};
  }
};

class FunctionTool final : public IToolHandler {
public:
  using Fn = std::function<Result<ToolOutcome>(const Json&, RequestContext&,
                                               IProgressSink&)>;
  explicit FunctionTool(Fn fn) : fn_(std::move(fn)) {}
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    return fn_(arguments, context, progress);
  }

private:
  Fn fn_;
};
void add_tool(McpServer& server, const std::string& name,
              std::function<Result<ToolOutcome>(const Json&, RequestContext&,
                                                IProgressSink&)> fn) {
  ToolDescriptor descriptor;
  descriptor.name = name;
  descriptor.description = name;
  descriptor.inputSchema = Json{{"type", "object"}};
  server.tools().add(std::move(descriptor),
                     std::make_shared<FunctionTool>(std::move(fn)));
}


void configure(McpServer& server) {
  add_tool(server, "test_simple_text",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             return ToolOutcome(text_result(
                 "This is a simple text response for testing."));
           });
  add_tool(server, "test_image_content",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             ToolResult result;
             ContentBlock block;
             block.type = ContentType::Image;
             block.dataBase64 = kPng;
             block.mimeType = "image/png";
             result.content.push_back(std::move(block));
             return ToolOutcome(std::move(result));
           });
  add_tool(server, "test_audio_content",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             ToolResult result;
             ContentBlock block;
             block.type = ContentType::Audio;
             block.dataBase64 = kWav;
             block.mimeType = "audio/wav";
             result.content.push_back(std::move(block));
             return ToolOutcome(std::move(result));
           });
  add_tool(server, "test_embedded_resource",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             ToolResult result;
             ContentBlock block;
             block.type = ContentType::EmbeddedResource;
             block.resource = Json{{"uri", "test://embedded-resource"},
                                   {"mimeType", "text/plain"},
                                   {"text",
                                    "This is an embedded resource content."}};
             result.content.push_back(std::move(block));
             return ToolOutcome(std::move(result));
           });
  add_tool(server, "test_multiple_content_types",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             ToolResult result;
             result.content.push_back(ContentBlock{
                 ContentType::Text, "Multiple content types test:"});
             ContentBlock image;
             image.type = ContentType::Image;
             image.dataBase64 = kPng;
             image.mimeType = "image/png";
             result.content.push_back(std::move(image));
             ContentBlock resource;
             resource.type = ContentType::EmbeddedResource;
             resource.resource = Json{
                 {"uri", "test://mixed-content-resource"},
                 {"mimeType", "application/json"},
                 {"text", R"({"test":"data","value":123})"}};
             result.content.push_back(std::move(resource));
             return ToolOutcome(std::move(result));
           });
  add_tool(server, "test_error_handling",
           [](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             return ToolOutcome(text_result(
                 "This tool intentionally returns an error for testing",
                 true));
           });
  add_tool(server, "test_tool_with_progress",
           [](const Json&, RequestContext&, IProgressSink& progress) -> Result<ToolOutcome> {
             progress.report(0.0, "start");
             progress.report(0.5, "half");
             progress.report(1.0, "done");
             return ToolOutcome(text_result("Progress test complete"));
           });
  add_tool(server, "test_streaming_elicitation",
           [](const Json&, RequestContext&, IProgressSink& progress) -> Result<ToolOutcome> {
             progress.report(0.5, "streaming");
             return ToolOutcome(text_result("Streaming elicitation complete"));
           });
  auto logging_handler =
      [&server](const Json&, RequestContext& context,
                IProgressSink&) -> Result<ToolOutcome> {
    const auto level = server.logLevel(context.sessionId);
    if (level.has_value()) {
      server.sendNotification(
          context.sessionId, "notifications/message",
          Json{{"level", *level}, {"data", "Tool execution started"}});
      server.sendNotification(
          context.sessionId, "notifications/message",
          Json{{"level", *level}, {"data", "Tool processing data"}});
      server.sendNotification(
          context.sessionId, "notifications/message",
          Json{{"level", *level}, {"data", "Tool execution completed"}});
    }
    return ToolOutcome(text_result("Logging tool complete"));
  };
  add_tool(server, "test_logging_tool", logging_handler);
  add_tool(server, "test_tool_with_logging", logging_handler);

  auto capability_error = [](const char* capability) {
    return Result<ToolOutcome>::failure(McpError::make(
        ErrorCode::MissingRequiredClientCapability,
        std::string(capability) + " capability is required",
        Json{{"requiredCapabilities",
              Json{{capability, Json::object()}}}}));
  };

  add_tool(server, "test_sampling",
           [&server, capability_error](const Json& arguments,
                                       RequestContext& context,
                                       IProgressSink&) -> Result<ToolOutcome> {
             if (!context.clientCapabilities.is_object() ||
                 !context.clientCapabilities.contains("sampling")) {
               return capability_error("sampling");
             }
             const std::string prompt =
                 arguments.value("prompt", std::string{});
             Json params = Json{
                 {"messages",
                  Json::array({Json{{"role", "user"},
                                    {"content",
                                     Json{{"type", "text"},
                                          {"text", prompt}}}}})},
                 {"maxTokens", 100}};
             auto result = server.requestClient(
                 context.sessionId, "sampling/createMessage", params,
                 std::chrono::seconds(5));
             if (!result.hasValue()) {
               return Result<ToolOutcome>::failure(result.error());
             }
             std::string text = result.value().dump();
             if (result.value().is_object() &&
                 result.value().contains("content") &&
                 result.value().at("content").is_object() &&
                 result.value().at("content").contains("text") &&
                 result.value().at("content").at("text").is_string()) {
               text = result.value().at("content").at("text").get<std::string>();
             }
             return ToolOutcome(text_result("LLM response: " + text));
           });

  add_tool(server, "test_elicitation",
           [&server, capability_error](const Json& arguments,
                                       RequestContext& context,
                                       IProgressSink&) -> Result<ToolOutcome> {
             if (!context.clientCapabilities.is_object() ||
                 !context.clientCapabilities.contains("elicitation")) {
               return capability_error("elicitation");
             }
             Json params = Json{
                 {"message", arguments.value("message", std::string{})},
                 {"requestedSchema",
                  Json{{"type", "object"},
                       {"properties",
                        Json{{"username",
                              Json{{"type", "string"},
                                   {"description", "User's response"}}},
                             {"email",
                              Json{{"type", "string"},
                                   {"description", "User's email address"}}}}},
                       {"required", Json::array({"username", "email"})}}}};
             auto result = server.requestClient(
                 context.sessionId, "elicitation/create", params,
                 std::chrono::seconds(5));
             if (!result.hasValue()) {
               return Result<ToolOutcome>::failure(result.error());
             }
             const std::string action =
                 result.value().value("action", std::string{});
             const Json content =
                 result.value().value("content", Json::object());
             return ToolOutcome(text_result(
                 "User response: action=" + action +
                 ", content=" + content.dump()));
           });

  auto elicitation_text = [&server](const std::string& session_id,
                                    Json params) -> Result<ToolOutcome> {
    auto result = server.requestClient(
        session_id, "elicitation/create", std::move(params),
        std::chrono::seconds(5));
    if (!result.hasValue()) {
      return Result<ToolOutcome>::failure(result.error());
    }
    const std::string action =
        result.value().value("action", std::string{});
    const Json content = result.value().value("content", Json::object());
    return ToolOutcome(text_result(
        "Elicitation completed: action=" + action +
        ", content=" + content.dump()));
  };

  add_tool(server, "test_elicitation_sep1034_defaults",
           [&server, capability_error, elicitation_text](
               const Json&, RequestContext& context,
               IProgressSink&) -> Result<ToolOutcome> {
             if (!context.clientCapabilities.is_object() ||
                 !context.clientCapabilities.contains("elicitation")) {
               return capability_error("elicitation");
             }
             Json params = Json{
                 {"message", "Please review the default values"},
                 {"requestedSchema",
                  Json{{"type", "object"},
                       {"properties",
                        Json{{"name",
                              Json{{"type", "string"},
                                   {"default", "John Doe"}}},
                             {"age",
                              Json{{"type", "integer"}, {"default", 30}}},
                             {"score",
                              Json{{"type", "number"}, {"default", 95.5}}},
                             {"status",
                              Json{{"type", "string"},
                                   {"enum",
                                    Json::array({"active", "inactive",
                                                 "pending"})},
                                   {"default", "active"}}},
                             {"verified",
                              Json{{"type", "boolean"},
                                   {"default", true}}}}}}}};
             return elicitation_text(context.sessionId, std::move(params));
           });

  add_tool(server, "test_elicitation_sep1330_enums",
           [&server, capability_error, elicitation_text](
               const Json&, RequestContext& context,
               IProgressSink&) -> Result<ToolOutcome> {
             if (!context.clientCapabilities.is_object() ||
                 !context.clientCapabilities.contains("elicitation")) {
               return capability_error("elicitation");
             }
             Json params = Json{
                 {"message", "Please select enum options"},
                 {"requestedSchema",
                  Json{{"type", "object"},
                       {"properties",
                        Json{{"untitledSingle",
                              Json{{"type", "string"},
                                   {"enum",
                                    Json::array({"option1", "option2",
                                                 "option3"})}}},
                             {"titledSingle",
                              Json{{"type", "string"},
                                   {"oneOf",
                                    Json::array(
                                        {Json{{"const", "value1"},
                                              {"title", "First Option"}},
                                         Json{{"const", "value2"},
                                              {"title", "Second Option"}}})}}},
                             {"legacyEnum",
                              Json{{"type", "string"},
                                   {"enum",
                                    Json::array({"opt1", "opt2", "opt3"})},
                                   {"enumNames",
                                    Json::array({"Option One",
                                                 "Option Two",
                                                 "Option Three"})}}},
                             {"untitledMulti",
                              Json{{"type", "array"},
                                   {"items",
                                    Json{{"type", "string"},
                                         {"enum",
                                          Json::array({"option1",
                                                       "option2",
                                                       "option3"})}}}}},
                             {"titledMulti",
                              Json{{"type", "array"},
                                   {"items",
                                    Json{{"anyOf",
                                          Json::array(
                                              {Json{{"const", "value1"},
                                                    {"title", "First Choice"}},
                                               Json{{"const", "value2"},
                                                    {"title",
                                                     "Second Choice"}}})}}}}}}}}}};
             return elicitation_text(context.sessionId, std::move(params));
           });
  add_tool(server, "test_missing_capability",
           [](const Json&, RequestContext& context, IProgressSink&) -> Result<ToolOutcome> {
             if (!context.clientCapabilities.is_object() ||
                 !context.clientCapabilities.contains("sampling")) {
               return Result<ToolOutcome>::failure(McpError::make(
                   ErrorCode::MissingRequiredClientCapability,
                   "sampling capability is required",
                   Json{{"requiredCapabilities",
                         Json{{"sampling", Json::object()}}}}));
             }
             return ToolOutcome(text_result("Capability present"));
           });
  add_tool(server, "test_trigger_tool_change",
           [&server](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             server.notifyListChanged(ListKind::Tools);
             return ToolOutcome(text_result("tools changed"));
           });
  add_tool(server, "test_trigger_prompt_change",
           [&server](const Json&, RequestContext&, IProgressSink&) -> Result<ToolOutcome> {
             server.notifyListChanged(ListKind::Prompts);
             return ToolOutcome(text_result("prompts changed"));
           });

  auto input_required = [](Json input_requests, std::string state) {
     InputRequired input;
     input.inputRequests = std::move(input_requests);
     input.requestState = std::move(state);
     return Result<ToolOutcome>(ToolOutcome(std::move(input)));
  };

  add_tool(server, "test_input_required_result_elicitation",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!has_input(context, "user_name")) {
               return input_required(
                   Json{{"user_name",
                         elicitation_request(
                             "What is your name?",
                             Json{{"name", Json{{"type", "string"}}}},
                             Json::array({"name"}))}},
                   "elicitation-state");
             }
             const std::string name =
                 input_content(context, "user_name").value("name",
                                                           std::string{"Alice"});
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("Hello, " + name + "!")));
           });
  add_tool(server, "test_input_required_result_sampling",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!has_input(context, "capital_question")) {
               Json requests = Json::object();
               requests["capital_question"] = Json{
                   {"method", "sampling/createMessage"},
                   {"params",
                    Json{{"messages",
                          Json::array(
                              {Json{{"role", "user"},
                                    {"content",
                                     Json{{"type", "text"},
                                          {"text",
                                           "What is the capital of "
                                           "France?"}}}}})},
                         {"maxTokens", 100}}}};
               return input_required(std::move(requests), "sampling-state");
             }
             return Result<ToolOutcome>(ToolOutcome(
                 text_result("The capital of France is Paris.")));
           });
  add_tool(server, "test_input_required_result_list_roots",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!has_input(context, "client_roots")) {
               return input_required(
                   Json{{"client_roots",
                         Json{{"method", "roots/list"},
                              {"params", Json::object()}}}},
                   "roots-state");
             }
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("Roots received")));
           });
  add_tool(server, "test_input_required_result_request_state",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!has_input(context, "confirm")) {
               return input_required(
                   Json{{"confirm",
                         elicitation_request(
                             "Please confirm",
                             Json{{"ok", Json{{"type", "boolean"}}}},
                             Json::array({"ok"}))}},
                   "state-ok");
             }
             if (!context.requestState.has_value() ||
                 *context.requestState != "state-ok") {
               return Result<ToolOutcome>::failure(McpError::make(
                   ErrorCode::InvalidParams, "invalid requestState"));
             }
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("state-ok")));
           });
  add_tool(server, "test_input_required_result_multiple_inputs",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!has_input(context, "user_name") ||
                 !has_input(context, "greeting") ||
                 !has_input(context, "client_roots")) {
               Json requests = Json::object();
               requests["user_name"] = elicitation_request(
                   "What is your name?",
                   Json{{"name", Json{{"type", "string"}}}},
                   Json::array({"name"}));
               requests["greeting"] = Json{
                   {"method", "sampling/createMessage"},
                   {"params",
                    Json{{"messages",
                          Json::array(
                              {Json{{"role", "user"},
                                    {"content",
                                     Json{{"type", "text"},
                                          {"text",
                                           "Generate a greeting"}}}}})},
                         {"maxTokens", 50}}}};
               requests["client_roots"] =
                   Json{{"method", "roots/list"},
                        {"params", Json::object()}};
               return input_required(std::move(requests), "multi-state");
             }
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("All inputs received")));
           });
  add_tool(server, "test_input_required_result_multi_round",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!context.requestState.has_value()) {
               return input_required(
                   Json{{"step1",
                         elicitation_request(
                             "Step 1: What is your name?",
                             Json{{"name", Json{{"type", "string"}}}},
                             Json::array({"name"}))}},
                   "round-1");
             }
             if (*context.requestState == "round-1") {
               return input_required(
                   Json{{"step2",
                         elicitation_request(
                             "Step 2: What is your favorite color?",
                             Json{{"color", Json{{"type", "string"}}}},
                             Json::array({"color"}))}},
                   "round-2");
             }
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("multi-round-complete")));
           });
  add_tool(server, "test_input_required_result_tampered_state",
           [input_required](const Json&, RequestContext& context,
                            IProgressSink&) -> Result<ToolOutcome> {
             if (!context.requestState.has_value()) {
               return input_required(
                   Json{{"confirm",
                         elicitation_request(
                             "Confirm",
                             Json{{"ok", Json{{"type", "boolean"}}}},
                             Json::array({"ok"}))}},
                   "tamper-state");
             }
             if (*context.requestState != "tamper-state") {
               return Result<ToolOutcome>::failure(McpError::make(
                   ErrorCode::InvalidParams, "invalid requestState"));
             }
             return Result<ToolOutcome>(
                 ToolOutcome(text_result("state accepted")));
           });
  add_tool(server, "test_input_required_result_capabilities",
           [](const Json&, RequestContext& context, IProgressSink&) -> Result<ToolOutcome> {
             if (has_input(context, "capital_question")) {
               return Result<ToolOutcome>(
                   ToolOutcome(text_result("capabilities complete")));
             }
             InputRequired input;
             Json requests = Json::object();
             requests["capital_question"] = Json{
                 {"method", "sampling/createMessage"},
                 {"params",
                  Json{{"messages",
                        Json::array(
                            {Json{{"role", "user"},
                                  {"content",
                                   Json{{"type", "text"},
                                        {"text", "Question"}}}}})},
                       {"maxTokens", 50}}}};
             input.inputRequests = std::move(requests);
             input.requestState = "cap-state";
             return Result<ToolOutcome>(ToolOutcome(std::move(input)));
           });
}

} // namespace

int main(int argc, char** argv) {
  int port = 0;
  std::string bind = "127.0.0.1";
  bool http = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--http") {
      http = true;
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--bind" && i + 1 < argc) {
      bind = argv[++i];
    }
  }
  if (!http) {
    std::cerr << "tugmcp-conformance: --http is required\n";
    return 2;
  }

  StreamableHttpServerOptions http_options;
  http_options.bind = bind;
  http_options.port = port;
  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created.hasValue()) {
    std::cerr << "tugmcp-conformance: " << created.error().message << "\n";
    return 1;
  }
  auto concrete =
      std::dynamic_pointer_cast<HttpServerTransport>(created.value());
  if (!concrete) {
    std::cerr << "tugmcp-conformance: unexpected transport\n";
    return 1;
  }

  ServerOptions options;
  options.name = "tugmcp-conformance";
  options.version = "0.1.0";
  options.allowUnvalidatedTools = true;
  options.supportedVersions = {"2026-07-28", "2025-11-25"};

  auto resources = std::make_shared<FixtureResources>();
  auto prompts = std::make_shared<FixturePrompts>();
  auto completions = std::make_shared<FixtureCompletion>();
  options.resources = resources;
  options.prompts = prompts;
  options.completions = completions;

  McpServer server(options, created.value());
  configure(server);

  Status started = created.value()->start();
  if (!started) {
    std::cerr << "tugmcp-conformance: " << started.error.message << "\n";
    return 1;
  }
  std::cerr << "tugmcp-conformance listening on http://" << bind << ":"
            << concrete->boundPort() << "/mcp" << std::endl;

  const Status status = server.run();
  return status ? 0 : 1;
}
