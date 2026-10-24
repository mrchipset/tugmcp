#include "tugmcp/client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcp {
namespace v1 {
namespace {

McpError failureWithReason(const char* message, const char* reason) {
  return McpError::make(ErrorCode::InternalError, message,
                        Json{{"reason", reason}});
}

} // namespace

struct PendingRequest {
  bool done = false;
  Json result;
  std::optional<McpError> error;
};

class McpClient::Impl {
public:
  Impl(ClientOptions options, std::shared_ptr<ITransport> transport)
      : options_(std::move(options)), transport_(std::move(transport)) {
    if (transport_) {
      transport_->setOnMessage(
          [this](Json message) { handleMessage(std::move(message)); });
      transport_->setOnError(
          [this](McpError error) { handleError(std::move(error)); });
      transport_->setOnClose([this] { handleClose(); });
    }
  }

  Status connect() {
    if (!transport_) {
      return Status::failure(
          failureWithReason("transport is null", "transport_closed"));
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return Status::failure(
            failureWithReason("transport is closed", "transport_closed"));
      }
    }

    Status started;
    try {
      started = transport_->start();
    } catch (...) {
      return Status::failure(
          failureWithReason("failed to start transport", "send_failed"));
    }
    if (!started) {
      return started;
    }

    Json params;
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"] = Json::object();
    params["clientInfo"] =
        Json{{"name", options_.name}, {"version", options_.version}};

    auto initialized = call("initialize", std::move(params));
    if (!initialized.hasValue()) {
      return Status::failure(initialized.error());
    }

    Json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "notifications/initialized";
    try {
      Status sent = transport_->send(notification);
      if (!sent) {
        return Status::failure(
            failureWithReason("failed to send initialized", "send_failed"));
      }
    } catch (...) {
      return Status::failure(
          failureWithReason("failed to send initialized", "send_failed"));
    }

    return Status::success();
  }

  Result<Json> call(std::string_view method, Json params) {
    if (!transport_) {
      return Result<Json>::failure(
          failureWithReason("transport is null", "transport_closed"));
    }

    const std::int64_t id =
        next_id_.fetch_add(1, std::memory_order_relaxed);
    RequestId request_id;
    request_id.value = id;

    auto pending = std::make_shared<PendingRequest>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return Result<Json>::failure(
            failureWithReason("transport is closed", "transport_closed"));
      }
      pending_.emplace(request_id, pending);
    }

    Json request;
    try {
      request["jsonrpc"] = "2.0";
      request["id"] = id;
      request["method"] = std::string(method);
      request["params"] = std::move(params);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(request_id);
      return Result<Json>::failure(
          failureWithReason("failed to build request", "send_failed"));
    }

    try {
      Status sent = transport_->send(request);
      if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(request_id);
        // Preserve transport-provided reasons (e.g. http_status / network_error)
        // and fall back to "send_failed" for plain transport failures.
        McpError error = sent.error;
        if (!error.data.is_object() || !error.data.contains("reason")) {
          error.data = Json{{"reason", "send_failed"}};
        }
        return Result<Json>::failure(std::move(error));
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(request_id);
      return Result<Json>::failure(
          failureWithReason("failed to send request", "send_failed"));
    }

    // All timeout arithmetic goes through the injected time source. Note that
    // condition_variable::wait_until itself still uses the real clock.
    const auto now = options_.timeSource ? options_.timeSource->now()
                                         : SystemTimeSource{}.now();
    const auto deadline =
        now + std::chrono::milliseconds(options_.timeoutMs);

    std::unique_lock<std::mutex> lock(mutex_);
    const bool signaled = cv_.wait_until(lock, deadline, [&pending, this] {
      return pending->done || closed_;
    });

    if (!pending->done) {
      pending_.erase(request_id);
      if (!signaled || !closed_) {
        return Result<Json>::failure(
            failureWithReason("request timed out", "timeout"));
      }
      return Result<Json>::failure(
          failureWithReason("transport is closed", "transport_closed"));
    }

    Json result = std::move(pending->result);
    std::optional<McpError> error = std::move(pending->error);
    pending_.erase(request_id);
    lock.unlock();

    if (error.has_value()) {
      return Result<Json>::failure(std::move(*error));
    }
    return result;
  }

  void close() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        for (auto& item : pending_) {
          auto& pending = item.second;
          if (!pending->done) {
            pending->done = true;
            pending->error =
                failureWithReason("transport is closed", "transport_closed");
          }
        }
      }
      cv_.notify_all();
    } catch (...) {
      // Never let cleanup escape.
    }

    if (!transport_) {
      return;
    }

    try {
      transport_->setOnMessage(nullptr);
      transport_->setOnError(nullptr);
      transport_->setOnClose(nullptr);
    } catch (...) {
    }

    try {
      transport_->close();
    } catch (...) {
    }
  }

private:
  void handleMessage(Json message) noexcept {
    try {
      auto decoded = decode(message.dump());
      if (!decoded.hasValue() ||
          decoded.value().kind != MessageKind::Response) {
        ignored_responses_.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      Message& response = decoded.value();
      std::shared_ptr<PendingRequest> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(response.id);
        if (it == pending_.end()) {
          ignored_responses_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        pending = it->second;
        if (pending->done) {
          return;
        }
        pending->done = true;
        if (response.error.has_value()) {
          pending->error = response.error;
        } else {
          pending->result = std::move(response.result);
        }
      }
      cv_.notify_all();
    } catch (...) {
      ignored_responses_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void handleError(McpError error) noexcept {
    // Transport errors are diagnostic for pending calls (which keep waiting for
    // a response, timeout, close, or onClose), but they must stay visible.
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      last_transport_error_ = std::move(error);
    } catch (...) {
    }
  }

  void handleClose() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        for (auto& item : pending_) {
          auto& pending = item.second;
          if (!pending->done) {
            pending->done = true;
            pending->error =
                failureWithReason("transport is closed", "transport_closed");
          }
        }
      }
      cv_.notify_all();
    } catch (...) {
    }
  }

public:
  std::optional<McpError> lastTransportError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_transport_error_;
  }

private:
  ClientOptions options_;
  std::shared_ptr<ITransport> transport_;
  std::atomic<std::int64_t> next_id_{1};
  std::atomic<std::size_t> ignored_responses_{0};
  std::optional<McpError> last_transport_error_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_ = false;
  std::unordered_map<RequestId, std::shared_ptr<PendingRequest>> pending_;
};

McpClient::McpClient(ClientOptions options,
                     std::shared_ptr<ITransport> transport)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(transport))) {}

McpClient::~McpClient() { close(); }

Status McpClient::connect() { return impl_->connect(); }

Result<Json> McpClient::call(std::string_view method, Json params) {
  return impl_->call(method, std::move(params));
}

Result<Json> McpClient::listTools() {
  return call("tools/list", Json::object());
}

Result<Json> McpClient::callTool(std::string_view name, Json arguments) {
  Json params;
  params["name"] = std::string(name);
  params["arguments"] = std::move(arguments);
  return call("tools/call", std::move(params));
}

void McpClient::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

std::optional<McpError> McpClient::lastTransportError() const {
  return impl_ ? impl_->lastTransportError() : std::nullopt;
}

} // namespace v1
} // namespace mcp
