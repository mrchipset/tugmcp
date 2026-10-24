#include "tugmcp/client.hpp"

#include <algorithm>
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
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace mcp {
namespace v1 {
namespace {

McpError failureWithReason(const char* message, const char* reason) {
  return McpError::make(ErrorCode::InternalError, message,
                        Json{{"reason", reason}});
}

const char* listKindName(ListKind kind) {
  switch (kind) {
  case ListKind::Tools:
    return "tools";
  case ListKind::Prompts:
    return "prompts";
  case ListKind::Resources:
    return "resources";
  }
  return "invalid";
}

std::chrono::steady_clock::time_point currentTime(
    const ClientOptions& options) {
  if (options.timeSource) {
    try {
      return options.timeSource->now();
    } catch (...) {
    }
  }
  return std::chrono::steady_clock::now();
}

} // namespace

struct PendingRequest {
  // Guards done/result/error/callbacks. The owning call waits on cv.
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  Json result;
  std::optional<McpError> error;
  std::shared_ptr<CancellationToken> cancellation;
  std::optional<ProgressToken> progress_token;
  std::function<void(const Progress&)> on_progress;
  std::function<void(const Json&)> on_notification;
};

struct SubscriptionHandshake {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool stream_ended = false;
  SubscriptionId id;
  std::optional<McpError> error;
};

struct ClientSubscription {
  RequestId request_id;
  std::function<void(const Json&)> callback;
  std::shared_ptr<std::atomic<bool>> closed{
      std::make_shared<std::atomic<bool>>(false)};
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
      if (closed_.load(std::memory_order_acquire)) {
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

    if (options_.protocolVersion == "2026-07-28") {
      auto discovered = discover();
      if (discovered.hasValue()) {
        return Status::success();
      }
      const ErrorCode code = discovered.error().code;
      const bool fallback_eligible =
          code == ErrorCode::UnsupportedVersion ||
          code == ErrorCode::MethodNotFound ||
          code == ErrorCode::InvalidParams;
      if (!options_.allow2025Fallback || !fallback_eligible) {
        return Status::failure(discovered.error());
      }
    }

    return initialize();
  }

  Status initialize() {
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

  Result<Json> discover() {
    return call("server/discover", Json::object());
  }

  Result<Json> call(std::string_view method, Json params,
                    const CallOptions& call_options = {}) {
    if (!transport_) {
      return Result<Json>::failure(
          failureWithReason("transport is null", "transport_closed"));
    }

    // Reclaim sender threads from earlier aborted calls.
    reapBackgroundSenders();

    Json call_params = std::move(params);
    try {
      if (call_options.progressToken.has_value()) {
        if (!call_params.is_object()) {
          return Result<Json>::failure(McpError::make(
              ErrorCode::InvalidParams,
              "params must be an object to use progressToken"));
        }
        if (!call_params.contains("_meta") ||
            !call_params.at("_meta").is_object()) {
          call_params["_meta"] = Json::object();
        }
        call_params["_meta"]["progressToken"] =
            progressTokenToJson(*call_options.progressToken);
      }
      if (call_options.requestState.has_value()) {
        if (!call_params.is_object()) {
          return Result<Json>::failure(McpError::make(
              ErrorCode::InvalidParams,
              "params must be an object to use requestState"));
        }
        call_params["requestState"] = *call_options.requestState;
      }
      if (!call_options.inputResponses.is_null()) {
        if (!call_params.is_object()) {
          return Result<Json>::failure(McpError::make(
              ErrorCode::InvalidParams,
              "params must be an object to use inputResponses"));
        }
        call_params["inputResponses"] = call_options.inputResponses;
      }
    } catch (...) {
      return Result<Json>::failure(
          failureWithReason("failed to build params", "send_failed"));
    }

    const std::int64_t id =
        next_id_.fetch_add(1, std::memory_order_relaxed);
    RequestId request_id;
    request_id.value = id;

    if (call_options.out_id != nullptr) {
      *call_options.out_id = request_id;
    }

    auto pending = std::make_shared<PendingRequest>();
    pending->cancellation = call_options.cancellation;
    pending->progress_token = call_options.progressToken;
    pending->on_progress = call_options.onProgress;
    pending->on_notification = call_options.onNotification;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_.load(std::memory_order_acquire)) {
        return Result<Json>::failure(
            failureWithReason("transport is closed", "transport_closed"));
      }
      if (call_options.progressToken.has_value()) {
        for (const auto& item : pending_) {
          if (item.second->progress_token.has_value() &&
              *item.second->progress_token == *call_options.progressToken) {
            return Result<Json>::failure(McpError::make(
                ErrorCode::InvalidParams, "progressToken already in use"));
          }
        }
      }
      pending_.emplace(request_id, pending);
    }

    Json request;
    try {
      request["jsonrpc"] = "2.0";
      request["id"] = id;
      request["method"] = std::string(method);
      request["params"] = std::move(call_params);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(request_id);
      return Result<Json>::failure(
          failureWithReason("failed to build request", "send_failed"));
    }

    const std::shared_ptr<ITransport> transport = transport_;
    std::thread sender;
    try {
      sender = std::thread([transport, pending,
                            request = std::move(request)]() mutable {
        Status sent;
        try {
          sent = transport->send(request);
        } catch (...) {
          sent = Status::failure(
              failureWithReason("failed to send request", "send_failed"));
        }
        if (sent) {
          return;
        }
        McpError error = sent.error;
        if (!error.data.is_object() || !error.data.contains("reason")) {
          error.data = Json{{"reason", "send_failed"}};
        }
        // The owning call removes the entry; only publish completion here.
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (!pending->done) {
          pending->done = true;
          pending->error = std::move(error);
        }
        pending->cv.notify_all();
      });
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(request_id);
      return Result<Json>::failure(
          failureWithReason("failed to start sender", "send_failed"));
    }

    const auto timeout = call_options.timeout.count() > 0
                             ? call_options.timeout
                             : std::chrono::milliseconds(options_.timeoutMs);
    const auto deadline = currentTime(options_) + timeout;

    bool timed_out = false;
    bool cancelled = false;

    {
      std::unique_lock<std::mutex> lock(pending->mutex);
      for (;;) {
        if (pending->done) {
          break;
        }
        if (closed_.load(std::memory_order_acquire)) {
          break;
        }
        if (call_options.cancellation &&
            call_options.cancellation->cancelled()) {
          cancelled = true;
          break;
        }
        const auto now = currentTime(options_);
        if (now >= deadline) {
          timed_out = true;
          break;
        }
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now);
        const auto slice = std::min(
            remaining, std::chrono::milliseconds(20));
        if (slice.count() <= 0) {
          timed_out = true;
          break;
        }
        pending->cv.wait_for(lock, slice);
      }

      if (pending->done) {
        Json result = std::move(pending->result);
        std::optional<McpError> error = std::move(pending->error);
        lock.unlock();
        {
          std::lock_guard<std::mutex> registry_lock(mutex_);
          pending_.erase(request_id);
        }
        const bool aborted =
            error.has_value() && error->data.is_object() &&
            error->data.contains("reason") &&
            error->data.at("reason").is_string() &&
            (error->data.at("reason") == "cancelled" ||
             error->data.at("reason") == "timeout");
        if (aborted) {
          parkSender(sender);
        } else {
          joinSender(sender);
        }
        if (error.has_value()) {
          return Result<Json>::failure(std::move(*error));
        }
        return result;
      }
    }

    {
      std::lock_guard<std::mutex> registry_lock(mutex_);
      pending_.erase(request_id);
    }

    if (cancelled) {
      cancelTransport(request_id);
      parkSender(sender);
      return Result<Json>::failure(
          failureWithReason("request cancelled", "cancelled"));
    }

    if (timed_out) {
      if (pending->cancellation) {
        pending->cancellation->cancel();
      }
      cancelTransport(request_id);
      parkSender(sender);
      return Result<Json>::failure(
          failureWithReason("request timed out", "timeout"));
    }

    parkSender(sender);
    return Result<Json>::failure(
        failureWithReason("transport is closed", "transport_closed"));
  }

  Result<SubscriptionId> subscribe(
      const SubscriptionFilter& filter,
      std::function<void(const Json&)> onNotification) {
    if (!transport_) {
      return Result<SubscriptionId>::failure(
          failureWithReason("transport is null", "transport_closed"));
    }

    Json subscriptions = Json::object();
    if (!filter.listChanged.empty()) {
      Json kinds = Json::array();
      for (const ListKind kind : filter.listChanged) {
        kinds.push_back(listKindName(kind));
      }
      subscriptions["listChanged"] = std::move(kinds);
    }
    if (!filter.resources.empty()) {
      Json resources = Json::array();
      for (const auto& uri : filter.resources) {
        resources.push_back(uri);
      }
      subscriptions["resources"] = std::move(resources);
    }

    Json params = Json::object();
    if (!subscriptions.empty()) {
      params["subscriptions"] = std::move(subscriptions);
    }

    const std::int64_t id =
        next_id_.fetch_add(1, std::memory_order_relaxed);
    RequestId request_id;
    request_id.value = id;

    auto handshake = std::make_shared<SubscriptionHandshake>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_.load(std::memory_order_acquire)) {
        return Result<SubscriptionId>::failure(
            failureWithReason("transport is closed", "transport_closed"));
      }
      subscription_handshakes_.emplace(request_id, handshake);
    }

    Json request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = "subscriptions/listen";
    request["params"] = std::move(params);

    auto closed_flag = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<ITransport> transport = transport_;
    std::thread sender([transport, handshake, closed_flag,
                        request = std::move(request)]() mutable {
      Status sent;
      try {
        sent = transport->send(request);
      } catch (...) {
        sent = Status::failure(
            failureWithReason("subscription send failed", "send_failed"));
      }

      {
        std::lock_guard<std::mutex> lock(handshake->mutex);
        handshake->stream_ended = true;
        if (!handshake->done) {
          handshake->done = true;
          handshake->error =
              sent ? failureWithReason("subscription stream ended",
                                       "transport_closed")
                   : sent.error;
        }
        handshake->cv.notify_all();
      }
      closed_flag->store(true, std::memory_order_release);
    });
    try {
      parkSubscriptionSender(sender);
    } catch (...) {
      if (sender.joinable()) {
        sender.detach();
      }
    }

    const auto timeout = std::chrono::milliseconds(options_.timeoutMs);
    const auto deadline = currentTime(options_) + timeout;
    bool timed_out = false;
    bool was_closed = false;
    bool stream_ended = false;
    SubscriptionId subscription_id;
    std::optional<McpError> error;
    {
      std::unique_lock<std::mutex> lock(handshake->mutex);
      for (;;) {
        if (handshake->done) {
          break;
        }
        if (closed_.load(std::memory_order_acquire)) {
          was_closed = true;
          break;
        }
        const auto now = currentTime(options_);
        if (now >= deadline) {
          timed_out = true;
          break;
        }
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now);
        const auto slice =
            std::min(remaining, std::chrono::milliseconds(20));
        if (slice.count() <= 0) {
          timed_out = true;
          break;
        }
        handshake->cv.wait_for(lock, slice);
      }
      subscription_id = handshake->id;
      error = handshake->error;
      stream_ended = handshake->stream_ended;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      subscription_handshakes_.erase(request_id);
    }

    if (timed_out) {
      cancelTransport(request_id);
      return Result<SubscriptionId>::failure(
          failureWithReason("subscription timed out", "timeout"));
    }
    if (was_closed) {
      return Result<SubscriptionId>::failure(
          failureWithReason("transport is closed", "transport_closed"));
    }
    if (error.has_value()) {
      return Result<SubscriptionId>::failure(std::move(*error));
    }
    if (subscription_id.empty()) {
      cancelTransport(request_id);
      return Result<SubscriptionId>::failure(McpError::make(
          ErrorCode::InvalidParams,
          "subscription acknowledgement missing subscriptionId"));
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      ClientSubscription subscription;
      subscription.request_id = request_id;
      subscription.callback = std::move(onNotification);
      subscription.closed = closed_flag;
      subscription.closed->store(
          stream_ended || closed_.load(std::memory_order_acquire),
          std::memory_order_release);
      subscriptions_[subscription_id] = std::move(subscription);
    }
    return subscription_id;
  }

  Status unsubscribe(const SubscriptionId& id) {
    RequestId request_id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = subscriptions_.find(id);
      if (it == subscriptions_.end()) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "unknown subscription",
            Json{{"reason", "unknown_subscription"}}));
      }
      request_id = it->second.request_id;
      it->second.closed->store(true, std::memory_order_release);
      subscriptions_.erase(it);
    }
    cancelTransport(request_id);
    return Status::success();
  }

  Status cancel(const RequestId& id, CancelMode mode) {
    std::shared_ptr<PendingRequest> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = pending_.find(id);
      if (it == pending_.end()) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "unknown request",
            Json{{"reason", "unknown_request"}}));
      }
      pending = it->second;
    }

    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->done) {
        return Status::success();
      }
      pending->done = true;
      pending->error = failureWithReason("request cancelled", "cancelled");
    }
    pending->cv.notify_all();

    if (pending->cancellation) {
      pending->cancellation->cancel();
    }
    const bool close_stream =
        mode == CancelMode::CloseStream ||
        (mode == CancelMode::Auto && options_.protocolVersion == "2026-07-28");
    if (close_stream) {
      cancelTransport(id);
    }
    return Status::success();
  }

  void close() noexcept {
    std::vector<std::pair<RequestId, std::shared_ptr<PendingRequest>>> entries;
    std::vector<std::shared_ptr<SubscriptionHandshake>> handshakes;
    std::vector<RequestId> subscription_ids;
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_.store(true, std::memory_order_release);
        for (const auto& item : pending_) {
          entries.emplace_back(item.first, item.second);
        }
        for (const auto& item : subscription_handshakes_) {
          handshakes.push_back(item.second);
        }
        for (const auto& item : subscriptions_) {
          subscription_ids.push_back(item.second.request_id);
        }
        subscription_handshakes_.clear();
        subscriptions_.clear();
      }
      for (const auto& handshake : handshakes) {
        {
          std::lock_guard<std::mutex> lock(handshake->mutex);
          if (handshake->done) {
            continue;
          }
          handshake->done = true;
          handshake->error =
              failureWithReason("transport is closed", "transport_closed");
        }
        handshake->cv.notify_all();
      }
      for (const auto& entry : entries) {
        const auto& pending = entry.second;
        {
          std::lock_guard<std::mutex> lock(pending->mutex);
          if (pending->done) {
            continue;
          }
          pending->done = true;
          pending->error =
              failureWithReason("transport is closed", "transport_closed");
        }
        pending->cv.notify_all();
        if (pending->cancellation) {
          pending->cancellation->cancel();
        }
      }
    } catch (...) {
      // Never let cleanup escape.
    }
    std::vector<RequestId> ids;
    ids.reserve(entries.size() + subscription_ids.size());
    for (const auto& entry : entries) {
      ids.push_back(entry.first);
    }
    for (const auto& id : subscription_ids) {
      ids.push_back(id);
    }

    if (!transport_) {
      return;
    }

    for (const auto& id : ids) {
      try {
        transport_->cancelPending(id);
      } catch (...) {
      }
    }

    reapBackgroundSenders();
    detachSubscriptionSenders();

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
  static void joinSender(std::thread& sender) noexcept {
    try {
      if (sender.joinable()) {
        sender.join();
      }
    } catch (...) {
    }
  }

  void parkSender(std::thread& sender) noexcept {
    try {
      if (!sender.joinable()) {
        return;
      }
      std::lock_guard<std::mutex> lock(senders_mutex_);
      background_senders_.push_back(std::move(sender));
    } catch (...) {
      try {
        if (sender.joinable()) {
          sender.detach();
        }
      } catch (...) {
      }
    }
  }

  void parkSubscriptionSender(std::thread& sender) {
    if (!sender.joinable()) {
      return;
    }
    std::lock_guard<std::mutex> lock(subscription_senders_mutex_);
    subscription_senders_.push_back(std::move(sender));
  }

  void detachSubscriptionSenders() noexcept {
    std::vector<std::thread> local;
    try {
      std::lock_guard<std::mutex> lock(subscription_senders_mutex_);
      local.swap(subscription_senders_);
    } catch (...) {
      return;
    }
    for (auto& sender : local) {
      try {
        if (sender.joinable()) {
          sender.detach();
        }
      } catch (...) {
      }
    }
  }

  void reapBackgroundSenders() noexcept {
    std::vector<std::thread> local;
    try {
      std::lock_guard<std::mutex> lock(senders_mutex_);
      local.swap(background_senders_);
    } catch (...) {
      return;
    }
    for (auto& sender : local) {
      joinSender(sender);
    }
  }

  void cancelTransport(const RequestId& id) noexcept {
    if (options_.protocolVersion != "2026-07-28" || !transport_) {
      return;
    }
    try {
      transport_->cancelPending(id);
    } catch (...) {
    }
  }

  void handleMessage(Json message) noexcept {
    try {
      auto decoded = decode(message);
      if (!decoded.hasValue()) {
        ignored_responses_.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      Message& decoded_message = decoded.value();
      if (decoded_message.kind == MessageKind::Notification) {
        handleNotification(decoded_message, message);
        return;
      }
      if (decoded_message.kind != MessageKind::Response) {
        ignored_responses_.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      std::shared_ptr<PendingRequest> pending;
      std::shared_ptr<SubscriptionHandshake> handshake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(decoded_message.id);
        if (it != pending_.end()) {
          pending = it->second;
        } else {
          const auto hs_it =
              subscription_handshakes_.find(decoded_message.id);
          if (hs_it != subscription_handshakes_.end()) {
            handshake = hs_it->second;
          }
        }
      }
      if (!pending) {
        if (!handshake) {
          ignored_responses_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        {
          std::lock_guard<std::mutex> lock(handshake->mutex);
          if (handshake->done) {
            return;
          }
          handshake->done = true;
          handshake->error = decoded_message.error.has_value()
                                 ? decoded_message.error
                                 : McpError::make(
                                       ErrorCode::InvalidParams,
                                       "subscription was not acknowledged");
        }
        handshake->cv.notify_all();
        return;
      }
      {
        std::lock_guard<std::mutex> pending_lock(pending->mutex);
        if (pending->done) {
          return;
        }
        pending->done = true;
        if (decoded_message.error.has_value()) {
          pending->error = decoded_message.error;
        } else {
          pending->result = std::move(decoded_message.result);
        }
      }
      pending->cv.notify_all();
    } catch (...) {
      ignored_responses_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void handleSubscriptionAck(const Json& params) noexcept {
    try {
      if (!params.is_object() || !params.contains("subscriptionId") ||
          !params.at("subscriptionId").is_string() ||
          !params.contains("requestId")) {
        return;
      }
      const SubscriptionId subscription_id =
          params.at("subscriptionId").get<SubscriptionId>();
      auto request_id = RequestId::fromJson(params.at("requestId"));
      if (!request_id.hasValue()) {
        return;
      }

      std::shared_ptr<SubscriptionHandshake> handshake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it =
            subscription_handshakes_.find(request_id.value());
        if (it == subscription_handshakes_.end()) {
          return;
        }
        handshake = it->second;
      }

      {
        std::lock_guard<std::mutex> lock(handshake->mutex);
        if (handshake->done) {
          return;
        }
        handshake->id = subscription_id;
        handshake->done = true;
        handshake->cv.notify_all();
      }
    } catch (...) {
    }
  }

  void handleSubscriptionEvent(const Json& raw,
                               const SubscriptionId& id) noexcept {
    std::function<void(const Json&)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = subscriptions_.find(id);
      if (it == subscriptions_.end() ||
          it->second.closed->load(std::memory_order_acquire)) {
        return;
      }
      callback = it->second.callback;
    }
    if (!callback) {
      return;
    }
    try {
      callback(raw);
    } catch (...) {
    }
  }

  void handleNotification(const Message& notification,
                          const Json& raw) noexcept {
    try {
      if (notification.method ==
          "notifications/subscriptions/acknowledged") {
        handleSubscriptionAck(notification.params);
        return;
      }
      if (notification.params.is_object() &&
          notification.params.contains("subscriptionId") &&
          notification.params.at("subscriptionId").is_string()) {
        handleSubscriptionEvent(
            raw,
            notification.params.at("subscriptionId").get<SubscriptionId>());
        return;
      }
      if (!notification.params.is_object()) {
        return;
      }
      const Json& params = notification.params;
      if (!params.contains("progressToken")) {
        return;
      }
      auto token = progressTokenFromJson(params.at("progressToken"));
      if (!token.hasValue()) {
        return;
      }

      std::shared_ptr<PendingRequest> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : pending_) {
          if (item.second->progress_token.has_value() &&
              *item.second->progress_token == token.value()) {
            pending = item.second;
            break;
          }
        }
      }
      if (!pending) {
        return;
      }

      if (notification.method == "notifications/progress") {
        std::function<void(const Progress&)> callback;
        {
          std::lock_guard<std::mutex> pending_lock(pending->mutex);
          callback = pending->on_progress;
        }
        if (!callback) {
          return;
        }
        Progress progress;
        progress.token = token.value();
        progress.progress = params.value("progress", 0.0);
        if (params.contains("total") && params.at("total").is_number()) {
          progress.total = params.at("total").get<double>();
        }
        if (params.contains("message") && params.at("message").is_string()) {
          progress.message = params.at("message").get<std::string>();
        }
        try {
          callback(progress);
        } catch (...) {
        }
        return;
      }

      std::function<void(const Json&)> callback;
      {
        std::lock_guard<std::mutex> pending_lock(pending->mutex);
        callback = pending->on_notification;
      }
      if (!callback) {
        return;
      }
      try {
        callback(raw);
      } catch (...) {
      }
    } catch (...) {
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
      std::vector<std::shared_ptr<PendingRequest>> entries;
      std::vector<std::shared_ptr<SubscriptionHandshake>> handshakes;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_.store(true, std::memory_order_release);
        for (const auto& item : pending_) {
          entries.push_back(item.second);
        }
        for (const auto& item : subscription_handshakes_) {
          handshakes.push_back(item.second);
        }
        for (auto& item : subscriptions_) {
          item.second.closed->store(true, std::memory_order_release);
        }
      }
      for (const auto& pending : entries) {
        {
          std::lock_guard<std::mutex> lock(pending->mutex);
          if (pending->done) {
            continue;
          }
          pending->done = true;
          pending->error =
              failureWithReason("transport is closed", "transport_closed");
        }
        pending->cv.notify_all();
      }
      for (const auto& handshake : handshakes) {
        {
          std::lock_guard<std::mutex> lock(handshake->mutex);
          if (handshake->done) {
            continue;
          }
          handshake->done = true;
          handshake->error =
              failureWithReason("transport is closed", "transport_closed");
        }
        handshake->cv.notify_all();
      }
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
  std::atomic<bool> closed_{false};
  std::unordered_map<RequestId, std::shared_ptr<PendingRequest>> pending_;

  std::mutex senders_mutex_;
  std::vector<std::thread> background_senders_;

  std::unordered_map<RequestId, std::shared_ptr<SubscriptionHandshake>>
      subscription_handshakes_;
  std::unordered_map<SubscriptionId, ClientSubscription> subscriptions_;

  std::mutex subscription_senders_mutex_;
  std::vector<std::thread> subscription_senders_;
};

