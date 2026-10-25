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
  
bool isRouteGone(const Status& status) {
  return !status && status.error.data.is_object() &&
         status.error.data.value("reason", std::string{}) == "route_not_found";
}

const char* serverListKindName(ListKind kind) {
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
        ErrorCode::InvalidParams,
        "subscriptions/listen params must be an object"));
  }

  if (params.contains("subscriptions")) {
    const Json& legacy = params.at("subscriptions");
    if (legacy.is_object() && legacy.contains("resources") &&
        legacy.at("resources").is_array() &&
        !legacy.at("resources").empty()) {
      return Result<SubscriptionFilter>::failure(McpError::make(
          ErrorCode::InvalidParams,
          "per-resource subscriptions are not supported in 2026"));
    }
  }

  if (!params.contains("notifications")) {
    return filter;
  }
  const Json& notifications = params.at("notifications");
  if (!notifications.is_object()) {
    return Result<SubscriptionFilter>::failure(McpError::make(
        ErrorCode::InvalidParams, "notifications must be an object"));
  }

  auto read_flag = [&](const char* key,
                       ListKind kind) -> Result<bool> {
    if (!notifications.contains(key)) {
      return false;
    }
    if (!notifications.at(key).is_boolean()) {
      return Result<bool>::failure(McpError::make(
          ErrorCode::InvalidParams,
          std::string(key) + " must be a boolean"));
    }
    if (notifications.at(key).get<bool>()) {
      filter.listChanged.push_back(kind);
    }
    return true;
  };

  auto tools = read_flag("toolsListChanged", ListKind::Tools);
  if (!tools.hasValue()) {
    return Result<SubscriptionFilter>::failure(tools.error());
  }
  auto prompts = read_flag("promptsListChanged", ListKind::Prompts);
  if (!prompts.hasValue()) {
    return Result<SubscriptionFilter>::failure(prompts.error());
  }
  auto resources = read_flag("resourcesListChanged", ListKind::Resources);
  if (!resources.hasValue()) {
    return Result<SubscriptionFilter>::failure(resources.error());
  }
  return filter;
}

