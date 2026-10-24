#include "dispatcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tugmcp/message.hpp>
#include <tugmcp/operation.hpp>
#include <tugmcp/prompt.hpp>
#include <tugmcp/request_context.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/tool.hpp>

namespace mcp {
namespace v1 {
namespace {

constexpr const char* kWire2026 = "2026-07-28";
constexpr const char* kWire2025 = "2025-11-25";

Json serverInfo(const DispatchConfig& config) {
  Json info;
  info["name"] = config.serverName;
  info["version"] = config.serverVersion;
  return info;
}

bool isSupported(const DispatchConfig& config, const std::string& version) {
  return std::find(config.supportedVersions.begin(),
                   config.supportedVersions.end(),
                   version) != config.supportedVersions.end();
}

Json capabilities(const DispatchConfig& config,
                  const IResourceProvider* resources,
                  const IPromptProvider* prompts, bool is2026) {
  Json value;
  value["tools"] = Json{{"listChanged", true}};
  if (resources != nullptr) {
    value["resources"] = Json{{"listChanged", true}};
  }
  if (prompts != nullptr) {
    value["prompts"] = Json{{"listChanged", true}};
  }
  if (is2026 && config.supportsSubscriptions) {
    value["subscriptions"] = Json::object();
  }
  return value;
}

Json descriptorToJson(const ToolDescriptor& descriptor) {
  Json value;
  value["name"] = descriptor.name;
  value["title"] = descriptor.title;
  value["description"] = descriptor.description;
  value["inputSchema"] = descriptor.inputSchema;
  if (!descriptor.outputSchema.is_null()) {
    value["outputSchema"] = descriptor.outputSchema;
  }
  if (descriptor.idempotent) {
    Json annotations = descriptor.annotations.is_object()
                           ? descriptor.annotations
                           : Json::object();
    annotations["idempotent"] = true;
    value["annotations"] = std::move(annotations);
  } else if (!descriptor.annotations.is_null()) {
    value["annotations"] = descriptor.annotations;
  }
  return value;
}

Result<Json> contentBlockToJson(const ContentBlock& block) {
  Json item;
  item["type"] = std::string(toString(block.type));

  switch (block.type) {
  case ContentType::Text:
    item["text"] = block.text;
    break;
  case ContentType::Image:
  case ContentType::Audio:
    item["data"] = block.dataBase64;
    item["mimeType"] = block.mimeType;
    break;
  case ContentType::ResourceLink:
    item["uri"] = block.uri;
    item["name"] = block.name;
    if (!block.description.empty()) {
      item["description"] = block.description;
    }
    if (!block.mimeType.empty()) {
      item["mimeType"] = block.mimeType;
    }
    break;
  case ContentType::EmbeddedResource: {
    if (!block.resource.is_object()) {
      return Result<Json>::failure(McpError::make(
          ErrorCode::InternalError, "invalid content block",
          Json{{"reason", "invalid_content_block"}}));
    }
    Json resource = Json::object();
    for (const char* key : {"uri", "mimeType", "text", "blob"}) {
      if (block.resource.contains(key)) {
        resource[key] = block.resource.at(key);
      }
    }
    item["resource"] = std::move(resource);
    break;
  }
  }

  if (!block.annotations.is_null()) {
    item["annotations"] = block.annotations;
  }
  return item;
}

std::string describeIssues(const Json& data) {
  std::string text;
  if (!data.is_object() || !data.contains("issues") ||
      !data.at("issues").is_array()) {
    return text;
  }
  for (const auto& issue : data.at("issues")) {
    if (!text.empty()) {
      text += "; ";
    }
    text += issue.value("path", std::string{});
    const std::string keyword = issue.value("keyword", std::string{});
    if (!keyword.empty()) {
      text += " [";
      text += keyword;
      text += "]";
    }
  }
  return text;
}

Json completeResult(bool is2026) {
  Json result = Json::object();
  if (is2026) {
    result["resultType"] = std::string(toString(ResultType::Complete));
  }
  return result;
}

void addCacheFields(Json& result, std::uint32_t ttlMs,
                    const std::string& cacheScope) {
  result["ttlMs"] = ttlMs;
  result["cacheScope"] = cacheScope.empty() ? "private" : cacheScope;
}

Result<std::string> readCursor(const Json& params) {
  if (params.is_null()) {
    return std::string{};
  }
  if (!params.is_object()) {
    return Result<std::string>::failure(
        McpError::make(ErrorCode::InvalidParams, "invalid cursor"));
  }
  if (!params.contains("cursor")) {
    return std::string{};
  }
  const Json& cursor = params.at("cursor");
  if (!cursor.is_string()) {
    return Result<std::string>::failure(
        McpError::make(ErrorCode::InvalidParams, "invalid cursor"));
  }
  return cursor.get<std::string>();
}

Result<std::size_t> parseCursorOffset(const std::string& cursor) {
  if (cursor.empty()) {
    return std::size_t{0};
  }
  std::size_t value = 0;
  for (const char character : cursor) {
    if (character < '0' || character > '9') {
      return Result<std::size_t>::failure(
          McpError::make(ErrorCode::InvalidParams, "invalid cursor"));
    }
    const std::size_t digit = static_cast<std::size_t>(character - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return Result<std::size_t>::failure(
          McpError::make(ErrorCode::InvalidParams, "invalid cursor"));
    }
    value = value * 10 + digit;
  }
  return value;
}

struct MrtrParams {
  Json inputResponses;
  std::optional<std::string> requestState;
};

Result<MrtrParams> readMrtr(const Json& params) {
  MrtrParams result;
  const bool has_request_state =
      params.is_object() && params.contains("requestState");
  const bool has_input_responses =
      params.is_object() && params.contains("inputResponses");

  if (has_request_state != has_input_responses) {
    return Result<MrtrParams>::failure(McpError::make(
        ErrorCode::InvalidParams,
        "requestState and inputResponses must be provided together"));
  }

  if (has_request_state) {
    const Json& state = params.at("requestState");
    if (!state.is_string()) {
      return Result<MrtrParams>::failure(McpError::make(
          ErrorCode::InvalidParams, "requestState must be a string"));
    }
    result.requestState = state.get<std::string>();
  }

  if (has_input_responses) {
    const Json& responses = params.at("inputResponses");
    if (!responses.is_object()) {
      return Result<MrtrParams>::failure(McpError::make(
          ErrorCode::InvalidParams, "inputResponses must be an object"));
    }
    result.inputResponses = responses;
  }

  return result;
}

Result<std::optional<ProgressToken>> readProgressToken(const Json& params) {
  if (!params.is_object() || !params.contains("_meta")) {
    return std::optional<ProgressToken>{};
  }
  const Json& meta = params.at("_meta");
  if (!meta.is_object() || !meta.contains("progressToken")) {
    return std::optional<ProgressToken>{};
  }
  auto token = progressTokenFromJson(meta.at("progressToken"));
  if (!token.hasValue()) {
    return Result<std::optional<ProgressToken>>::failure(token.error());
  }
  return std::optional<ProgressToken>{token.value()};
}

class NullProgressSink final : public IProgressSink {
public:
  void report(double, std::string_view) override {}
};

class BoundProgressSink final : public IProgressSink {
public:
  BoundProgressSink(ProgressToken token, const NotificationSink* notify)
      : token_(std::move(token)), notify_(notify) {}

