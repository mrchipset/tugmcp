#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <tugmcp/message.hpp>
#include <tugmcp/operation.hpp>
#include <tugmcp/request_context.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

enum class CancelMode { Auto, CloseStream, Notification };

struct ClientOptions {
  std::string name = "tugmcp-client";
  std::string version = "0.1.0";
  std::uint32_t timeoutMs = 30000;
  std::string protocolVersion = "2026-07-28";
  bool allow2025Fallback = true;
  std::shared_ptr<ITimeSource> timeSource;
};

struct CallOptions {
  // Zero means ClientOptions::timeoutMs.
  std::chrono::milliseconds timeout{0};
  std::optional<ProgressToken> progressToken;
  std::shared_ptr<CancellationToken> cancellation;
  std::optional<std::string> requestState; // MRTR retry passthrough
  Json inputResponses;
  std::function<void(const Progress&)> onProgress;
  std::function<void(const Json&)> onNotification;
  // Optional diagnostic out-parameter: receives the JSON-RPC request id before
  // the request is sent, so callers can target cancel() at this call.
  RequestId* out_id = nullptr;
};

class McpClient {
public:
  McpClient(ClientOptions options, std::shared_ptr<ITransport> transport);
  ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  Status connect();
  Status initialize();
  Result<Json> discover();
  Result<Json> call(std::string_view method, Json params = Json::object(),
                    const CallOptions& options = {});
  Result<Json> listTools(std::string_view cursor = {});
  Result<Json> callTool(std::string_view name, Json arguments = Json::object(),
                        const CallOptions& options = {});
  Result<Json> listResources(std::string_view cursor = {});
  Result<Json> listResourceTemplates(std::string_view cursor = {});
  Result<Json> readResource(std::string_view uri,
                            const CallOptions& options = {});
  Result<Json> listPrompts(std::string_view cursor = {});
  Result<Json> getPrompt(std::string_view name,
                         Json arguments = Json::object(),
                         const CallOptions& options = {});
  Result<SubscriptionId> subscribe(
      const SubscriptionFilter& filter,
      std::function<void(const Json&)> onNotification);
  Status unsubscribe(const SubscriptionId& id);
  Status cancel(const RequestId& id, CancelMode mode = CancelMode::Auto);
  void close() noexcept;

  // Last transport-level error observed (diagnostic; pending calls keep
  // waiting for a response, timeout, close, or onClose).
  std::optional<McpError> lastTransportError() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace v1
} // namespace mcp
