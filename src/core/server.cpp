#include "tugmcp/server.hpp"

#include <exception>
#include <mutex>
#include <string>
#include <utility>

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
  Dispatcher dispatcher(tools_, options_.resources.get(),
                        options_.prompts.get(), std::move(config));

  auto record_error = [this](McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
  };

  auto clear_callbacks = [this] {
    transport_->setOnMessageEx(nullptr);
    transport_->setOnMessage(nullptr);
    transport_->setOnError(nullptr);
    transport_->setOnClose(nullptr);
  };

  auto notify = [this, record_error](Json message) {
    try {
      const std::string encoded = message.dump();
      Status sent = transport_->sendRaw(encoded);
      if (!sent) {
        record_error(sent.error);
      }
    } catch (const std::exception& error) {
      record_error(McpError::make(
          ErrorCode::InternalError,
          std::string("failed to encode notification: ") + error.what()));
    } catch (...) {
      record_error(McpError::make(ErrorCode::InternalError,
                                  "failed to encode notification"));
    }
  };

  const std::shared_ptr<IExecutor> executor = options_.executor;

  transport_->setOnMessageEx(
      [this, &dispatcher, record_error, executor,
       notify](Json incoming, const MessageMetadata& metadata) {
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

        auto dispatch_and_send =
            [this, &dispatcher, record_error, notify,
             message = std::move(message),
             protocol_version = metadata.protocolVersion]() mutable {
              try {
                auto response =
                    dispatcher.dispatch(message, protocol_version, notify);
                if (!response.has_value()) {
                  return;
                }

                auto encoded = encode(*response);
                if (!encoded.hasValue()) {
                  record_error(encoded.error());
                  return;
                }

                Status sent = transport_->sendRaw(encoded.value());
                if (!sent) {
                  record_error(sent.error);
                }
              } catch (const std::exception& error) {
                record_error(McpError::make(
                    ErrorCode::InternalError,
                    std::string("failed to handle message: ") +
                        error.what()));
              } catch (...) {
                record_error(McpError::make(ErrorCode::InternalError,
                                            "failed to handle message"));
              }
            };

        if (!executor) {
          dispatch_and_send();
          return;
        }

        const ExecutorSubmitResult submitted =
            executor->post(std::move(dispatch_and_send), Priority::Normal);
        if (submitted) {
          return;
        }
        record_error(McpError::make(
            ErrorCode::InternalError, "executor rejected request",
            Json{{"reason",
                  submitted.kind ==
                          ExecutorSubmitResult::Kind::RejectedQueueFull
                      ? "queue_full"
                      : "stopped"}}));
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
    if (executor) {
      executor->drain();
    }
    return started;
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stopping_; });
  }

  if (executor) {
    executor->drain();
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