  void report(double fraction, std::string_view message) override {
    if (notify_ == nullptr || !*notify_) {
      return;
    }
    Json params;
    params["progressToken"] = progressTokenToJson(token_);
    params["progress"] = fraction;
    if (!message.empty()) {
      params["message"] = std::string(message);
    }

    Json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "notifications/progress";
    notification["params"] = std::move(params);
    (*notify_)(std::move(notification));
  }

private:
  ProgressToken token_;
  const NotificationSink* notify_;
};

RequestContext makeContext(const Message& request,
                           const std::shared_ptr<CancellationToken>& token) {
  RequestContext context;
  context.id = request.id;
  context.method = request.method;
  context.cancellation =
      token ? token : std::make_shared<CancellationToken>();
  return context;
}

Result<Json> inputRequiredToJson(const InputRequired& input, bool is2026) {
  if (!is2026) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "input_required is not supported",
        Json{{"reason", "input_required_unsupported"}}));
  }
  if (!input.valid()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "invalid input_required payload",
        Json{{"reason", "invalid_input_required"}}));
  }

  Json result;
  result["resultType"] = std::string(toString(ResultType::InputRequired));
  if (input.inputRequests.has_value()) {
    result["inputRequests"] = *input.inputRequests;
  }
  if (input.requestState.has_value()) {
    result["requestState"] = *input.requestState;
  }
  return result;
}

