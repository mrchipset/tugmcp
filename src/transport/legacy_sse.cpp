#include "tugmcp/transport/legacy_sse.hpp"

#include "tugmcp/detail/http_httplib.hpp"

#include "http_security.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tugmcp/message.hpp>

namespace mcp {
namespace v1 {
namespace {

bool isJsonContentType(const std::string& value) {
  return value.find("application/json") != std::string::npos;
}

std::string makeSessionId() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t sequence =
      counter.fetch_add(1, std::memory_order_relaxed) + 1;
  std::random_device random;
  const std::uint64_t high =
      (static_cast<std::uint64_t>(random()) << 32) ^
      static_cast<std::uint64_t>(random());
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                static_cast<unsigned long long>(high),
                static_cast<unsigned long long>(sequence));
  return std::string(buffer);
}

Headers collectHeaders(const httplib::Request& request) {
  Headers headers;
  for (const auto& item : request.headers) {
    headers[detail::asciiLower(item.first)] = item.second;
  }
  return headers;
}


} // namespace

class LegacySseServerTransport::Impl {
public:
  explicit Impl(LegacySseServerOptions options)
      : options_(std::move(options)) {}

  ~Impl() { close(); }

  Status start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (closed_) {
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
            options_.tls.clientCaFile.empty()
                ? nullptr
                : options_.tls.clientCaFile.c_str());
      } else {
        server_ = std::make_unique<httplib::Server>();
      }
#else
      server_ = std::make_unique<httplib::Server>();
