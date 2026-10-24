#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>
#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/tool.hpp>

#include "../../src/core/dispatcher.hpp"

using mcp::v1::ContentBlock;
using mcp::v1::Dispatcher;
using mcp::v1::DispatchConfig;
using mcp::v1::ErrorCode;
using mcp::v1::InputRequired;
using mcp::v1::IProgressSink;
using mcp::v1::IPromptProvider;
using mcp::v1::IResourceProvider;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::ListParams;
using mcp::v1::Message;
using mcp::v1::MessageKind;
using mcp::v1::Page;
using mcp::v1::PromptDescriptor;
using mcp::v1::PromptOutcome;
using mcp::v1::PromptResult;
using mcp::v1::RequestContext;
using mcp::v1::ResourceContent;
using mcp::v1::ResourceDescriptor;
using mcp::v1::ResourceOutcome;
using mcp::v1::ResourceTemplate;
using mcp::v1::Result;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolRegistry;
using mcp::v1::ToolResult;

namespace {

constexpr const char* kV2026 = "2026-07-28";
constexpr const char* kV2025 = "2025-11-25";

using InvokeFn = std::function<Result<ToolOutcome>(
    const Json&, RequestContext&, IProgressSink&)>;

class FakeTool final : public IToolHandler {
public:
  explicit FakeTool(InvokeFn fn) : fn_(std::move(fn)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    return fn_(arguments, context, progress);
  }

private:
  InvokeFn fn_;
};

void addTool(ToolRegistry& registry, const std::string& name, InvokeFn fn) {
  ToolDescriptor descriptor;
  descriptor.name = name;
  descriptor.title = name + " title";
  descriptor.description = name + " description";
  descriptor.inputSchema = Json::object();
  REQUIRE(registry.add(std::move(descriptor), std::make_shared<FakeTool>(
                                                  std::move(fn)))
              .ok);
}

Dispatcher makeDispatcher(ToolRegistry& registry,
                          IResourceProvider* resources = nullptr,
                          IPromptProvider* prompts = nullptr,
                          DispatchConfig config = {}) {
  return Dispatcher(registry, resources, prompts, std::move(config));
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

Result<ToolOutcome> completeText(std::string text, bool is_error = false) {
  ToolResult result;
  result.isError = is_error;
  result.content.push_back(ContentBlock{"text", std::move(text)});
  return Result<ToolOutcome>(ToolOutcome(std::move(result)));
}

class FakeResources final : public IResourceProvider {
public:
  Result<Page<ResourceDescriptor>> list(const ListParams& params,
                                        RequestContext&) override {
    last_list_cursor = params.cursor;
    return list_page;
  }

  Result<Page<ResourceTemplate>> templates(const ListParams& params,
                                           RequestContext&) override {
    last_template_cursor = params.cursor;
    return template_page;
  }

  Result<ResourceOutcome> read(std::string_view uri,
                               RequestContext&) override {
    last_read_uri = std::string(uri);
    return read_outcome;
  }

  Page<ResourceDescriptor> list_page;
  Page<ResourceTemplate> template_page;
  ResourceOutcome read_outcome{ResourceContent{}};
  std::string last_list_cursor;
  std::string last_template_cursor;
  std::string last_read_uri;
};

class FakePrompts final : public IPromptProvider {
public:
  Result<Page<PromptDescriptor>> list(const ListParams& params,
                                      RequestContext&) override {
    last_list_cursor = params.cursor;
    return list_page;
  }

  Result<PromptOutcome> get(std::string_view name, const Json& args,
                            RequestContext&) override {
    last_name = std::string(name);
    last_args = args;
    return get_outcome;
  }

  Page<PromptDescriptor> list_page;
  PromptOutcome get_outcome{PromptResult{}};
  std::string last_list_cursor;
  std::string last_name;
  Json last_args;
};

} // namespace

TEST_CASE("protocol surface: initialize negotiates and rejects versions") {
  ToolRegistry registry;
  DispatchConfig config;
  config.serverName = "surface";
  config.serverVersion = "1.2.3";
  auto dispatcher = makeDispatcher(registry, nullptr, nullptr, config);

  auto response = dispatcher.dispatch(
      request("initialize", Json{{"protocolVersion", "2026-07-28"}}), kV2026);
  REQUIRE(response.has_value());
  REQUIRE_FALSE(response->error.has_value());
  CHECK(response->result.at("protocolVersion") == "2026-07-28");
  CHECK(response->result.at("capabilities").at("tools").is_object());
  CHECK(response->result.at("serverInfo").at("name") == "surface");
  CHECK(response->result.at("serverInfo").at("version") == "1.2.3");

  auto unsupported = dispatcher.dispatch(
      request("initialize", Json{{"protocolVersion", "2024-11-05"}}), kV2025);
  REQUIRE(unsupported.has_value());
  REQUIRE(unsupported->error.has_value());
  CHECK(static_cast<int>(unsupported->error->code) ==
        static_cast<int>(ErrorCode::UnsupportedVersion));

  auto bad_envelope = dispatcher.dispatch(request("ping"), "1999-01-01");
  REQUIRE(bad_envelope.has_value());
  REQUIRE(bad_envelope->error.has_value());
  CHECK(static_cast<int>(bad_envelope->error->code) ==
        static_cast<int>(ErrorCode::UnsupportedVersion));
}

TEST_CASE("protocol surface: discover reports versions and instructions") {
  ToolRegistry registry;
  DispatchConfig config;
  config.instructions = "be careful";
  auto dispatcher = makeDispatcher(registry, nullptr, nullptr, config);

  auto response =
      dispatcher.dispatch(request("server/discover"), kV2026);
  REQUIRE(response.has_value());
  REQUIRE_FALSE(response->error.has_value());
  CHECK(response->result.at("supportedVersions") ==
        Json::array({"2026-07-28", "2025-11-25"}));
  CHECK(response->result.at("instructions") == "be careful");
  CHECK(response->result.at("serverInfo").at("name") == "tugmcp");
}

TEST_CASE("protocol surface: tools/list paginates with cursors") {
  ToolRegistry registry;
  addTool(registry, "alpha", [](const Json&, RequestContext&, IProgressSink&) {
    return completeText("alpha");
  });
  addTool(registry, "beta", [](const Json&, RequestContext&, IProgressSink&) {
    return completeText("beta");
  });
  addTool(registry, "gamma", [](const Json&, RequestContext&, IProgressSink&) {
    return completeText("gamma");
  });

  DispatchConfig config;
  config.pageSize = 2;
  auto dispatcher = makeDispatcher(registry, nullptr, nullptr, config);

  auto first = dispatcher.dispatch(request("tools/list"), kV2025);
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->error.has_value());
  REQUIRE(first->result.at("tools").size() == 2);
  CHECK(first->result.at("tools").at(0).at("name") == "alpha");
  CHECK(first->result.at("tools").at(1).at("name") == "beta");
  REQUIRE(first->result.contains("nextCursor"));
  CHECK(first->result.at("nextCursor") == "2");