Result<Json> toolResultToJson(const ToolResult& result, bool is2026) {
  Json content = Json::array();
  for (const auto& block : result.content) {
    auto encoded = contentBlockToJson(block);
    if (!encoded.hasValue()) {
      return Result<Json>::failure(encoded.error());
    }
    content.push_back(std::move(encoded.value()));
  }

  Json value;
  value["content"] = std::move(content);
  if (!result.structuredContent.is_null()) {
    value["structuredContent"] = result.structuredContent;
  }
  if (result.isError) {
    value["isError"] = true;
  }
  if (is2026) {
    value.update(completeResult(is2026));
    addCacheFields(value, result.ttlMs, result.cacheScope);
  }
  return value;
}

Result<Json> callTool(ToolRegistry& registry, const Message& request,
                      bool is2026, const NotificationSink& notify,
                      const std::shared_ptr<CancellationToken>& token) {
  if (!request.params.is_object()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "tools/call params must be an object"));
  }

  const Json& params = request.params;
  if (!params.contains("name") || !params.at("name").is_string()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "tools/call requires a tool name"));
  }

  const std::string name = params.at("name").get<std::string>();
  ToolHandlerPtr handler = registry.handler(name);
  if (!handler) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "unknown tool: " + name));
  }

  Json arguments = Json::object();
  if (params.contains("arguments") && !params.at("arguments").is_null()) {
    if (!params.at("arguments").is_object()) {
      return Result<Json>::failure(McpError::make(
          ErrorCode::InvalidParams, "tool arguments must be an object"));
    }
    arguments = params.at("arguments");
  }

  Status input_status = registry.validateInput(name, arguments);
  if (!input_status) {
    return Result<Json>::failure(input_status.error);
  }

  auto progress_token = readProgressToken(params);
  if (!progress_token.hasValue()) {
    return Result<Json>::failure(progress_token.error());
  }

  auto mrtr = readMrtr(params);
  if (!mrtr.hasValue()) {
    return Result<Json>::failure(mrtr.error());
  }

  RequestContext context = makeContext(request, token);
  context.inputResponses = mrtr.value().inputResponses;
  context.requestState = mrtr.value().requestState;

  NullProgressSink null_sink;
  IProgressSink* sink = &null_sink;
  BoundProgressSink bound_sink(
      progress_token.value().value_or(ProgressToken{}), &notify);
  if (progress_token.value().has_value()) {
    sink = &bound_sink;
  }

  try {
    auto called = handler->invoke(arguments, context, *sink);
    if (!called.hasValue()) {
      return Result<Json>::failure(called.error());
    }

    ToolOutcome outcome = std::move(called.value());
    if (outcome.isInputRequired()) {
      return inputRequiredToJson(outcome.inputRequired(), is2026);
    }

    ToolResult result = std::move(outcome.result());
    const auto descriptor = registry.find(name);
    if (descriptor && !descriptor->outputSchema.is_null()) {
      if (result.structuredContent.is_null()) {
        result.isError = true;
        result.content.push_back(ContentBlock{
            ContentType::Text,
            "output schema is declared but structuredContent is missing"});
      } else {
        Status output_status =
            registry.validateOutput(name, result.structuredContent);
        if (!output_status) {
          result.isError = true;
          std::string note = "output validation failed: " +
                             output_status.error.message;
          const std::string details = describeIssues(output_status.error.data);
          if (!details.empty()) {
            note += " [" + details + "]";
          }
          result.content.push_back(ContentBlock{ContentType::Text, std::move(note)});
        }
      }
    }
    return toolResultToJson(result, is2026);
  } catch (const std::exception& error) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError,
        std::string("tool handler failed: ") + error.what()));
  } catch (...) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "tool handler failed"));
  }
}

Json resourceDescriptorToJson(const ResourceDescriptor& descriptor) {
  Json value;
  value["uri"] = descriptor.uri;
  value["name"] = descriptor.name;
  if (!descriptor.title.empty()) {
    value["title"] = descriptor.title;
  }
  if (!descriptor.description.empty()) {
    value["description"] = descriptor.description;
  }
  if (!descriptor.mimeType.empty()) {
    value["mimeType"] = descriptor.mimeType;
  }
  if (descriptor.size != 0) {
    value["size"] = descriptor.size;
  }
  if (!descriptor.annotations.is_null()) {
    value["annotations"] = descriptor.annotations;
  }
  return value;
}