#endif
    } catch (...) {
      server_.reset();
      return failure(ErrorCode::InternalError, "failed to create server");
    }
    if (!server_ || !server_->is_valid()) {
      server_.reset();
      return failure(ErrorCode::InternalError, "invalid server");
    }
    server_->set_payload_max_length(options_.maxBodyBytes);
    server_->Get("/sse", [this](const httplib::Request& request,
                                httplib::Response& response) {
      handleSse(request, response);
    });
    server_->Post("/messages", [this](const httplib::Request& request,
                                      httplib::Response& response) {
      handleMessages(request, response);
    });

    int port = options_.port;
    if (port == 0) {
      port = server_->bind_to_any_port(options_.bind);
      if (port <= 0) {
        server_.reset();
        return failure(ErrorCode::InternalError, "failed to bind");
      }
    } else if (!server_->bind_to_port(options_.bind, port)) {
      server_.reset();
      return failure(ErrorCode::InternalError, "failed to bind");
    }
    bound_port_.store(port, std::memory_order_release);
    try {
      listen_thread_ = std::thread([this] { server_->listen_after_bind(); });
    } catch (...) {
      server_.reset();
      return failure(ErrorCode::InternalError, "failed to listen");
    }
    started_ = true;
    return Status::success();
  }

  Status send(const Json& message) {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      if (sessions_.size() != 1) {
        return failure(ErrorCode::InternalError, "ambiguous route");
      }
      session = sessions_.begin()->second;
    }
    if (!session) {
      return failure(ErrorCode::InternalError, "route not found",
                     Json{{"reason", "route_not_found"}});
    }
    return enqueue(session, message);
  }

  Status sendTo(std::string_view route, const Json& message) {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      const auto it = sessions_.find(std::string(route));
      if (it != sessions_.end()) {
        session = it->second;
      }
    }
    if (!session || session->closed) {
      return failure(ErrorCode::InternalError, "route not found",
                     Json{{"reason", "route_not_found"}});
    }
    return enqueue(session, message);
  }

  Status sendToSession(std::string_view sessionId, const Json& message) {
    return sendTo(sessionId, message);
  }

  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_session_closed_ = std::move(handler);
  }
  void setOnMessage(std::function<void(Json)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ = std::move(handler);
  }
  void setOnMessageEx(MessageHandlerEx handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ex_ = std::move(handler);
  }
  void setOnError(std::function<void(McpError)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_error_ = std::move(handler);
  }
  void setOnClose(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_close_ = std::move(handler);
  }
  void setAuthenticator(std::shared_ptr<IAuthenticator> auth) {
    if (!auth) {
      return;
    }
    std::lock_guard<std::mutex> lock(security_mutex_);
    options_.auth = std::move(auth);
  }
  void setAuditHook(std::shared_ptr<IAuditHook> audit) {
    if (!audit) {
      return;
    }
    std::lock_guard<std::mutex> lock(security_mutex_);
    options_.audit = std::move(audit);
  }
  void setSupportedVersions(std::vector<std::string>) {}

  void close() noexcept {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
      notifyClose();
      return;
    }
    std::vector<std::shared_ptr<Session>> sessions;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      for (auto& item : sessions_) {
        sessions.push_back(item.second);
      }
    }
    for (const auto& session : sessions) {
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->closed = true;
        session->cv.notify_all();
      }
      notifySessionClosed(session->id);
    }
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
        listener = std::move(listen_thread_);
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
  struct Session {
    std::string id;
    AuthContext auth;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> frames;
    bool closed = false;
  };

  static Status failure(ErrorCode code, std::string message,
                        Json data = Json()) {
    return Status::failure(
        McpError::make(code, std::move(message), std::move(data)));
  }

  void notifyClose() noexcept {
    std::function<void()> handler;
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      notify = !close_notified_.exchange(true);
      if (notify) {
        handler = std::move(on_close_);
      }
      on_message_ = nullptr;
      on_message_ex_ = nullptr;
      on_error_ = nullptr;
      on_close_ = nullptr;
      on_session_closed_ = nullptr;
    }
    if (notify && handler) {
      try {
        handler();
      } catch (...) {
      }
    }
  }

  void notifySessionClosed(const std::string& id) noexcept {
    std::function<void(const std::string&)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_session_closed_;
    }
    if (handler) {
      try {
        handler(id);
      } catch (...) {
      }
    }
  }

  void reportError(McpError error) {
    std::function<void(McpError)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_error_;
    }
    if (handler) {
      try {
        handler(std::move(error));
      } catch (...) {
      }
    }
  }

  AuthContext authenticate(const httplib::Request& request, bool& failed) {
    failed = false;
    std::shared_ptr<IAuthenticator> authenticator;
    {
      std::lock_guard<std::mutex> lock(security_mutex_);
      authenticator = options_.auth;
    }
    if (!authenticator) {
      return AuthContext{};
    }
    try {
      auto result = authenticator->authenticate(collectHeaders(request));
      if (result.hasValue()) {
        return result.value();
      }
    } catch (...) {
    }
    failed = true;
    auditAuthFailed();
    return AuthContext{};
  }
  void auditAuthFailed() noexcept {
    std::shared_ptr<IAuditHook> audit;
    {
      std::lock_guard<std::mutex> lock(security_mutex_);
      audit = options_.audit;
    }
    if (!audit) {
      return;
    }
    try {
      audit->onEvent(AuthContext{}, "auth_failed",
                     Json{{"reason", "unauthorized"}});
    } catch (...) {
      reportError(McpError::make(
          ErrorCode::InternalError, "audit hook failed",
          Json{{"reason", "audit_failed"}}));
    }
  }

  Status enqueueLocked(const std::shared_ptr<Session>& session,
                       const Json& message) {
    if (options_.perStreamQueue != 0 &&
        session->frames.size() >= options_.perStreamQueue) {
      session->closed = true;
      session->cv.notify_all();
      return failure(ErrorCode::InternalError, "stream overflow",
                     Json{{"reason", "route_not_found"}});
    }
    try {
      session->frames.push_back(
          "event: message\ndata: " + message.dump() + "\n\n");
      session->cv.notify_all();
      return Status::success();
    } catch (...) {
      return failure(ErrorCode::InternalError, "failed to queue message");
    }
  }

  Status enqueue(const std::shared_ptr<Session>& session,
                 const Json& message) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->closed) {
      return failure(ErrorCode::InternalError, "route not found",
                     Json{{"reason", "route_not_found"}});
    }
    return enqueueLocked(session, message);
  }

  bool streamProvider(const std::shared_ptr<Session>& session,
                      httplib::DataSink& sink) {
    std::string endpoint = "event: endpoint\ndata: /messages?sessionId=" +
                           session->id + "\n\n";
    if (!sink.write(endpoint.data(), endpoint.size())) {
      return false;
    }
    for (;;) {
      std::string frame;
      bool keepalive = false;
      {
        std::unique_lock<std::mutex> lock(session->mutex);
        if (session->frames.empty() && !session->closed) {
          session->cv.wait_for(lock, std::chrono::milliseconds(500));
        }
        if (session->closed && session->frames.empty()) {
          return false;
        }
        if (!session->frames.empty()) {
          frame = std::move(session->frames.front());
          session->frames.pop_front();
        } else {
          keepalive = true;
        }
      }
      if (keepalive) {
        static const char kKeepalive[] = ": keepalive\n\n";
        if (!sink.write(kKeepalive, sizeof(kKeepalive) - 1)) {
          std::lock_guard<std::mutex> lock(session->mutex);
          session->closed = true;
          return false;
        }
        continue;
      }
      if (!sink.write(frame.data(), frame.size())) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->closed = true;
        return false;
      }
    }
  }

  void handleSse(const httplib::Request& request,
                 httplib::Response& response) {
    if (closed_.load(std::memory_order_acquire)) {
      response.status = 410;
      return;
    }
    if (!detail::hostAllowed(request.get_header_value("Host"),
                             options_.allowedHosts) ||
        !detail::originAllowed(request.get_header_value("Origin"),
                               options_.allowedOrigins)) {
      response.status = 403;
      return;
    }
    bool auth_failed = false;
    const AuthContext auth = authenticate(request, auth_failed);
    if (auth_failed) {
      response.status = 401;
      response.set_content(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"unauthorized","data":{"reason":"unauthorized"}}})",
                           "application/json");
      return;
    }
    auto session = std::make_shared<Session>();
    session->id = makeSessionId();
    session->auth = auth;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      if (options_.maxSessions != 0 &&
          sessions_.size() >= options_.maxSessions) {
        response.status = 429;
        return;
      }
      sessions_[session->id] = session;
    }
    response.set_header("Cache-Control", "no-cache");
    response.status = 200;
    const std::string route = session->id;
    response.set_chunked_content_provider(
        "text/event-stream",
        [this, session](std::size_t, httplib::DataSink& sink) {
          return streamProvider(session, sink);
        },
        [this, route, session](bool) {
          {
            std::lock_guard<std::mutex> lock(routes_mutex_);
            sessions_.erase(route);
          }
          {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->closed = true;
            session->cv.notify_all();
          }
          notifySessionClosed(route);
        });
  }
  void handleMessages(const httplib::Request& request,
                      httplib::Response& response) {
    if (closed_.load(std::memory_order_acquire)) {
      response.status = 410;
      return;
    }
    if (!detail::hostAllowed(request.get_header_value("Host"),
                             options_.allowedHosts) ||
        !detail::originAllowed(request.get_header_value("Origin"),
                               options_.allowedOrigins)) {
      response.status = 403;
      return;
    }
    bool auth_failed = false;
    const AuthContext auth = authenticate(request, auth_failed);
    if (auth_failed) {
      response.status = 401;
      response.set_content(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"unauthorized","data":{"reason":"unauthorized"}}})",
                           "application/json");
      return;
    }
    if (!isJsonContentType(request.get_header_value("Content-Type"))) {
      response.status = 415;
      return;
    }
    if (request.body.size() > options_.maxBodyBytes) {
      response.status = 413;
      return;
    }
    const std::string session_id = request.get_param_value("sessionId");
    if (session_id.empty()) {
      response.status = 400;
      return;
    }
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      const auto it = sessions_.find(session_id);
      if (it == sessions_.end()) {
        response.status = 404;
        return;
      }
      session = it->second;
    }
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (session->closed) {
        response.status = 410;
        return;
      }
    }

    auto parsed = parseStrict(request.body);
    if (!parsed.hasValue()) {
      response.status = 400;
      return;
    }
    MessageMetadata metadata;
    metadata.protocolVersion = "2025-11-25";
    metadata.route = session_id;
    metadata.sessionId = session_id;
    metadata.auth = auth;
    auto invoke = [this, &metadata](const Json& message) {
      MessageHandlerEx extended;
      std::function<void(Json)> simple;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        extended = on_message_ex_;
        simple = on_message_;
      }
      try {
        if (extended) {
          extended(message, metadata);
        } else if (simple) {
          simple(message);
        }
      } catch (...) {
      }
    };

    if (parsed.value().is_array()) {
      for (const auto& item : parsed.value()) {
        invoke(item);
      }
    } else {
      invoke(parsed.value());
    }
    response.status = 202;
    response.set_content("", "application/json");
  }

  LegacySseServerOptions options_;
  std::unique_ptr<httplib::Server> server_;
  mutable std::mutex lifecycle_mutex_;
  bool started_ = false;
  std::thread listen_thread_;
  std::atomic<int> bound_port_{0};
  std::atomic<bool> closed_{false};
  std::atomic<bool> close_notified_{false};

  std::mutex callback_mutex_;
  std::function<void(Json)> on_message_;
  MessageHandlerEx on_message_ex_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
  std::function<void(const std::string&)> on_session_closed_;

  std::mutex security_mutex_;

  std::mutex routes_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