McpClient::McpClient(ClientOptions options,
                     std::shared_ptr<ITransport> transport)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(transport))) {}

McpClient::~McpClient() { close(); }

Status McpClient::connect() { return impl_->connect(); }

Status McpClient::initialize() { return impl_->initialize(); }

Result<Json> McpClient::discover() { return impl_->discover(); }

Result<Json> McpClient::call(std::string_view method, Json params,
                             const CallOptions& options) {
  return impl_->call(method, std::move(params), options);
}

Result<Json> McpClient::listTools(std::string_view cursor) {
  Json params = Json::object();
  if (!cursor.empty()) {
    params["cursor"] = std::string(cursor);
  }
  return call("tools/list", std::move(params));
}

Result<Json> McpClient::callTool(std::string_view name, Json arguments,
                                 const CallOptions& options) {
  Json params;
  params["name"] = std::string(name);
  params["arguments"] = std::move(arguments);
  return call("tools/call", std::move(params), options);
}

Result<Json> McpClient::listResources(std::string_view cursor) {
  Json params = Json::object();
  if (!cursor.empty()) {
    params["cursor"] = std::string(cursor);
  }
  return call("resources/list", std::move(params));
}

Result<Json> McpClient::listResourceTemplates(std::string_view cursor) {
  Json params = Json::object();
  if (!cursor.empty()) {
    params["cursor"] = std::string(cursor);
  }
  return call("resources/templates/list", std::move(params));
}

Result<Json> McpClient::readResource(std::string_view uri,
                                     const CallOptions& options) {
  Json params;
  params["uri"] = std::string(uri);
  return call("resources/read", std::move(params), options);
}

Result<Json> McpClient::listPrompts(std::string_view cursor) {
  Json params = Json::object();
  if (!cursor.empty()) {
    params["cursor"] = std::string(cursor);
  }
  return call("prompts/list", std::move(params));
}

Result<Json> McpClient::getPrompt(std::string_view name, Json arguments,
                                  const CallOptions& options) {
  Json params;
  params["name"] = std::string(name);
  params["arguments"] = std::move(arguments);
  return call("prompts/get", std::move(params), options);
}

Result<SubscriptionId> McpClient::subscribe(
    const SubscriptionFilter& filter,
    std::function<void(const Json&)> onNotification) {
  return impl_->subscribe(filter, std::move(onNotification));
}

Status McpClient::unsubscribe(const SubscriptionId& id) {
  return impl_->unsubscribe(id);
}

Status McpClient::cancel(const RequestId& id, CancelMode mode) {
  return impl_->cancel(id, mode);
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