Json acceptedNotifications(const SubscriptionFilter& filter) {
  Json accepted = Json::object();
  for (const ListKind kind : filter.listChanged) {
    switch (kind) {
    case ListKind::Tools:
      accepted["toolsListChanged"] = true;
      break;
    case ListKind::Prompts:
      accepted["promptsListChanged"] = true;
      break;
    case ListKind::Resources:
      accepted["resourcesListChanged"] = true;
      break;
    }
  }
  return accepted;
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
  config.supportedVersions = options_.supportedVersions;
  config.supportsSubscriptions = transport_->supportsStreaming();
  config.completions = options_.completions.get();
  config.authorizer = options_.authorizer.get();
  config.approval = options_.approval.get();
  config.audit = options_.audit.get();
  config.approvalTimeout = options_.approvalTimeout;
  config.onError = [this](McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
  };
  // Declared before the cleanup guard so queued tasks always see a live
  // dispatcher: guard cleanup drains the executor before this object dies.
  Dispatcher dispatcher(tools_, options_.resources.get(),
                        options_.prompts.get(), std::move(config));

  // D17: default to an internal pool. Hosts may inject their own executor, or
  // opt into inline execution on the transport thread for deterministic tests.
  std::shared_ptr<IExecutor> executor = options_.executor;
  if (!executor && !options_.useInlineExecutor) {
    executor = makeThreadPoolExecutor(4, 1024);
  }

  bool cleaned = false;
  bool did_start = false;
  auto cleanup = [this, &cleaned, &did_start, executor]() noexcept {
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
      if (did_start && executor) {
        executor->drain();
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
      transport_->setOnSessionClosed(nullptr);
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
    {
      std::lock_guard<std::mutex> lock(logging_mutex_);
      log_levels_.clear();
      resource_subscriptions_.clear();
    }
    completeServerRequests(McpError::make(
        ErrorCode::InternalError, "transport is closed",
        Json{{"reason", "transport_closed"}}));
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
      if (!sent && !isRouteGone(sent)) {
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
              Message response = std::move(decoded.value());
              std::shared_ptr<PendingClientResponse> pending;
              {
                std::lock_guard<std::mutex> lock(server_requests_mutex_);
                const auto it = server_requests_.find(response.id);
                if (it != server_requests_.end()) {
                  pending = it->second;
                }
              }
              if (!pending) {
                return;
              }
              if (pending->sessionId != metadata.sessionId) {
                record_error(McpError::make(
                    ErrorCode::InternalError, "session mismatch",
                    Json{{"reason", "session_mismatch"}}));
                return;
              }
              {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (pending->done) {
                  return;
                }
                pending->done = true;
                if (response.error.has_value()) {
                  pending->error = std::move(response.error);
                } else {
                  pending->result = std::move(response.result);
                }
              }
              pending->cv.notify_all();
              return;
            }
            if (decoded.value().kind == MessageKind::Notification &&
                decoded.value().method == "notifications/cancelled" &&
                decoded.value().params.is_object() &&
                decoded.value().params.contains("requestId")) {
              auto request_id =
                  RequestId::fromJson(decoded.value().params.at("requestId"));
              if (request_id.hasValue()) {
                const std::string suffix =
                    "\x1f" + request_id.value().toJson().dump();
                const std::string session_prefix =
                    metadata.sessionId + "\x1f";
                std::vector<std::shared_ptr<CancellationToken>> tokens;
                {
                  std::lock_guard<std::mutex> lock(mutex_);
                  for (const auto& item : cancellation_tokens_) {
                    const bool suffix_match =
                        item.first.size() >= suffix.size() &&
                        item.first.compare(item.first.size() - suffix.size(),
                                           suffix.size(), suffix) == 0;
                    if (!suffix_match) {
                      continue;
                    }
                    if (metadata.sessionId.empty() ||
                        item.first.compare(0, session_prefix.size(),
                                           session_prefix) == 0) {
                      tokens.push_back(item.second);
                    }
                  }
                }
                for (const auto& token : tokens) {
                  token->cancel();
                }
              }
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

            const Json subscription_id_json = message.id.toJson();
            const SubscriptionId subscription_id =
                subscription_id_json.dump();

            const SubscriptionId subscription_key =
                metadata.route + "\x1f" + subscription_id;
            SubscriptionState state;
            state.id = subscription_key;
            state.subscriptionIdJson = subscription_id_json;
            state.route = metadata.route;
            state.listChanged = filter.value().listChanged;
            state.resources.insert(filter.value().resources.begin(),
                                   filter.value().resources.end());
            state.protocolVersion = metadata.protocolVersion;
            {
              std::lock_guard<std::mutex> lock(subscriptions_mutex_);
              subscriptions_.emplace(subscription_key, std::move(state));
            }

            Json ack;
            ack["jsonrpc"] = "2.0";
            ack["method"] =
                "notifications/subscriptions/acknowledged";
            ack["params"] =
                Json{{"notifications",
                      acceptedNotifications(filter.value())},
                     {"_meta",
                      Json{{"io.modelcontextprotocol/subscriptionId",
                            subscription_id_json}}}};

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
              removeSubscription(subscription_key);
            }
            return;
          }

          const bool is_2025_session =
              metadata.protocolVersion.empty() ||
              metadata.protocolVersion == "2025-11-25";
          if (is_2025_session && message.kind == MessageKind::Request) {
            auto send_empty = [&] {
              Message response;
              response.kind = MessageKind::Response;
              response.jsonrpc = "2.0";
              response.id = message.id;
              response.result = Json::object();
              send_response(metadata.route, response);
            };
            auto send_invalid = [&](const std::string& text) {
              Message response;
              response.kind = MessageKind::Response;
              response.jsonrpc = "2.0";
              response.id = message.id;
              response.error =
                  McpError::make(ErrorCode::InvalidParams, text);
              send_response(metadata.route, response);
            };

            if (message.method == "logging/setLevel") {
              const Json& params = message.params;
              static const char* kLevels[] = {
                  "debug", "info", "notice", "warning",
                  "error", "critical", "alert", "emergency"};
              bool valid = false;
              std::string level;
              if (params.is_object() && params.contains("level") &&
                  params.at("level").is_string()) {
                level = params.at("level").get<std::string>();
                for (const char* candidate : kLevels) {
                  if (level == candidate) {
                    valid = true;
                    break;
                  }
                }
              }
              if (!valid) {
                send_invalid("invalid logging level");
              } else {
                {
                  std::lock_guard<std::mutex> lock(logging_mutex_);
                  log_levels_[metadata.sessionId] = level;
                }
                send_empty();
              }
              return;
            }

            if (message.method == "resources/subscribe" ||
                message.method == "resources/unsubscribe") {
              const Json& params = message.params;
              if (!params.is_object() || !params.contains("uri") ||
                  !params.at("uri").is_string()) {
                send_invalid("resources subscription requires a string uri");
                return;
              }
              const std::string uri =
                  params.at("uri").get<std::string>();
              if (metadata.sessionId.empty()) {
                Message response;
                response.kind = MessageKind::Response;
                response.jsonrpc = "2.0";
                response.id = message.id;
                response.error = McpError::make(
                    ErrorCode::InternalError,
                    "resource subscriptions require a session",
                    Json{{"reason", "subscriptions_require_session"}});
                send_response(metadata.route, response);
                return;
              }
              if (message.method == "resources/subscribe" &&
                  options_.authorizer != nullptr) {
                Status allowed;
                try {
                  allowed =
                      options_.authorizer->authorize(metadata.auth, uri);
                } catch (...) {
                  allowed = Status::failure(McpError::make(
                      ErrorCode::InternalError, "authorization failed"));
                }
                if (!allowed) {
                  Message response;
                  response.kind = MessageKind::Response;
                  response.jsonrpc = "2.0";
                  response.id = message.id;
                  response.error = McpError::make(
                      ErrorCode::InternalError, "forbidden",
                      Json{{"reason", "forbidden"}});
                  send_response(metadata.route, response);
                  return;
                }
              }
              bool unknown_subscription = false;
              {
                std::lock_guard<std::mutex> lock(logging_mutex_);
                auto& subscriptions =
                    resource_subscriptions_[metadata.sessionId];
                if (message.method == "resources/subscribe") {
                  subscriptions.insert(uri);
                } else {
                  const auto it = subscriptions.find(uri);
                  if (it == subscriptions.end()) {
                    unknown_subscription = true;
                  } else {
                    subscriptions.erase(it);
                  }
                }
              }
              if (unknown_subscription) {
                send_invalid("resource is not subscribed");
              } else {
                send_empty();
              }
              return;
            }
          }
          const bool is_request = message.kind == MessageKind::Request;
          const RequestId request_id = message.id;
          const std::string token_key =
              metadata.sessionId + "\x1f" + metadata.route + "\x1f" +
              request_id.toJson().dump();
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
              if (!sent && !isRouteGone(sent)) {
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
               protocol_version, token,
               session_id = std::string(metadata.sessionId),
               client_capabilities = Json(metadata.clientCapabilities),
               auth = AuthContext(metadata.auth)]() mutable {
                try {
                  auto response = dispatcher.dispatch(
                      message, protocol_version, notify, token, session_id,
                      std::move(client_capabilities),
                      std::move(auth));
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
    transport_->setOnSessionClosed(
        [this](const std::string& id) { handleSessionClosed(id); });

    transport_->setSupportedVersions(options_.supportedVersions);
    transport_->setAuthenticator(options_.auth);
    transport_->setAuditHook(options_.audit);
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
             {"_meta",
              Json{{"io.modelcontextprotocol/subscriptionId",
                    subscription.subscriptionIdJson}}}};

    Status sent = Status::failure(McpError::make(
        ErrorCode::InternalError, "subscription route unavailable"));
    try {
      sent = transport_->sendTo(subscription.route, notification);
    } catch (...) {
    }
    if (!sent) {
      const bool route_gone =
          sent.error.data.is_object() &&
          sent.error.data.contains("reason") &&
          sent.error.data.at("reason") == "route_not_found";
      if (!route_gone) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = sent.error;
      }
      removeSubscription(subscription.id);
    }
  }

  std::vector<std::string> session_ids;
  {
    std::lock_guard<std::mutex> lock(logging_mutex_);
    for (const auto& item : resource_subscriptions_) {
      if (item.second.count(std::string(uri)) != 0) {
        session_ids.push_back(item.first);
      }
    }
  }
  for (const auto& session_id : session_ids) {
    Json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "notifications/resources/updated";
    notification["params"] = Json{{"uri", std::string(uri)}};
    Status sent = Status::failure(McpError::make(
        ErrorCode::InternalError, "session stream unavailable"));
    try {
      sent = transport_->sendToSession(session_id, notification);
    } catch (...) {
    }
    if (!sent) {
      const std::string reason =
          sent.error.data.is_object() &&
                  sent.error.data.contains("reason") &&
                  sent.error.data.at("reason").is_string()
              ? sent.error.data.at("reason").get<std::string>()
              : std::string{};
      const bool benign =
          reason == "no_session_stream" ||
          reason == "session_closed" ||
          reason == "route_not_found" ||
          reason == "transport_closed";
      if (!benign) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = sent.error;
      }
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
    notification["params"] =
        Json{{"_meta",
              Json{{"io.modelcontextprotocol/subscriptionId",
                    subscription.subscriptionIdJson}}}};

    Status sent = Status::failure(McpError::make(
        ErrorCode::InternalError, "subscription route unavailable"));
    try {
      sent = transport_->sendTo(subscription.route, notification);
    } catch (...) {
    }
    if (!sent) {
      const bool route_gone =
          sent.error.data.is_object() &&
          sent.error.data.contains("reason") &&
          sent.error.data.at("reason") == "route_not_found";
      if (!route_gone) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = sent.error;
      }
      removeSubscription(subscription.id);
    }
  }
}