LegacySseServerTransport::LegacySseServerTransport(
    LegacySseServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LegacySseServerTransport::~LegacySseServerTransport() = default;

Status LegacySseServerTransport::start() { return impl_->start(); }
Status LegacySseServerTransport::send(const Json& message) {
  return impl_->send(message);
}
Status LegacySseServerTransport::sendTo(std::string_view route,
                                        const Json& message) {
  return impl_->sendTo(route, message);
}
void LegacySseServerTransport::setOnMessage(
    std::function<void(Json)> handler) {
  impl_->setOnMessage(std::move(handler));
}
void LegacySseServerTransport::setOnMessageEx(MessageHandlerEx handler) {
  impl_->setOnMessageEx(std::move(handler));
}
void LegacySseServerTransport::setOnError(
    std::function<void(McpError)> handler) {
  impl_->setOnError(std::move(handler));
}
void LegacySseServerTransport::setOnClose(std::function<void()> handler) {
  impl_->setOnClose(std::move(handler));
}
void LegacySseServerTransport::setAuthenticator(
    std::shared_ptr<IAuthenticator> auth) {
  impl_->setAuthenticator(std::move(auth));
}
void LegacySseServerTransport::setAuditHook(
    std::shared_ptr<IAuditHook> audit) {
  impl_->setAuditHook(std::move(audit));
}
void LegacySseServerTransport::setSupportedVersions(
    std::vector<std::string> versions) {
  impl_->setSupportedVersions(std::move(versions));
}
void LegacySseServerTransport::setOnSessionClosed(
    std::function<void(const std::string&)> handler) {
  impl_->setOnSessionClosed(std::move(handler));
}
Status LegacySseServerTransport::sendToSession(
    std::string_view sessionId, const Json& message) {
  return impl_->sendToSession(sessionId, message);
}
void LegacySseServerTransport::close() { impl_->close(); }
int LegacySseServerTransport::boundPort() const { return impl_->boundPort(); }

// ---------------------------------------------------------------------------
// Legacy client
// ---------------------------------------------------------------------------

class LegacySseClientTransport::Impl {
public:
  explicit Impl(LegacySseClientOptions options)
      : options_(std::move(options)) {}

  ~Impl() { close(); }

  Status start() {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (closed_) {
        return failure(ErrorCode::InternalError, "transport is closed");
      }
      if (started_) {
        return Status::success();
      }
      client_ = createClient();
      if (!client_) {
        return failure(ErrorCode::InternalError, "failed to create client");
      }
      started_ = true;
      {
        std::lock_guard<std::mutex> endpoint_lock(endpoint_mutex_);
        endpoint_ready_ = false;
        messages_url_.clear();
      }
    }
    reader_ = std::thread([this] { readerLoop(); });
    std::unique_lock<std::mutex> ready_lock(endpoint_mutex_);
    const bool ready = endpoint_cv_.wait_for(
        ready_lock, std::chrono::milliseconds(options_.connectTimeoutMs),
        [this] { return endpoint_ready_ || closed_; });
    if (!ready || !endpoint_ready_) {
      ready_lock.unlock();
      stopReader();
      return failure(ErrorCode::InternalError, "legacy SSE endpoint timeout");
    }
    return Status::success();
  }

  Status send(const Json& message) {
    std::string url;
    {
      std::lock_guard<std::mutex> lock(endpoint_mutex_);
      url = messages_url_;
    }
    if (url.empty()) {
      return failure(ErrorCode::InternalError, "legacy endpoint not ready");
    }
    std::string body;
    try {
      body = message.dump();
    } catch (...) {
      return failure(ErrorCode::InternalError, "failed to encode message");
    }
    auto client = createClient();
    if (!client) {
      return failure(ErrorCode::InternalError, "transport is closed");
    }
    auto response = client->Post(url, body, "application/json");
    client->stop();
    if (!response) {
      return failure(ErrorCode::InternalError, "legacy POST failed");
    }
    if (response->status < 200 || response->status >= 300) {
      return failure(ErrorCode::InternalError, "legacy POST status",
                     Json{{"reason", "http_status"},
                          {"status", response->status}});
    }
    return Status::success();
  }

  Status sendTo(std::string_view, const Json& message) {
    return send(message);
  }

  Status sendToSession(std::string_view sessionId, const Json& message) {
    return sendTo(sessionId, message);
  }

  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_session_closed_ = std::move(handler);
  }
  void setOnMessage(std::function<void(Json)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ = std::move(handler);
  }
  void setOnMessageEx(MessageHandlerEx handler) {
    setOnMessage([handler = std::move(handler)](Json message) mutable {
      handler(std::move(message), MessageMetadata{});
    });
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
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
      notifyClose();
      return;
    }
    stopReader();
    notifyClose();
  }

