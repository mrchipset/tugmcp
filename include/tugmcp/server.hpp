#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tugmcp/executor.hpp>
#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/schema.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

struct ServerOptions {
  std::string name = "tugmcp";
  std::string version = "0.1.0";
  std::string instructions;
  std::vector<std::string> supportedVersions{"2026-07-28",
                                               "2025-11-25"};
  std::shared_ptr<ITimeSource> timeSource;

  // Optional resource and prompt providers. At most one provider per kind is
  // supported in this phase; each provider owns its namespace and pagination.
  std::shared_ptr<IResourceProvider> resources;
  std::shared_ptr<IPromptProvider> prompts;
  std::shared_ptr<ICompletionProvider> completions;

  // Schema validation policy. With no validator and a tool that declares an
  // inputSchema, registration fails unless allowUnvalidatedTools is true.
  SchemaValidatorPtr schemaValidator;
  bool allowUnvalidatedTools = false;

  // D17: a null executor means the server creates an internal thread pool
  // (4 workers, 1024 queued tasks) and drains it on run() exit. Inject your own
  // executor to share capacity with the host. Set useInlineExecutor to keep the
  // legacy synchronous dispatch on the transport thread. Precedence: a non-null
  // executor always wins over useInlineExecutor.
  std::shared_ptr<IExecutor> executor;
  bool useInlineExecutor = false;
};

class McpServer {
public:
  McpServer(ServerOptions options, std::shared_ptr<ITransport> transport);
  ~McpServer();

  ToolRegistry& tools() noexcept;

  // Starts the transport and blocks until onClose()/stop(), then closes it.
  Status run();

  // Thread-safe and idempotent.
  void stop() noexcept;

  // Push notifications to matching 2026 subscriptions. Failures remove the
  // affected subscription; these methods never hold the registry lock while
  // calling into the transport.
  Result<Json> requestClient(std::string_view sessionId,
                             std::string_view method,
                             Json params = Json::object(),
                             std::chrono::milliseconds timeout =
                                 std::chrono::seconds(30));
  Status sendNotification(std::string_view sessionId,
                          std::string_view method,
                          Json params = Json::object());
  void notifyResourceUpdated(std::string_view uri);
  void notifyListChanged(ListKind kind);
  std::size_t activeSubscriptions() const;

  // Last transport/protocol error observed while running (test aid).
  std::optional<McpError> lastError() const;

private:
  struct SubscriptionState {
    SubscriptionId id;
    Json subscriptionIdJson;
    std::string route;
    std::vector<ListKind> listChanged;
    std::unordered_set<std::string> resources;
    std::string protocolVersion;
  };

  void removeSubscription(const SubscriptionId& id);
  void clearSubscriptions() noexcept;
  void handleRouteClosed(const std::string& route);
  void completeServerRequests(const McpError& error) noexcept;
  void handleSessionClosed(const std::string& sessionId);
  struct PendingClientResponse {
    std::string sessionId;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    Json result;
    std::optional<McpError> error;
  };

  // transport_ is declared first so it is destroyed last.
  std::shared_ptr<ITransport> transport_;
  ServerOptions options_;
  ToolRegistry tools_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::size_t in_flight_ = 0;
  std::optional<McpError> last_error_;
  std::unordered_map<std::string, std::shared_ptr<CancellationToken>>
      cancellation_tokens_;

  mutable std::mutex subscriptions_mutex_;
  std::unordered_map<SubscriptionId, SubscriptionState> subscriptions_;
  std::atomic<std::uint64_t> next_subscription_id_{1};
  std::atomic<std::int64_t> next_server_request_id_{1};
  std::mutex server_requests_mutex_;
  std::unordered_map<RequestId, std::shared_ptr<PendingClientResponse>,
                     std::hash<RequestId>>
      server_requests_;
};

} // namespace v1
} // namespace mcp
