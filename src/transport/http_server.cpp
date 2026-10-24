#include "tugmcp/transport/http.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <tugmcp/error.hpp>
#include <tugmcp/message.hpp>

namespace mcp {
namespace v1 {
namespace {

bool isJsonContentType(const std::string& value) {
  return value.find("application/json") != std::string::npos;
}

} // namespace

class HttpServerTransport::Impl {
public:
  explicit Impl(StreamableHttpServerOptions options)
      : options_(std::move(options)) {}

  ~Impl() { close(); }

  Status start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
      return failure(ErrorCode::InternalError, "transport is closed");
    }
    if (started_) {
      return Status::success();
    }

    try {
#if TUGMCP_TLS_ENABLED
      if (options_.tls.enabled) {
        server_ = std::make_unique<httplib::SSLServer>(
            options_.tls.certChainPath.c_str(),
            options_.tls.privateKeyPath.c_str(),
            options_.tls.clientCaFile.empty() ? nullptr
                                              : options_.tls.clientCaFile.c_str(),
            nullptr,
            options_.tls.privateKeyPassword.empty()
                ? nullptr
                : options_.tls.privateKeyPassword.c_str());
      } else {
        server_ = std::make_unique<httplib::Server>();
      }
#else
      server_ = std::make_unique<httplib::Server>();
#endif
    } catch (...) {
      server_.reset();
      return failure(ErrorCode::InternalError, "failed to create http server");
    }

    if (!server_ || !server_->is_valid()) {
      server_.reset();
      return failure(ErrorCode::InternalError, "invalid http server");
    }

    server_->set_payload_max_length(options_.maxBodyBytes);
    registerHandlers();

    int port = options_.port;
    if (port == 0) {
      port = server_->bind_to_any_port(options_.bind);
      if (port <= 0) {
        server_.reset();
        return failure(ErrorCode::InternalError, "failed to bind http server");
      }
    } else if (!server_->bind_to_port(options_.bind, port)) {
      server_.reset();
      return failure(ErrorCode::InternalError, "failed to bind http server");
    }

    bound_port_.store(port, std::memory_order_release);

    try {
      listen_thread_ = std::thread([this] { server_->listen_after_bind(); });
    } catch (...) {
      server_.reset();
      return failure(ErrorCode::InternalError,
                     "failed to start http listener");
    }

    started_ = true;
    return Status::success();
  }

  Status send(const Json& message) {
    if (closed_.load(std::memory_order_acquire)) {
      return failure(ErrorCode::InternalError, "transport is closed");
    }

    Message decoded;
    try {
      auto result = decode(message.dump());
      if (!result.hasValue()) {
        return failure(ErrorCode::InternalError, "invalid response message");
      }
      decoded = std::move(result.value());
    } catch (...) {
      return failure(ErrorCode::InternalError, "invalid response message");
    }

    if (decoded.kind != MessageKind::Response) {
      return Status::success();
    }

    std::shared_ptr<Pending> pending;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      const auto it = pending_.find(decoded.id);
      if (it == pending_.end()) {
        return Status::success();
      }
      pending = it->second;
      if (pending->done) {
        return Status::success();
      }
      pending->done = true;
      pending->response = message;
    }
    pending_cv_.notify_all();
    return Status::success();
  }

  void setOnMessage(std::function<void(Json)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(McpError)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_close_ = std::move(handler);
  }

  void close() noexcept {
    closed_.store(true, std::memory_order_release);

    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      for (auto& item : pending_) {
        auto& pending = item.second;
        if (!pending->done) {
          pending->done = true;
          pending->closed = true;
        }
      }
    }
    pending_cv_.notify_all();

    std::thread listener;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (server_) {
        try {
          server_->stop();
        } catch (...) {
        }
      }
      if (listen_thread_.joinable()) {
        if (listen_thread_.get_id() == std::this_thread::get_id()) {
          listen_thread_.detach();
        } else {
          listener = std::move(listen_thread_);
        }
      }
    }
    if (listener.joinable()) {
      listener.join();
    }

    notifyClose();
  }

  int boundPort() const {
    return bound_port_.load(std::memory_order_acquire);
  }

