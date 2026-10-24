#include "tugmcp/server.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tugmcp/message.hpp>

#include "dispatcher.hpp"

namespace mcp {
namespace v1 {
namespace {

const char* listKindName(ListKind kind) {
  switch (kind) {
  case ListKind::Tools:
    return "tools";
  case ListKind::Prompts:
    return "prompts";
  case ListKind::Resources:
    return "resources";
  }
  return "tools";
}

const char* listChangedMethod(ListKind kind) {
  switch (kind) {
  case ListKind::Tools:
    return "notifications/tools/list_changed";
  case ListKind::Prompts:
    return "notifications/prompts/list_changed";
  case ListKind::Resources:
    return "notifications/resources/list_changed";
  }
  return "notifications/tools/list_changed";
}

Result<SubscriptionFilter> parseSubscriptionFilter(const Json& params) {
  SubscriptionFilter filter;
  if (params.is_null()) {
    return filter;
  }
  if (!params.is_object()) {
    return Result<SubscriptionFilter>::failure(McpError::make(
        ErrorCode::InvalidParams, "subscriptions/listen params must be an object"));
  }
  if (!params.contains("subscriptions")) {
    return filter;
  }
  const Json& subscriptions = params.at("subscriptions");
  if (!subscriptions.is_object()) {
    return Result<SubscriptionFilter>::failure(McpError::make(
        ErrorCode::InvalidParams, "subscriptions must be an object"));
  }

  if (subscriptions.contains("listChanged")) {
    const Json& list_changed = subscriptions.at("listChanged");
    if (!list_changed.is_array()) {
      return Result<SubscriptionFilter>::failure(McpError::make(
          ErrorCode::InvalidParams, "listChanged must be an array"));
    }
    for (const auto& item : list_changed) {
      if (!item.is_string()) {
        return Result<SubscriptionFilter>::failure(McpError::make(
            ErrorCode::InvalidParams,
            "listChanged values must be strings"));
      }
      const std::string value = item.get<std::string>();
      if (value == "tools") {
        filter.listChanged.push_back(ListKind::Tools);
      } else if (value == "prompts") {
        filter.listChanged.push_back(ListKind::Prompts);
      } else if (value == "resources") {
        filter.listChanged.push_back(ListKind::Resources);
      } else {
        return Result<SubscriptionFilter>::failure(McpError::make(
            ErrorCode::InvalidParams,
            "invalid listChanged value: " + value));
      }
    }
  }

  if (subscriptions.contains("resources")) {
    const Json& resources = subscriptions.at("resources");
    if (!resources.is_array()) {
      return Result<SubscriptionFilter>::failure(McpError::make(
          ErrorCode::InvalidParams, "resources must be an array"));
    }
    for (const auto& item : resources) {
      if (!item.is_string()) {
        return Result<SubscriptionFilter>::failure(McpError::make(
            ErrorCode::InvalidParams, "resources values must be strings"));
      }
      filter.resources.push_back(item.get<std::string>());
    }
  }

  return filter;
}

} // namespace

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

  DispatchConfig config;
  config.serverName = options_.name;
  config.serverVersion = options_.version;
  config.instructions = options_.instructions;
  config.supportsSubscriptions = transport_->supportsStreaming();
  // Declared before the cleanup guard so queued tasks always see a live
  // dispatcher: guard cleanup drains the executor before this object dies.
  Dispatcher dispatcher(tools_, options_.resources.get(),
                        options_.prompts.get(), std::move(config));

