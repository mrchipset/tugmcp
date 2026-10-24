#include "dispatcher.hpp"

#include <exception>
#include <mutex>
#include <string>
#include <utility>

#include <tugmcp/message.hpp>
#include <tugmcp/request_context.hpp>
#include <tugmcp/server.hpp>

namespace mcp {
namespace v1 {
namespace {

Json serverInfo(const DispatchConfig& config) {
  Json info;
  info["name"] = config.serverName;
  info["version"] = config.serverVersion;
  return info;
}

Json capabilities() {
  Json value;
  value["tools"] = Json::object();
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
  if (!descriptor.annotations.is_null()) {
    value["annotations"] = descriptor.annotations;
  }
  if (descriptor.idempotent) {
    value["idempotent"] = true;
  }
  return value;
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

Result<Json> toolResultToJson(const ToolResult& result) {
  Json content = Json::array();
  for (const auto& block : result.content) {
    Json item;
    item["type"] = block.type;
    item["text"] = block.text;
    content.push_back(std::move(item));
  }

  Json value;
  value["content"] = std::move(content);
  if (!result.structuredContent.is_null()) {
    value["structuredContent"] = result.structuredContent;
  }
  if (result.isError) {
    value["isError"] = true;
  }
  return value;
}

Result<Json> callTool(ToolRegistry& registry, const Message& request) {
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

  CancellationToken token;
  RequestContext context{request.id, request.method, &token};

  try {
    auto called = handler->call(arguments, context);
    if (!called.hasValue()) {
      return Result<Json>::failure(called.error());
    }

    ToolResult result = called.value();
    const auto descriptor = registry.find(name);
    if (descriptor && !descriptor->outputSchema.is_null()) {
      if (result.structuredContent.is_null()) {
        result.isError = true;
        result.content.push_back(ContentBlock{
            "text",
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
          result.content.push_back(ContentBlock{"text", std::move(note)});
        }
      }
    }
    return toolResultToJson(result);
  } catch (const std::exception& error) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError,
        std::string("tool handler failed: ") + error.what()));
  } catch (...) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "tool handler failed"));
  }
}

} // namespace

Dispatcher::Dispatcher(ToolRegistry& registry, DispatchConfig config)
    : registry_(registry), config_(std::move(config)) {}

std::optional<Message> Dispatcher::dispatch(const Message& request) {
  if (request.kind == MessageKind::Response) {
    return std::nullopt;
  }

  const bool notification = request.kind == MessageKind::Notification;

  Message response;
  response.kind = MessageKind::Response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  try {
    if (request.method == "initialize") {
      Json result;
      result["protocolVersion"] = "2025-11-25";
      result["capabilities"] = capabilities();
      result["serverInfo"] = serverInfo(config_);
      response.result = std::move(result);
    } else if (request.method == "server/discover") {
      Json result;
      result["supportedVersions"] = Json::array({"2026-07-28", "2025-11-25"});
      result["capabilities"] = capabilities();
      result["serverInfo"] = serverInfo(config_);
      response.result = std::move(result);
    } else if (request.method == "ping") {
      response.result = Json::object();
    } else if (request.method == "tools/list") {
      Json tools = Json::array();
      for (const auto& descriptor : registry_.list()) {
        tools.push_back(descriptorToJson(descriptor));
      }
      Json result;
      result["tools"] = std::move(tools);
      response.result = std::move(result);
    } else if (request.method == "tools/call") {
      auto called = callTool(registry_, request);
      if (!called.hasValue()) {
        response.error = called.error();
      } else {
        response.result = called.value();
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

McpServer::McpServer(ServerOptions options,
                     std::shared_ptr<ITransport> transport)
    : transport_(std::move(transport)), options_(std::move(options)) {
  tools_.setValidationPolicy(options_.schemaValidator,
                             options_.allowUnvalidatedTools);
}

McpServer::~McpServer() {
  if (transport_) {
    transport_->close();
  }
}

ToolRegistry& McpServer::tools() noexcept { return tools_; }

Status McpServer::run() {
  if (!transport_) {
    return Status::failure(McpError::make(
        ErrorCode::InternalError, "transport must not be null"));
  }

  Dispatcher dispatcher(
      tools_, DispatchConfig{options_.name, options_.version});

  auto record_error = [this](McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
  };

  auto clear_callbacks = [this] {
    transport_->setOnMessage(nullptr);
    transport_->setOnError(nullptr);
    transport_->setOnClose(nullptr);
  };

  transport_->setOnMessage([this, &dispatcher, record_error](Json incoming) {
    try {
      auto decoded = decode(incoming.dump());
      if (!decoded.hasValue()) {
        record_error(decoded.error());
        return;
      }
      if (decoded.value().kind == MessageKind::Response) {
        return;
      }

      auto response = dispatcher.dispatch(decoded.value());
      if (!response.has_value()) {
        return;
      }

      auto encoded = encode(*response);
      if (!encoded.hasValue()) {
        record_error(encoded.error());
        return;
      }

      Json wire = Json::parse(encoded.value());
      Status sent = transport_->send(wire);
      if (!sent) {
        record_error(sent.error);
      }
    } catch (const std::exception& error) {
      record_error(McpError::make(
          ErrorCode::InternalError,
          std::string("failed to handle message: ") + error.what()));
    } catch (...) {
      record_error(McpError::make(ErrorCode::InternalError,
                                  "failed to handle message"));
    }
  });

  transport_->setOnError(
      [record_error](McpError error) { record_error(std::move(error)); });

  transport_->setOnClose([this] {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
  });

  Status started = transport_->start();
  if (!started) {
    transport_->close();
    clear_callbacks();
    return started;
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stopping_; });
  }

  transport_->close();
  clear_callbacks();
  return Status::success();
}

void McpServer::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
}

std::optional<McpError> McpServer::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

} // namespace v1
} // namespace mcp