private:
  struct Pending {
    bool done = false;
    bool closed = false;
    Json response;
  };

  static Status failure(ErrorCode code, std::string message) {
    return Status::failure(McpError::make(code, std::move(message)));
  }

  void registerHandlers() {
    server_->Post("/mcp",
                  [this](const httplib::Request& request,
                         httplib::Response& response) {
                    handlePost(request, response);
                  });
    server_->Get("/mcp", [](const httplib::Request&,
                            httplib::Response& response) {
      response.status = 405;
    });
    server_->Delete("/mcp", [](const httplib::Request&,
                               httplib::Response& response) {
      response.status = 405;
    });
  }

  std::chrono::steady_clock::time_point now() const {
    if (options_.timeSource) {
      try {
        return options_.timeSource->now();
      } catch (...) {
      }
    }
    return std::chrono::steady_clock::now();
  }

  void respondJson(httplib::Response& response, int status, const Json& body) {
    response.status = status;
    try {
      response.set_content(body.dump(), "application/json");
    } catch (...) {
      response.status = 500;
      response.set_content("{}", "application/json");
    }
  }

  void respondError(httplib::Response& response, int status,
                    const McpError& error, const Json& id = Json(nullptr)) {
    Json body;
    body["jsonrpc"] = "2.0";
    body["id"] = id;
    body["error"] = errorToJson(error);
    respondJson(response, status, body);
  }

  void reportError(McpError error) {
    std::function<void(McpError)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_error_;
    }
    if (!handler) {
      return;
    }
    try {
      handler(std::move(error));
    } catch (...) {
    }
  }

  void notifyClose() noexcept {
    std::function<void()> handler;
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      notify = !close_notified_.exchange(true, std::memory_order_acq_rel);
      if (notify) {
        handler = std::move(on_close_);
      }
      on_message_ = nullptr;
      on_error_ = nullptr;
      on_close_ = nullptr;
    }
    if (!notify || !handler) {
      return;
    }
    try {
      handler();
    } catch (...) {
    }
  }

  bool invokeMessage(const Json& message) {
    std::function<void(Json)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_message_;
    }
    if (!handler) {
      return false;
    }
    try {
      handler(message);
      return true;
    } catch (const std::exception& error) {
      reportError(McpError::make(
          ErrorCode::InternalError,
          std::string("onMessage handler threw: ") + error.what()));
      return false;
    } catch (...) {
      reportError(McpError::make(ErrorCode::InternalError,
                                 "onMessage handler threw"));
      return false;
    }
  }

  void handlePost(const httplib::Request& request,
                  httplib::Response& response) {
    try {
      const std::string content_type =
          request.get_header_value("Content-Type");
      if (!isJsonContentType(content_type)) {
        response.status = 415;
        return;
      }

      if (request.body.size() > options_.maxBodyBytes) {
        response.status = 413;
        return;
      }

      const std::string version =
          request.get_header_value("MCP-Protocol-Version");
      if (!version.empty() && version != "2025-11-25" &&
          version != "2026-07-28") {
        respondError(response, 400,
                     McpError::make(ErrorCode::UnsupportedVersion,
                                    "unsupported protocol version: " +
                                        version));
        return;
      }

      auto decoded = decode(request.body);
      if (!decoded.hasValue()) {
        respondError(response, 400, decoded.error());
        return;
      }

      const Message& message = decoded.value();
      if (message.kind == MessageKind::Response) {
        respondError(response, 400,
                     McpError::make(ErrorCode::InvalidRequest,
                                    "response is not a request"));
        return;
      }

      if (version == "2026-07-28") {
        if (request.get_header_value("Mcp-Method") != message.method) {
          respondError(response, 400,
                       McpError::make(ErrorCode::HeaderMismatch,
                                      "Mcp-Method header mismatch"),
                       message.id.toJson());
          return;
        }
        if (message.method == "tools/call") {
          std::string name;
          if (message.params.is_object() &&
              message.params.contains("name") &&
              message.params.at("name").is_string()) {
            name = message.params.at("name").get<std::string>();
          }
          if (name.empty() ||
              request.get_header_value("Mcp-Name") != name) {
            respondError(response, 400,
                         McpError::make(ErrorCode::HeaderMismatch,
                                        "Mcp-Name header mismatch"),
                         message.id.toJson());
            return;
          }
        }
      }

      Json document;
      try {
        document = Json::parse(request.body);
      } catch (...) {
        respondError(response, 400,
                     McpError::make(ErrorCode::ParseError, "malformed JSON"));
        return;
      }

      if (message.kind == MessageKind::Notification) {
        if (!invokeMessage(document)) {
          respondError(response, 500,
                       McpError::make(ErrorCode::InternalError,
                                      "message handler unavailable"));
          return;
        }
        response.status = 202;
        response.set_content("", "application/json");
        return;
      }

      auto pending = std::make_shared<Pending>();
      bool closed_now = false;
      bool inserted = false;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        closed_now = closed_.load(std::memory_order_acquire);
        if (!closed_now) {
          inserted = pending_.emplace(message.id, pending).second;
        }
      }
      if (closed_now) {
        respondError(response, 503,
                     McpError::make(ErrorCode::InternalError,
                                    "transport is closed"),
                     message.id.toJson());
        return;
      }
      if (!inserted) {
        respondError(response, 409,
                     McpError::make(ErrorCode::InvalidRequest,
                                    "duplicate in-flight request id"),
                     message.id.toJson());
        return;
      }

      const auto deadline = now() + options_.requestTimeout;

      if (!invokeMessage(document)) {
        {
          std::lock_guard<std::mutex> lock(pending_mutex_);
          pending_.erase(message.id);
        }
        respondError(response, 500,
                     McpError::make(ErrorCode::InternalError,
                                    "message handler unavailable"),
                     message.id.toJson());
        return;
      }

      std::unique_lock<std::mutex> lock(pending_mutex_);
      const bool completed = pending_cv_.wait_until(
          lock, deadline, [&pending] { return pending->done; });

      if (!completed) {
        pending_.erase(message.id);
        lock.unlock();
        respondError(response, 504,
                     McpError::make(ErrorCode::InternalError,
                                    "request timed out"),
                     message.id.toJson());
        return;
      }
      if (pending->closed) {
        pending_.erase(message.id);
        lock.unlock();
        respondError(response, 503,
                     McpError::make(ErrorCode::InternalError,
                                    "transport is closed"),
                     message.id.toJson());
        return;
      }

      Json body = pending->response;
      pending_.erase(message.id);
      lock.unlock();

      respondJson(response, 200, body);
    } catch (...) {
      respondError(response, 500,
                   McpError::make(ErrorCode::InternalError,
                                  "internal server error"));
    }
  }

  StreamableHttpServerOptions options_;
  std::unique_ptr<httplib::Server> server_;

  std::mutex lifecycle_mutex_;
  bool started_ = false;
  std::thread listen_thread_;
  std::atomic<int> bound_port_{0};
  std::atomic<bool> closed_{false};
  std::atomic<bool> close_notified_{false};

  std::mutex callback_mutex_;
  std::function<void(Json)> on_message_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;

  std::mutex pending_mutex_;
  std::condition_variable pending_cv_;
  std::unordered_map<RequestId, std::shared_ptr<Pending>> pending_;
};