Json resourceTemplateToJson(const ResourceTemplate& resource_template) {
  Json value;
  value["uriTemplate"] = resource_template.uriTemplate;
  value["name"] = resource_template.name;
  if (!resource_template.description.empty()) {
    value["description"] = resource_template.description;
  }
  if (!resource_template.mimeType.empty()) {
    value["mimeType"] = resource_template.mimeType;
  }
  return value;
}

Json promptDescriptorToJson(const PromptDescriptor& descriptor) {
  Json value;
  value["name"] = descriptor.name;
  if (!descriptor.description.empty()) {
    value["description"] = descriptor.description;
  }
  if (!descriptor.arguments.is_null()) {
    value["arguments"] = descriptor.arguments;
  }
  return value;
}

template <class T, class Converter>
Result<Json> pageToJson(const Page<T>& page, bool is2026,
                        const std::string& key, Converter convert) {
  Json items = Json::array();
  for (const auto& item : page.items) {
    items.push_back(convert(item));
  }

  Json result;
  result[key] = std::move(items);
  if (page.nextCursor.has_value()) {
    result["nextCursor"] = *page.nextCursor;
  }
  if (is2026) {
    result.update(completeResult(is2026));
    addCacheFields(result, page.ttlMs, page.cacheScope);
  }
  return result;
}

Result<Json> listResources(IResourceProvider& provider,
                           const Message& request, const Json& cursor,
                           bool is2026,
                           const std::shared_ptr<CancellationToken>& token) {
  RequestContext context = makeContext(request, token);
  auto page = provider.list(ListParams{cursor}, context);
  if (!page.hasValue()) {
    return Result<Json>::failure(page.error());
  }
  return pageToJson(page.value(), is2026, "resources",
                    [](const ResourceDescriptor& item) {
                      return resourceDescriptorToJson(item);
                    });
}

Result<Json> listResourceTemplates(IResourceProvider& provider,
                                   const Message& request, const Json& cursor,
                                   bool is2026,
                                   const std::shared_ptr<CancellationToken>& token) {
  RequestContext context = makeContext(request, token);
  auto page = provider.templates(ListParams{cursor}, context);
  if (!page.hasValue()) {
    return Result<Json>::failure(page.error());
  }
  return pageToJson(page.value(), is2026, "resourceTemplates",
                    [](const ResourceTemplate& item) {
                      return resourceTemplateToJson(item);
                    });
}

Result<Json> listPrompts(IPromptProvider& provider,
                         const Message& request, const Json& cursor,
                         bool is2026,
                         const std::shared_ptr<CancellationToken>& token) {
  RequestContext context = makeContext(request, token);
  auto page = provider.list(ListParams{cursor}, context);
  if (!page.hasValue()) {
    return Result<Json>::failure(page.error());
  }
  return pageToJson(page.value(), is2026, "prompts",
                    [](const PromptDescriptor& item) {
                      return promptDescriptorToJson(item);
                    });
}

Result<Json> readResource(IResourceProvider& provider,
                          const Message& request, bool is2026,
                          const std::shared_ptr<CancellationToken>& token) {
  if (!request.params.is_object()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "resources/read params must be an object"));
  }

  const Json& params = request.params;
  if (!params.contains("uri") || !params.at("uri").is_string()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams,
        "resources/read requires a string uri"));
  }

  auto mrtr = readMrtr(params);
  if (!mrtr.hasValue()) {
    return Result<Json>::failure(mrtr.error());
  }

  RequestContext context = makeContext(request, token);
  context.inputResponses = mrtr.value().inputResponses;
  context.requestState = mrtr.value().requestState;

  try {
    auto outcome = provider.read(params.at("uri").get<std::string>(), context);
    if (!outcome.hasValue()) {
      return Result<Json>::failure(outcome.error());
    }
    if (outcome.value().isInputRequired()) {
      return inputRequiredToJson(outcome.value().inputRequired(), is2026);
    }

    const ResourceContent& content = outcome.value().result();
    Json item;
    item["uri"] = content.uri;
    if (!content.mimeType.empty()) {
      item["mimeType"] = content.mimeType;
    }
    if (!content.text.is_null()) {
      item["text"] = content.text;
    }
    if (!content.blob.is_null()) {
      item["blob"] = content.blob;
    }
    if (is2026) {
      addCacheFields(item, content.ttlMs, content.cacheScope);
    }

    Json result;
    result["contents"] = Json::array({std::move(item)});
    if (is2026) {
      result.update(completeResult(is2026));
    }
    return result;
  } catch (const std::exception& error) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError,
        std::string("resource provider failed: ") + error.what()));
  } catch (...) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "resource provider failed"));
  }
}