  bool cleaned = false;
  bool did_start = false;
  auto cleanup = [this, &cleaned, &did_start]() noexcept {
    if (cleaned) {
      return;
    }
    cleaned = true;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    } catch (...) {
    }
    cv_.notify_all();
    try {
      if (did_start && options_.executor) {
        options_.executor->drain();
      }
    } catch (...) {
    }
    try {
      // Synchronous dispatch runs on the transport thread; do not return from
      // run() while a handler is still using the dispatcher.
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return in_flight_ == 0; });
    } catch (...) {
    }
    try {
      transport_->close();
    } catch (...) {
    }
    try {
      transport_->setOnRouteClosed(nullptr);
      transport_->setOnMessageEx(nullptr);
      transport_->setOnMessage(nullptr);
      transport_->setOnError(nullptr);
      transport_->setOnClose(nullptr);
    } catch (...) {
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      cancellation_tokens_.clear();
    } catch (...) {
    }
    clearSubscriptions();
  };

  struct CleanupGuard {
    std::function<void()> fn;
    ~CleanupGuard() {
      try {
        fn();
      } catch (...) {
      }
    }
  } guard{cleanup};

  auto record_error = [this](McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
  };

  auto record_error_if_running = [this](McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
      last_error_ = std::move(error);
    }
  };

  auto send_response = [this, record_error](const std::string& route,
                                            const Message& response) {
    try {
      auto encoded = encode(response);
      if (!encoded.hasValue()) {
        record_error(encoded.error());
        return;
      }
      auto parsed = parseStrict(encoded.value());
      if (!parsed.hasValue()) {
        record_error(parsed.error());
        return;
      }
      Status sent = transport_->sendTo(route, parsed.value());
      if (!sent) {
        record_error(sent.error);
      }
    } catch (const std::exception& error) {
      record_error(McpError::make(
          ErrorCode::InternalError,
          std::string("failed to send response: ") + error.what()));
    } catch (...) {
      record_error(McpError::make(ErrorCode::InternalError,
                                  "failed to send response"));
    }
  };

  const std::shared_ptr<IExecutor> executor = options_.executor;

  try {
    transport_->setOnMessageEx(
        [this, &dispatcher, record_error, record_error_if_running,
         send_response, executor](Json incoming,
                                  const MessageMetadata& metadata) {
          Message message;
          try {
            auto decoded = decode(incoming);
            if (!decoded.hasValue()) {
              record_error(decoded.error());
              return;
            }
            if (decoded.value().kind == MessageKind::Response) {
              return;
            }
            message = std::move(decoded.value());
          } catch (const std::exception& error) {
            record_error(McpError::make(
                ErrorCode::InternalError,
                std::string("failed to decode message: ") + error.what()));
            return;
          } catch (...) {
            record_error(McpError::make(ErrorCode::InternalError,
                                        "failed to decode message"));
            return;
          }

          if (message.method == "subscriptions/listen") {
            if (message.kind != MessageKind::Request) {
              return;
            }

            auto send_listen_error =
                [&](ErrorCode code, std::string text,
                    std::optional<std::string> reason) {
                  Message response;
                  response.kind = MessageKind::Response;
                  response.jsonrpc = "2.0";
                  response.id = message.id;
                  response.error = reason.has_value()
                                       ? McpError::make(
                                             code, std::move(text),
                                             Json{{"reason", *reason}})
                                       : McpError::make(code, std::move(text));
                  send_response(metadata.route, response);
                };

            if (metadata.route.empty()) {
              send_listen_error(
                  ErrorCode::InternalError,
                  "subscriptions require a streaming transport",
                  std::string("subscriptions_require_stream"));
              return;
            }
            if (metadata.protocolVersion != "2026-07-28") {
              send_listen_error(ErrorCode::MethodNotFound,
                                "subscriptions/listen requires 2026-07-28",
                                std::nullopt);
              return;
            }

            auto filter = parseSubscriptionFilter(message.params);
            if (!filter.hasValue()) {
              send_listen_error(ErrorCode::InvalidParams,
                                filter.error().message, std::nullopt);
              return;
            }

            const std::uint64_t serial =
                next_subscription_id_.fetch_add(1, std::memory_order_relaxed);
            const SubscriptionId subscription_id =
                "s" + std::to_string(serial);

            SubscriptionState state;
            state.id = subscription_id;
            state.route = metadata.route;
            state.listChanged = filter.value().listChanged;
            state.resources.insert(filter.value().resources.begin(),
                                   filter.value().resources.end());
            state.protocolVersion = metadata.protocolVersion;
            {
              std::lock_guard<std::mutex> lock(subscriptions_mutex_);
              subscriptions_.emplace(subscription_id, std::move(state));
            }

            Json ack;
            ack["jsonrpc"] = "2.0";
            ack["method"] =
                "notifications/subscriptions/acknowledged";
            ack["params"] =
                Json{{"subscriptionId", subscription_id},
                     {"requestId", message.id.toJson()}};

            bool sent_ok = false;
            try {
              sent_ok =
                  transport_->sendTo(metadata.route, ack).ok;
            } catch (const std::exception& error) {
              record_error(McpError::make(
                  ErrorCode::InternalError,
                  std::string("failed to send subscription ack: ") +
                      error.what()));
            } catch (...) {
              record_error(McpError::make(
                  ErrorCode::InternalError,
                  "failed to send subscription ack"));
            }
            if (!sent_ok) {
              removeSubscription(subscription_id);
            }
            return;
          }

          const bool is_request = message.kind == MessageKind::Request;
          const RequestId request_id = message.id;
          const std::string token_key =
              metadata.route + "\x1f" + request_id.toJson().dump();
          std::shared_ptr<CancellationToken> token =
              std::make_shared<CancellationToken>();
          if (is_request) {
            std::lock_guard<std::mutex> lock(mutex_);
            cancellation_tokens_[token_key] = token;
          }

          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++in_flight_;
          }

          auto finish = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (in_flight_ > 0) {
              --in_flight_;
            }
            cv_.notify_all();
          };

          auto erase_token = [this, is_request, token_key, token] {
            if (!is_request) {
              return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = cancellation_tokens_.find(token_key);
            if (it != cancellation_tokens_.end() && it->second == token) {
              cancellation_tokens_.erase(it);
            }
          };

          auto complete = [erase_token, finish] {
            erase_token();
            finish();
          };

          const std::string route = metadata.route;
          const std::string protocol_version = metadata.protocolVersion;

          auto notify = [this, record_error, route](Json notification) {
            try {
              Status sent = transport_->sendTo(route, notification);
              if (!sent) {
                record_error(sent.error);
              }
            } catch (const std::exception& error) {
              record_error(McpError::make(
                  ErrorCode::InternalError,
                  std::string("failed to send notification: ") +
                      error.what()));
            } catch (...) {
              record_error(McpError::make(
                  ErrorCode::InternalError, "failed to send notification"));
            }
          };

          auto dispatch_and_send =
              [this, &dispatcher, record_error, notify, send_response,
               complete, message = std::move(message), route,
               protocol_version, token]() mutable {
                try {
                  auto response = dispatcher.dispatch(
                      message, protocol_version, notify, token);
                  if (response.has_value()) {
                    send_response(route, *response);
                  }
                  complete();
                } catch (const std::exception& error) {
                  record_error(McpError::make(
                      ErrorCode::InternalError,
                      std::string("failed to handle message: ") +
                          error.what()));
                  complete();
                } catch (...) {
                  record_error(McpError::make(ErrorCode::InternalError,
                                              "failed to handle message"));
                  complete();
                }
              };

          if (!executor) {
            dispatch_and_send();
            return;
          }

          const ExecutorSubmitResult submitted = executor->post(
              std::move(dispatch_and_send),
              metadata.emergency ? Priority::Emergency : Priority::Normal);
          if (submitted) {
            return;
          }

          const char* reason =
              submitted.kind ==
                      ExecutorSubmitResult::Kind::RejectedQueueFull
                  ? "queue_full"
                  : "stopped";
          McpError rejection = McpError::make(
              ErrorCode::InternalError, "executor rejected request",
              Json{{"reason", reason}});
          record_error_if_running(rejection);

          if (is_request) {
            Message response;
            response.kind = MessageKind::Response;
            response.jsonrpc = "2.0";
            response.id = request_id;
            response.error = std::move(rejection);
            send_response(route, response);
          }
          complete();
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

    transport_->setOnRouteClosed(
        [this](const std::string& route) { handleRouteClosed(route); });

    Status started = transport_->start();
    if (!started) {
      return started;
    }
    did_start = true;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_; });
    }

    return Status::success();
  } catch (const std::exception&) {
    return Status::failure(McpError::make(ErrorCode::InternalError,
                                          "server run failed"));
  } catch (...) {
    return Status::failure(McpError::make(ErrorCode::InternalError,
                                          "server run failed"));
  }
}

