#include "tugmcp/server.hpp"

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
  // Declared before the cleanup guard so queued tasks always see a live
  // dispatcher: guard cleanup drains the executor before this object dies.
  Dispatcher dispatcher(tools_, options_.resources.get(),
                        options_.prompts.get(), std::move(config));

  bool cleaned = false;
  auto cleanup = [this, &cleaned]() noexcept {
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
      if (options_.executor) {
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

          const bool is_request = message.kind == MessageKind::Request;
          const RequestId request_id = message.id;
          std::shared_ptr<CancellationToken> token =
              std::make_shared<CancellationToken>();
          if (is_request) {
            std::lock_guard<std::mutex> lock(mutex_);
            cancellation_tokens_[request_id] = token;
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

          auto erase_token = [this, is_request, request_id, token] {
            if (!is_request) {
              return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = cancellation_tokens_.find(request_id);
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
              std::move(dispatch_and_send), Priority::Normal);
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

    Status started = transport_->start();
    if (!started) {
      return started;
    }

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
  cv_.notify_all();
}

std::optional<McpError> McpServer::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

} // namespace v1
} // namespace mcp