Result<Json> getPrompt(IPromptProvider& provider, const Message& request,
                       bool is2026,
                       const std::shared_ptr<CancellationToken>& token) {
  if (!request.params.is_object()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "prompts/get params must be an object"));
  }

  const Json& params = request.params;
  if (!params.contains("name") || !params.at("name").is_string()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InvalidParams, "prompts/get requires a string name"));
  }

  Json arguments = Json::object();
  if (params.contains("arguments") && !params.at("arguments").is_null()) {
    if (!params.at("arguments").is_object()) {
      return Result<Json>::failure(McpError::make(
          ErrorCode::InvalidParams, "prompt arguments must be an object"));
    }
    arguments = params.at("arguments");
  }

  auto mrtr = readMrtr(params);
  if (!mrtr.hasValue()) {
    return Result<Json>::failure(mrtr.error());
  }

  RequestContext context = makeContext(request, token);
  context.inputResponses = mrtr.value().inputResponses;
  context.requestState = mrtr.value().requestState;

  try {
    auto outcome =
        provider.get(params.at("name").get<std::string>(), arguments, context);
    if (!outcome.hasValue()) {
      return Result<Json>::failure(outcome.error());
    }
    if (outcome.value().isInputRequired()) {
      return inputRequiredToJson(outcome.value().inputRequired(), is2026);
    }

    const PromptResult& prompt = outcome.value().result();
    Json result;
    if (!prompt.description.empty()) {
      result["description"] = prompt.description;
    }
    result["messages"] = prompt.messages;
    if (is2026) {
      result.update(completeResult(is2026));
    }
    return result;
  } catch (const std::exception& error) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError,
        std::string("prompt provider failed: ") + error.what()));
  } catch (...) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "prompt provider failed"));
  }
}

} // namespace

Dispatcher::Dispatcher(ToolRegistry& registry,
                       IResourceProvider* resources,
                       IPromptProvider* prompts, DispatchConfig config)
    : registry_(registry),
      resources_(resources),
      prompts_(prompts),
      config_(std::move(config)) {}