std::optional<std::string> McpServer::logLevel(
    std::string_view sessionId) const {
  std::lock_guard<std::mutex> lock(logging_mutex_);
  const auto it = log_levels_.find(std::string(sessionId));
  if (it == log_levels_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::size_t McpServer::activeSubscriptions() const {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  return subscriptions_.size();
}

void McpServer::handleRouteClosed(const std::string& route) {
  std::vector<std::shared_ptr<CancellationToken>> tokens;
  const std::string needle = "\x1f" + route + "\x1f";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = cancellation_tokens_.begin();
         it != cancellation_tokens_.end();) {
      if (it->first.find(needle) != std::string::npos) {
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

Result<Json> McpServer::requestClient(std::string_view sessionId,
                                      std::string_view method, Json params,
                                      std::chrono::milliseconds timeout) {
  if (!transport_) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "transport is null",
        Json{{"reason", "transport_closed"}}));
  }

  const std::int64_t numeric_id =
      next_server_request_id_.fetch_add(1, std::memory_order_relaxed);
  RequestId id;
  id.value = numeric_id;
  auto pending = std::make_shared<PendingClientResponse>();
  pending->sessionId = std::string(sessionId);
  {
    std::lock_guard<std::mutex> lock(server_requests_mutex_);
    server_requests_[id] = pending;
  }

  Json request;
  request["jsonrpc"] = "2.0";
  request["id"] = numeric_id;
  request["method"] = std::string(method);
  request["params"] = std::move(params);

  Status sent = Status::failure(McpError::make(
      ErrorCode::InternalError, "failed to send server request"));
  try {
    sent = transport_->sendToSession(sessionId, request);
  } catch (const std::exception& error) {
    sent = Status::failure(McpError::make(
        ErrorCode::InternalError,
        std::string("failed to send server request: ") + error.what()));
  } catch (...) {
  }
  if (!sent) {
    {
      std::lock_guard<std::mutex> lock(server_requests_mutex_);
      server_requests_.erase(id);
    }
    return Result<Json>::failure(sent.error);
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock<std::mutex> lock(pending->mutex);
  const bool completed = pending->cv.wait_until(
      lock, deadline, [&pending] { return pending->done; });
  if (!completed) {
    lock.unlock();
    {
      std::lock_guard<std::mutex> registry_lock(server_requests_mutex_);
      server_requests_.erase(id);
    }
    return Result<Json>::failure(McpError::make(
        ErrorCode::InternalError, "server request timed out",
        Json{{"reason", "timeout"}}));
  }

  Json result = std::move(pending->result);
  std::optional<McpError> error = std::move(pending->error);
  lock.unlock();
  {
    std::lock_guard<std::mutex> registry_lock(server_requests_mutex_);
    server_requests_.erase(id);
  }
  if (error.has_value()) {
    return Result<Json>::failure(std::move(*error));
  }
  return result;
}

Status McpServer::sendNotification(std::string_view sessionId,
                                   std::string_view method, Json params) {
  if (!transport_) {
    return Status::failure(McpError::make(
        ErrorCode::InternalError, "transport is null",
        Json{{"reason", "transport_closed"}}));
  }
  Json notification;
  notification["jsonrpc"] = "2.0";
  notification["method"] = std::string(method);
  notification["params"] = std::move(params);
  return transport_->sendToSession(sessionId, notification);
}

void McpServer::handleSessionClosed(const std::string& sessionId) {
  {
    std::lock_guard<std::mutex> lock(logging_mutex_);
    log_levels_.erase(sessionId);
    resource_subscriptions_.erase(sessionId);
  }
  std::vector<std::shared_ptr<PendingClientResponse>> pending_requests;
  {
    std::lock_guard<std::mutex> lock(server_requests_mutex_);
    for (auto it = server_requests_.begin(); it != server_requests_.end();) {
      if (it->second->sessionId == sessionId) {
        pending_requests.push_back(it->second);
        it = server_requests_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& pending : pending_requests) {
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->done) {
        continue;
      }
      pending->done = true;
      pending->error = McpError::make(
          ErrorCode::InternalError, "session closed",
          Json{{"reason", "session_closed"}});
    }
    pending->cv.notify_all();
  }
}
void McpServer::completeServerRequests(const McpError& error) noexcept {
  try {
    std::vector<std::shared_ptr<PendingClientResponse>> pending_requests;
    {
      std::lock_guard<std::mutex> lock(server_requests_mutex_);
      for (auto& item : server_requests_) {
        pending_requests.push_back(item.second);
      }
      server_requests_.clear();
    }
    for (const auto& pending : pending_requests) {
      {
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (pending->done) {
          continue;
        }
        pending->done = true;
        pending->error = error;
      }
      pending->cv.notify_all();
    }
  } catch (...) {
  }
}

} // namespace v1
} // namespace mcp