private:
  static Status failure(ErrorCode code, std::string message,
                        Json data = Json()) {
    return Status::failure(
        McpError::make(code, std::move(message), std::move(data)));
  }

  std::shared_ptr<httplib::Client> createClient() const {
    const std::string base = options_.baseUrl;
    try {
#if TUGMCP_TLS_ENABLED
      if (options_.tls.enabled) {
        std::shared_ptr<httplib::Client> client;
        if (!options_.tls.certChainPath.empty() &&
            !options_.tls.privateKeyPath.empty()) {
          client = std::make_shared<httplib::Client>(
              base, options_.tls.certChainPath,
              options_.tls.privateKeyPath);
        } else {
          client = std::make_shared<httplib::Client>(base);
        }
        if (!options_.tls.caBundlePath.empty()) {
          client->set_ca_cert_path(options_.tls.caBundlePath);
        }
        client->enable_server_certificate_verification(
            options_.tls.verifyPeer);
        client->enable_server_hostname_verification(options_.tls.verifyPeer);
        client->set_connection_timeout(std::chrono::milliseconds(options_.connectTimeoutMs));
        client->set_read_timeout(std::chrono::milliseconds(options_.readTimeoutMs));
        return client;
      }
#endif
      auto client = std::make_shared<httplib::Client>(base);
      client->set_connection_timeout(std::chrono::milliseconds(options_.connectTimeoutMs));
      client->set_read_timeout(std::chrono::milliseconds(options_.readTimeoutMs));
      return client;
    } catch (...) {
      return nullptr;
    }
  }

  void stopReader() {
    std::shared_ptr<httplib::Client> client;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      client = client_;
      client_.reset();
      started_ = false;
    }
    if (client) {
      try {
        client->stop();
      } catch (...) {
      }
    }
    if (reader_.joinable() &&
        reader_.get_id() != std::this_thread::get_id()) {
      reader_.join();
    }
  }

  void notifyClose() noexcept {
    std::function<void()> handler;
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      notify = !close_notified_.exchange(true);
      if (notify) {
        handler = std::move(on_close_);
      }
      on_message_ = nullptr;
      on_error_ = nullptr;
      on_close_ = nullptr;
      on_session_closed_ = nullptr;
    }
    if (notify && handler) {
      try {
        handler();
      } catch (...) {
      }
    }
  }

  void readerLoop() {
    std::shared_ptr<httplib::Client> client;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      client = client_;
    }
    if (!client) {
      return;
    }
    std::string line_buffer;
    std::string event_name;
    std::string data;
    bool has_data = false;
    auto dispatch = [this](const std::string& name,
                           const std::string& payload) {
      if (name == "endpoint") {
        std::string url = payload;
        if (url.rfind("http://", 0) == 0 ||
            url.rfind("https://", 0) == 0) {
          const std::size_t path =
              url.find('/', url.find("://") + 3);
          if (path != std::string::npos) {
            url = url.substr(path);
          }
        }
        {
          std::lock_guard<std::mutex> lock(endpoint_mutex_);
          messages_url_ = url;
          endpoint_ready_ = true;
        }
        endpoint_cv_.notify_all();
        return;
      }
      if (name != "message" && !name.empty()) {
        return;
      }
      auto parsed = parseStrict(payload);
      if (!parsed.hasValue()) {
        return;
      }
      std::function<void(Json)> handler;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        handler = on_message_;
      }
      if (handler) {
        try {
          handler(std::move(parsed.value()));
        } catch (...) {
        }
      }
    };
    auto process_line = [&](std::string line) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        if (has_data) {
          dispatch(event_name, data);
        }
        event_name.clear();
        data.clear();
        has_data = false;
        return;
      }
      if (line.front() == ':') {
        return;
      }
      const std::size_t colon = line.find(':');
      std::string field = line;
      std::string value;
      if (colon != std::string::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
          value.erase(value.begin());
        }
      }
      if (field == "event") {
        event_name = value;
      } else if (field == "data") {
        if (has_data) {
          data.push_back('\n');
        }
        data += value;
        has_data = true;
      }
    };

    client->Get("/sse", [&](const char* data_ptr, std::size_t length) {
      line_buffer.append(data_ptr, length);
      for (;;) {
        const std::size_t newline = line_buffer.find('\n');
        if (newline == std::string::npos) {
          return true;
        }
        std::string line = line_buffer.substr(0, newline);
        line_buffer.erase(0, newline + 1);
        process_line(std::move(line));
      }
    });
    {
      std::lock_guard<std::mutex> lock(endpoint_mutex_);
      endpoint_ready_ = true;
    }
    endpoint_cv_.notify_all();
  }

  LegacySseClientOptions options_;
  std::shared_ptr<httplib::Client> client_;
  mutable std::mutex lifecycle_mutex_;
  bool started_ = false;
  std::atomic<bool> closed_{false};
  std::atomic<bool> close_notified_{false};
  std::thread reader_;

  std::mutex callback_mutex_;
  std::function<void(Json)> on_message_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
  std::function<void(const std::string&)> on_session_closed_;

  std::mutex endpoint_mutex_;
  std::condition_variable endpoint_cv_;
  bool endpoint_ready_ = false;
  std::string messages_url_;
};

