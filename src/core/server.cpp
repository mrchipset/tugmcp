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
      auto decoded = decode(incoming);
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

      Status sent = transport_->sendRaw(encoded.value());
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