HttpServerTransport::HttpServerTransport(StreamableHttpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

HttpServerTransport::~HttpServerTransport() = default;

Status HttpServerTransport::start() { return impl_->start(); }

Status HttpServerTransport::send(const Json& message) {
  return impl_->send(message);
}

void HttpServerTransport::setOnMessage(
    std::function<void(Json)> handler) {
  impl_->setOnMessage(std::move(handler));
}

void HttpServerTransport::setOnError(
    std::function<void(McpError)> handler) {
  impl_->setOnError(std::move(handler));
}

void HttpServerTransport::setOnClose(std::function<void()> handler) {
  impl_->setOnClose(std::move(handler));
}

void HttpServerTransport::close() { impl_->close(); }

int HttpServerTransport::boundPort() const { return impl_->boundPort(); }

Result<std::shared_ptr<ITransport>> makeStreamableHttpServerTransport(
    const StreamableHttpServerOptions& options) {
  if (options.port < 0 || options.port > 65535) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams, "port must be between 0 and 65535"));
  }
  if (options.maxBodyBytes == 0) {
    return Result<std::shared_ptr<ITransport>>::failure(
        McpError::make(ErrorCode::InvalidParams,
                       "maxBodyBytes must be greater than zero"));
  }
  if (options.admission) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams,
        "admission hook is not implemented",
        Json{{"reason", "option_not_supported"}}));
  }
#if !TUGMCP_TLS_ENABLED
  if (options.tls.enabled) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams, "TLS is not enabled in this build"));
  }
#endif

  std::shared_ptr<ITransport> transport(new HttpServerTransport(options));
  return transport;
}

} // namespace v1
} // namespace mcp