LegacySseClientTransport::LegacySseClientTransport(
    LegacySseClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LegacySseClientTransport::~LegacySseClientTransport() = default;

Status LegacySseClientTransport::start() { return impl_->start(); }
Status LegacySseClientTransport::send(const Json& message) {
  return impl_->send(message);
}
Status LegacySseClientTransport::sendTo(std::string_view route,
                                        const Json& message) {
  return impl_->sendTo(route, message);
}
void LegacySseClientTransport::setOnMessage(
    std::function<void(Json)> handler) {
  impl_->setOnMessage(std::move(handler));
}
void LegacySseClientTransport::setOnMessageEx(MessageHandlerEx handler) {
  impl_->setOnMessageEx(std::move(handler));
}
void LegacySseClientTransport::setOnError(
    std::function<void(McpError)> handler) {
  impl_->setOnError(std::move(handler));
}
void LegacySseClientTransport::setOnClose(std::function<void()> handler) {
  impl_->setOnClose(std::move(handler));
}
void LegacySseClientTransport::close() { impl_->close(); }

Result<std::shared_ptr<ITransport>> makeLegacySseServerTransport(
    const LegacySseServerOptions& options) {
  if (options.port < 0 || options.port > 65535) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams, "invalid port"));
  }
  if (options.maxBodyBytes == 0) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams, "invalid body limit"));
  }
  std::shared_ptr<ITransport> transport =
      std::make_shared<LegacySseServerTransport>(options);
  return transport;
}

Result<std::shared_ptr<ITransport>> makeLegacySseClientTransport(
    const LegacySseClientOptions& options) {
  if (options.baseUrl.empty()) {
    return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
        ErrorCode::InvalidParams, "baseUrl is required"));
  }
  std::shared_ptr<ITransport> transport =
      std::make_shared<LegacySseClientTransport>(options);
  return transport;
}

} // namespace v1
} // namespace mcp