std::optional<Message> Dispatcher::dispatch(
    const Message& request, const std::string& protocolVersion,
    const NotificationSink& notify,
    std::shared_ptr<CancellationToken> token) {
  if (request.kind == MessageKind::Response) {
    return std::nullopt;
  }

  const bool notification = request.kind == MessageKind::Notification;
  const std::string version =
      protocolVersion.empty() ? std::string(kWire2025) : protocolVersion;
  const bool is2026 = version == kWire2026;

  Message response;
  response.kind = MessageKind::Response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  try {
    if (!isSupported(config_, version)) {
      response.error = McpError::make(
          ErrorCode::UnsupportedVersion,
          "unsupported protocol version: " + version);
    } else if (request.method == "initialize") {
      std::string requested = kWire2025;
      if (request.params.is_object() &&
          request.params.contains("protocolVersion")) {
        const Json& version_value = request.params.at("protocolVersion");
        if (!version_value.is_string()) {
          response.error = McpError::make(
              ErrorCode::InvalidParams,
              "initialize params.protocolVersion must be a string");
        } else {
          requested = version_value.get<std::string>();
          if (!isSupported(config_, requested)) {
            response.error = McpError::make(
                ErrorCode::UnsupportedVersion,
                "unsupported protocol version: " + requested);
          }
        }
      }

      if (!response.error.has_value()) {
        Json result;
        result["protocolVersion"] = requested;
        result["capabilities"] =
            capabilities(config_, resources_, prompts_, requested == kWire2026);
        result["serverInfo"] = serverInfo(config_);
        if (!config_.instructions.empty()) {
          result["instructions"] = config_.instructions;
        }
        response.result = std::move(result);
      }
    } else if (request.method == "server/discover") {
      Json supported = Json::array();
      for (const auto& item : config_.supportedVersions) {
        supported.push_back(item);
      }
      Json result = completeResult(is2026);
      result["supportedVersions"] = std::move(supported);
      result["capabilities"] =
          capabilities(config_, resources_, prompts_, is2026);
      result["serverInfo"] = serverInfo(config_);
      if (!config_.instructions.empty()) {
        result["instructions"] = config_.instructions;
      }
      response.result = std::move(result);
    } else if (request.method == "ping") {
      response.result = completeResult(is2026);
    } else if (request.method == "tools/list") {
      auto cursor = readCursor(request.params);
      if (!cursor.hasValue()) {
        response.error = cursor.error();
      } else {
        auto offset = parseCursorOffset(cursor.value());
        if (!offset.hasValue()) {
          response.error = offset.error();
        } else {
          const std::vector<ToolDescriptor> all = registry_.list();
          const std::size_t begin =
              std::min(offset.value(), all.size());
          std::size_t end = all.size();
          if (config_.pageSize != 0) {
            end = std::min(begin + config_.pageSize, all.size());
          }

          Json tools = Json::array();
          for (std::size_t index = begin; index < end; ++index) {
            tools.push_back(descriptorToJson(all[index]));
          }

          Json result;
          result["tools"] = std::move(tools);
          if (config_.pageSize != 0 && end < all.size()) {
            result["nextCursor"] = std::to_string(end);
          }
          if (is2026) {
            result.update(completeResult(is2026));
            addCacheFields(result, 0, "private");
          }
          response.result = std::move(result);
        }
      }
    } else if (request.method == "tools/call") {
      auto called = callTool(registry_, request, is2026, notify, token);
      if (!called.hasValue()) {
        response.error = called.error();
      } else {
        response.result = called.value();
      }
    } else if (request.method == "resources/list" ||
               request.method == "resources/templates/list" ||
               request.method == "resources/read" ||
               request.method == "prompts/list" ||
               request.method == "prompts/get") {
      if (request.method == "resources/list") {
        if (resources_ == nullptr) {
          response.error = McpError::make(
              ErrorCode::MethodNotFound, "method not found: " + request.method);
        } else {
          auto cursor = readCursor(request.params);
          if (!cursor.hasValue()) {
            response.error = cursor.error();
          } else {
            auto result =
                listResources(*resources_, request, cursor.value(), is2026, token);
            if (!result.hasValue()) {
              response.error = result.error();
            } else {
              response.result = result.value();
            }
          }
        }
      } else if (request.method == "resources/templates/list") {
        if (resources_ == nullptr) {
          response.error = McpError::make(
              ErrorCode::MethodNotFound, "method not found: " + request.method);
        } else {
          auto cursor = readCursor(request.params);
          if (!cursor.hasValue()) {
            response.error = cursor.error();
          } else {
            auto result = listResourceTemplates(*resources_, request,
                                                cursor.value(), is2026, token);
            if (!result.hasValue()) {
              response.error = result.error();
            } else {
              response.result = result.value();
            }
          }
        }
      } else if (request.method == "resources/read") {
        if (resources_ == nullptr) {
          response.error = McpError::make(
              ErrorCode::MethodNotFound, "method not found: " + request.method);
        } else {
          auto result = readResource(*resources_, request, is2026, token);
          if (!result.hasValue()) {
            response.error = result.error();
          } else {
            response.result = result.value();
          }
        }
      } else if (request.method == "prompts/list") {
        if (prompts_ == nullptr) {
          response.error = McpError::make(
              ErrorCode::MethodNotFound, "method not found: " + request.method);
        } else {
          auto cursor = readCursor(request.params);
          if (!cursor.hasValue()) {
            response.error = cursor.error();
          } else {
            auto result =
                listPrompts(*prompts_, request, cursor.value(), is2026, token);
            if (!result.hasValue()) {
              response.error = result.error();
            } else {
              response.result = result.value();
            }
          }
        }
      } else {
        if (prompts_ == nullptr) {
          response.error = McpError::make(
              ErrorCode::MethodNotFound, "method not found: " + request.method);
        } else {
          auto result = getPrompt(*prompts_, request, is2026, token);
          if (!result.hasValue()) {
            response.error = result.error();
          } else {
            response.result = result.value();
          }
        }
      }
    } else {
      response.error = McpError::make(
          ErrorCode::MethodNotFound, "method not found: " + request.method);
    }
  } catch (const std::exception&) {
    response.error =
        McpError::make(ErrorCode::InternalError, "internal error");
  } catch (...) {
    response.error =
        McpError::make(ErrorCode::InternalError, "internal error");
  }

  if (notification) {
    return std::nullopt;
  }
  return response;
}

} // namespace v1
} // namespace mcp