  auto second = dispatcher.dispatch(
      request("tools/list", Json{{"cursor", "2"}}), kV2025);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->error.has_value());
  REQUIRE(second->result.at("tools").size() == 1);
  CHECK(second->result.at("tools").at(0).at("name") == "gamma");
  CHECK_FALSE(second->result.contains("nextCursor"));

  auto invalid = dispatcher.dispatch(
      request("tools/list", Json{{"cursor", "not-a-number"}}), kV2025);
  REQUIRE(invalid.has_value());
  REQUIRE(invalid->error.has_value());
  CHECK(static_cast<int>(invalid->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("protocol surface: tools/list resultType and cache fields") {
  ToolRegistry registry;
  addTool(registry, "alpha", [](const Json&, RequestContext&, IProgressSink&) {
    return completeText("alpha");
  });

  DispatchConfig config;
  config.pageSize = 0;
  auto dispatcher = makeDispatcher(registry, nullptr, nullptr, config);

  auto modern = dispatcher.dispatch(request("tools/list"), kV2026);
  REQUIRE(modern.has_value());
  REQUIRE_FALSE(modern->error.has_value());
  CHECK(modern->result.at("resultType") == "complete");
  CHECK(modern->result.at("ttlMs") == 0);
  CHECK(modern->result.at("cacheScope") == "private");

  auto legacy = dispatcher.dispatch(request("tools/list"), kV2025);
  REQUIRE(legacy.has_value());
  REQUIRE_FALSE(legacy->error.has_value());
  CHECK_FALSE(legacy->result.contains("resultType"));
  CHECK_FALSE(legacy->result.contains("ttlMs"));
  CHECK_FALSE(legacy->result.contains("cacheScope"));
}

TEST_CASE("protocol surface: tools/call complete result") {
  ToolRegistry registry;
  addTool(registry, "echo", [](const Json& arguments, RequestContext&,
                               IProgressSink&) {
    ToolResult result;
    result.content.push_back(
        ContentBlock{"text", arguments.value("text", std::string{})});
    result.structuredContent = Json{{"text", arguments.value("text", "")}};
    result.isError = true;
    return Result<ToolOutcome>(ToolOutcome(std::move(result)));
  });

  auto dispatcher = makeDispatcher(registry);
  const Json params{{"name", "echo"}, {"arguments", Json{{"text", "hello"}}}};

  auto modern = dispatcher.dispatch(request("tools/call", params), kV2026);
  REQUIRE(modern.has_value());
  REQUIRE_FALSE(modern->error.has_value());
  CHECK(modern->result.at("content").at(0).at("type") == "text");
  CHECK(modern->result.at("content").at(0).at("text") == "hello");
  CHECK(modern->result.at("structuredContent").at("text") == "hello");
  CHECK(modern->result.at("isError") == true);
  CHECK(modern->result.at("resultType") == "complete");

  auto legacy = dispatcher.dispatch(request("tools/call", params), kV2025);
  REQUIRE(legacy.has_value());
  REQUIRE_FALSE(legacy->error.has_value());
  CHECK_FALSE(legacy->result.contains("resultType"));
  CHECK(legacy->result.at("isError") == true);
}

TEST_CASE("protocol surface: progress notifications follow the token") {
  ToolRegistry registry;
  addTool(registry, "work", [](const Json&, RequestContext&,
                               IProgressSink& progress) {
    progress.report(0.5, "halfway");
    return completeText("done");
  });

  auto dispatcher = makeDispatcher(registry);
  std::vector<Json> notifications;

  auto with_token = dispatcher.dispatch(
      request("tools/call",
              Json{{"name", "work"},
                   {"arguments", Json::object()},
                   {"_meta", Json{{"progressToken", "tok"}}}}),
      kV2026, [&notifications](Json note) {
        notifications.push_back(std::move(note));
      });
  REQUIRE(with_token.has_value());
  REQUIRE_FALSE(with_token->error.has_value());
  REQUIRE(notifications.size() == 1);
  CHECK(notifications.at(0).at("method") == "notifications/progress");
  CHECK(notifications.at(0).at("params").at("progressToken") == "tok");
  CHECK(notifications.at(0).at("params").at("progress") == 0.5);
  CHECK(notifications.at(0).at("params").at("message") == "halfway");

  notifications.clear();
  auto without_token = dispatcher.dispatch(
      request("tools/call", Json{{"name", "work"},
                                 {"arguments", Json::object()}}),
      kV2026, [&notifications](Json note) {
        notifications.push_back(std::move(note));
      });
  REQUIRE(without_token.has_value());
  CHECK(notifications.empty());

  auto invalid = dispatcher.dispatch(
      request("tools/call",
              Json{{"name", "work"},
                   {"arguments", Json::object()},
                   {"_meta", Json{{"progressToken", true}}}}),
      kV2026, [&notifications](Json note) {
        notifications.push_back(std::move(note));
      });
  REQUIRE(invalid.has_value());
  REQUIRE(invalid->error.has_value());
  CHECK(static_cast<int>(invalid->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("protocol surface: MRTR round-trip and validation") {
  ToolRegistry registry;
  std::string seen_state;
  Json seen_responses = nullptr;
  addTool(registry, "ask",
          [&seen_state, &seen_responses](const Json&, RequestContext& context,
                                          IProgressSink&) {
            if (context.requestState.has_value()) {
              seen_state = *context.requestState;
              seen_responses = context.inputResponses;
              return completeText("answered");
            }
            InputRequired input;
            input.inputRequests = Json{{"answer", "boolean"}};
            input.requestState = "state-1";
            return Result<ToolOutcome>(ToolOutcome(std::move(input)));
          });

  auto dispatcher = makeDispatcher(registry);
  const Json base_params{{"name", "ask"}, {"arguments", Json::object()}};

  auto first = dispatcher.dispatch(request("tools/call", base_params), kV2026);
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->error.has_value());
  CHECK(first->result.at("resultType") == "input_required");
  CHECK(first->result.at("requestState") == "state-1");
  CHECK(first->result.at("inputRequests") == Json{{"answer", "boolean"}});

  auto retry = dispatcher.dispatch(
      request("tools/call",
              Json{{"name", "ask"},
                   {"arguments", Json::object()},
                   {"requestState", "state-1"},
                   {"inputResponses", Json{{"answer", true}}}}),
      kV2026);
  REQUIRE(retry.has_value());
  REQUIRE_FALSE(retry->error.has_value());
  CHECK(seen_state == "state-1");
  CHECK(seen_responses == Json{{"answer", true}});
  CHECK(retry->result.at("resultType") == "complete");

  auto only_state = dispatcher.dispatch(
      request("tools/call", Json{{"name", "ask"},
                                 {"arguments", Json::object()},
                                 {"requestState", "state-1"}}),
      kV2026);
  REQUIRE(only_state.has_value());
  REQUIRE(only_state->error.has_value());
  CHECK(static_cast<int>(only_state->error->code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(only_state->error->message.find("provided together") !=
        std::string::npos);

  auto legacy = dispatcher.dispatch(request("tools/call", base_params), kV2025);
  REQUIRE(legacy.has_value());
  REQUIRE(legacy->error.has_value());
  CHECK(static_cast<int>(legacy->error->code) ==
        static_cast<int>(ErrorCode::InternalError));
  CHECK(legacy->error->data.at("reason") == "input_required_unsupported");
}

TEST_CASE("protocol surface: resources lists and reads") {
  ToolRegistry registry;
  FakeResources resources;

  ResourceDescriptor descriptor;
  descriptor.uri = "file:///a.txt";
  descriptor.name = "a";
  descriptor.title = "A";
  descriptor.mimeType = "text/plain";
  descriptor.size = 5;
  resources.list_page.items.push_back(descriptor);
  resources.list_page.nextCursor = "next";
  resources.list_page.ttlMs = 250;
  resources.list_page.cacheScope = "public";

  ResourceTemplate resource_template;
  resource_template.uriTemplate = "file:///{path}";
  resource_template.name = "files";
  resources.template_page.items.push_back(resource_template);
  resources.template_page.ttlMs = 50;

  ResourceContent content;
  content.uri = "file:///a.txt";
  content.mimeType = "text/plain";
  content.text = "hello";
  content.ttlMs = 500;
  content.cacheScope = "private";
  resources.read_outcome = ResourceOutcome(std::move(content));

  auto dispatcher = makeDispatcher(registry, &resources);

  auto list = dispatcher.dispatch(
      request("resources/list", Json{{"cursor", "page-2"}}), kV2026);
  REQUIRE(list.has_value());
  REQUIRE_FALSE(list->error.has_value());
  CHECK(resources.last_list_cursor == "page-2");
  CHECK(list->result.at("resources").at(0).at("uri") == "file:///a.txt");
  CHECK(list->result.at("resources").at(0).at("size") == 5);
  CHECK(list->result.at("nextCursor") == "next");
  CHECK(list->result.at("resultType") == "complete");
  CHECK(list->result.at("ttlMs") == 250);
  CHECK(list->result.at("cacheScope") == "public");

  auto templates = dispatcher.dispatch(request("resources/templates/list"),
                                       kV2026);
  REQUIRE(templates.has_value());
  REQUIRE_FALSE(templates->error.has_value());
  CHECK(templates->result.at("resourceTemplates").at(0).at("uriTemplate") ==
        "file:///{path}");
  CHECK(templates->result.at("ttlMs") == 50);
  CHECK(templates->result.at("cacheScope") == "private");

  auto read = dispatcher.dispatch(
      request("resources/read", Json{{"uri", "file:///a.txt"}}), kV2026);
  REQUIRE(read.has_value());
  REQUIRE_FALSE(read->error.has_value());
  CHECK(resources.last_read_uri == "file:///a.txt");
  REQUIRE(read->result.at("contents").size() == 1);
  CHECK(read->result.at("contents").at(0).at("text") == "hello");
  CHECK(read->result.at("contents").at(0).at("ttlMs") == 500);
  CHECK(read->result.at("contents").at(0).at("cacheScope") == "private");
  CHECK(read->result.at("resultType") == "complete");

  auto legacy_read = dispatcher.dispatch(
      request("resources/read", Json{{"uri", "file:///a.txt"}}), kV2025);
  REQUIRE(legacy_read.has_value());
  REQUIRE_FALSE(legacy_read->error.has_value());
  CHECK_FALSE(legacy_read->result.contains("resultType"));
  CHECK_FALSE(legacy_read->result.at("contents").at(0).contains("ttlMs"));

  auto no_provider = makeDispatcher(registry);
  auto missing = no_provider.dispatch(request("resources/list"), kV2026);
  REQUIRE(missing.has_value());
  REQUIRE(missing->error.has_value());
  CHECK(static_cast<int>(missing->error->code) ==
        static_cast<int>(ErrorCode::MethodNotFound));
}

TEST_CASE("protocol surface: prompts list and get") {
  ToolRegistry registry;
  FakePrompts prompts;

  PromptDescriptor descriptor;
  descriptor.name = "greet";
  descriptor.description = "Greeting";
  descriptor.arguments = Json::array({Json{{"name", "who"}}});
  prompts.list_page.items.push_back(descriptor);
  prompts.list_page.nextCursor = "more";

  PromptResult prompt;
  prompt.description = "Greeting";
  prompt.messages = Json::array(
      {Json{{"role", "user"}, {"content", Json{{"type", "text"},
                                               {"text", "hi"}}}}});
  prompts.get_outcome = PromptOutcome(std::move(prompt));

  auto dispatcher = makeDispatcher(registry, nullptr, &prompts);

  auto list = dispatcher.dispatch(request("prompts/list"), kV2026);
  REQUIRE(list.has_value());
  REQUIRE_FALSE(list->error.has_value());
  CHECK(list->result.at("prompts").at(0).at("name") == "greet");
  CHECK(list->result.at("nextCursor") == "more");
  CHECK(list->result.at("resultType") == "complete");

  auto get = dispatcher.dispatch(
      request("prompts/get", Json{{"name", "greet"},
                                  {"arguments", Json{{"who", "world"}}}}),
      kV2026);
  REQUIRE(get.has_value());
  REQUIRE_FALSE(get->error.has_value());
  CHECK(prompts.last_name == "greet");
  CHECK(prompts.last_args.at("who") == "world");
  CHECK(get->result.at("description") == "Greeting");
  CHECK(get->result.at("messages").at(0).at("role") == "user");
  CHECK(get->result.at("resultType") == "complete");

  auto legacy_get = dispatcher.dispatch(
      request("prompts/get", Json{{"name", "greet"}}), kV2025);
  REQUIRE(legacy_get.has_value());
  REQUIRE_FALSE(legacy_get->error.has_value());
  CHECK_FALSE(legacy_get->result.contains("resultType"));
}

TEST_CASE("protocol surface: capabilities reflect configured providers") {
  ToolRegistry registry;
  FakeResources resources;
  FakePrompts prompts;

  auto configured = makeDispatcher(registry, &resources, &prompts);
  auto modern = configured.dispatch(request("server/discover"), kV2026);
  REQUIRE(modern.has_value());
  REQUIRE_FALSE(modern->error.has_value());
  const Json& modern_caps = modern->result.at("capabilities");
  CHECK(modern_caps.at("tools").is_object());
  CHECK(modern_caps.at("resources").at("listChanged") == true);
  CHECK(modern_caps.at("resources").at("subscribe") == true);
  CHECK(modern_caps.at("prompts").at("listChanged") == true);
  CHECK(modern_caps.contains("subscriptions"));
  CHECK(modern_caps.at("subscriptions").is_object());

  auto legacy = configured.dispatch(request("server/discover"), kV2025);
  REQUIRE(legacy.has_value());
  REQUIRE_FALSE(legacy->error.has_value());
  CHECK_FALSE(legacy->result.at("capabilities").contains("subscriptions"));

  auto bare = makeDispatcher(registry);
  auto bare_discover = bare.dispatch(request("server/discover"), kV2026);
  REQUIRE(bare_discover.has_value());
  REQUIRE_FALSE(bare_discover->error.has_value());
  CHECK_FALSE(bare_discover->result.at("capabilities").contains("resources"));
  CHECK_FALSE(bare_discover->result.at("capabilities").contains("prompts"));
}

TEST_CASE("protocol surface: content blocks serialize non-text fields") {
  ToolRegistry registry;
  addTool(registry, "rich", [](const Json&, RequestContext&,
                               IProgressSink&) {
    ToolResult result;
    ContentBlock block;
    block.type = "image";
    block.text = "caption";
    block.mimeType = "image/png";
    block.dataBase64 = "aGk=";
    block.uri = "file:///image.png";
    block.annotations = Json{{"priority", 0.5}};
    result.content.push_back(std::move(block));
    return Result<ToolOutcome>(ToolOutcome(std::move(result)));
  });

  auto dispatcher = makeDispatcher(registry);
  auto response = dispatcher.dispatch(
      request("tools/call", Json{{"name", "rich"},
                                 {"arguments", Json::object()}}),
      kV2026);
  REQUIRE(response.has_value());
  REQUIRE_FALSE(response->error.has_value());
  const Json& block = response->result.at("content").at(0);
  CHECK(block.at("type") == "image");
  CHECK(block.at("text") == "caption");
  CHECK(block.at("mimeType") == "image/png");
  CHECK(block.at("data") == "aGk=");
  CHECK(block.at("uri") == "file:///image.png");
  CHECK(block.at("annotations").at("priority") == 0.5);
  CHECK_FALSE(response->result.contains("ttlMs"));
}