void McpServer::stop() noexcept {
  std::vector<std::shared_ptr<CancellationToken>> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    tokens.reserve(cancellation_tokens_.size());
    for (const auto& item : cancellation_tokens_) {
      tokens.push_back(item.second);
    }
  }
  for (const auto& token : tokens) {
    token->cancel();
  }
  clearSubscriptions();
  cv_.notify_all();
}

std::optional<McpError> McpServer::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

void McpServer::removeSubscription(const SubscriptionId& id) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  subscriptions_.erase(id);
}

void McpServer::clearSubscriptions() noexcept {
  try {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_.clear();
  } catch (...) {
  }
}

void McpServer::notifyResourceUpdated(std::string_view uri) {
  std::vector<SubscriptionState> matches;
  {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    matches.reserve(subscriptions_.size());
    for (const auto& item : subscriptions_) {
      if (item.second.resources.count(std::string(uri)) != 0) {
        matches.push_back(item.second);
      }
    }
  }

  for (const auto& subscription : matches) {
    Json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "notifications/resources/updated";
    notification["params"] =
        Json{{"uri", std::string(uri)},
             {"subscriptionId", subscription.id}};

    Status sent = Status::failure(McpError::make(
        ErrorCode::InternalError, "subscription route unavailable"));
    try {
      sent = transport_->sendTo(subscription.route, notification);
    } catch (...) {
    }
    if (!sent) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = sent.error;
      }
      removeSubscription(subscription.id);
    }
  }
}

void McpServer::notifyListChanged(ListKind kind) {
  std::vector<SubscriptionState> matches;
  {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    matches.reserve(subscriptions_.size());
    for (const auto& item : subscriptions_) {
      const auto& kinds = item.second.listChanged;
      if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) {
        matches.push_back(item.second);
      }
    }
  }

  const std::string method = listChangedMethod(kind);
  for (const auto& subscription : matches) {
    Json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = method;
    notification["params"] = Json{{"subscriptionId", subscription.id}};

    Status sent = Status::failure(McpError::make(
        ErrorCode::InternalError, "subscription route unavailable"));
    try {
      sent = transport_->sendTo(subscription.route, notification);
    } catch (...) {
    }
    if (!sent) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = sent.error;
      }
      removeSubscription(subscription.id);
    }
  }
}

std::size_t McpServer::activeSubscriptions() const {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  return subscriptions_.size();
}

void McpServer::handleRouteClosed(const std::string& route) {
  std::vector<std::shared_ptr<CancellationToken>> tokens;
  const std::string prefix = route + "\x1f";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = cancellation_tokens_.begin();
         it != cancellation_tokens_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) {
        tokens.push_back(it->second);
        it = cancellation_tokens_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& token : tokens) {
    token->cancel();
  }

  {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
      if (it->second.route == route) {
        it = subscriptions_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

} // namespace v1
} // namespace mcp
