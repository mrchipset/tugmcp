#include "tugmcp/transport/http.hpp"

#include "tugmcp/detail/http_httplib.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <random>
#include <sstream>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tugmcp/error.hpp>
#include <tugmcp/message.hpp>

namespace mcp {
namespace v1 {
namespace {

bool isJsonContentType(const std::string& value) {
  return value.find("application/json") != std::string::npos;
}

bool acceptsEventStream(const httplib::Request& request) {
  return request.get_header_value("Accept").find("text/event-stream") !=
         std::string::npos;
}

bool hasProgressToken(const Message& message) {
  return message.params.is_object() && message.params.contains("_meta") &&
         message.params.at("_meta").is_object() &&
         message.params.at("_meta").contains("progressToken");
}

std::string asciiLower(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

std::string hostFromAuthority(const std::string& authority) {
  if (authority.empty()) {
    return {};
  }
  if (authority.front() == '[') {
    const std::size_t end = authority.find(']');
    if (end == std::string::npos) {
      return asciiLower(authority);
    }
    return asciiLower(authority.substr(1, end - 1));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos &&
      authority.find(':') == colon) {
    return asciiLower(authority.substr(0, colon));
  }
  return asciiLower(authority);
}

bool isLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool listMatches(const std::vector<std::string>& list,
                 const std::string& value) {
  const std::string candidate = asciiLower(value);
  for (const auto& item : list) {
    if (item == "*" || asciiLower(item) == candidate) {
      return true;
    }
  }
  return false;
}

bool hostAllowed(const std::string& host_header,
                 const std::vector<std::string>& allowed_hosts) {
  if (host_header.empty()) {
    return false;
  }
  const std::string host = hostFromAuthority(host_header);
  if (isLoopbackHost(host)) {
    return true;
  }
  return listMatches(allowed_hosts, host);
}

bool originAllowed(const std::string& origin,
                   const std::vector<std::string>& allowed_origins) {
  if (origin.empty()) {
    return true;
  }
  if (origin == "null") {
    return false;
  }
  const std::size_t scheme_end = origin.find("://");
  const std::string authority =
      scheme_end == std::string::npos
          ? origin
          : origin.substr(scheme_end + 3);
  const std::size_t path = authority.find('/');
  const std::string host =
      hostFromAuthority(path == std::string::npos
                            ? authority
                            : authority.substr(0, path));
  if (isLoopbackHost(host)) {
    return true;
  }
  return listMatches(allowed_origins, origin);
}

#if TUGMCP_TLS_ENABLED
bool isReadableFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return stream.good();
}
#endif

Result<std::shared_ptr<ITransport>> optionFailure(const char* reason,
                                                  std::string message) {
  return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
      ErrorCode::InvalidParams, std::move(message),
      Json{{"reason", reason}}));
}

} // namespace

class HttpServerTransport::Impl {
public:
  explicit Impl(StreamableHttpServerOptions options)
      : options_(std::move(options)),
        supported_versions_(options_.supportedVersions) {}

  void setSupportedVersions(std::vector<std::string> versions) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    supported_versions_ = std::move(versions);
  }

  bool isSupportedVersion(const std::string& version) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return std::find(supported_versions_.begin(),
                     supported_versions_.end(),
                     version) != supported_versions_.end();
  }

  std::vector<std::string> supportedVersions() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return supported_versions_;
  }

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
    const std::size_t pool =
        options_.maxConcurrentStreams == 0
            ? 16
            : std::max<std::size_t>(8, options_.maxConcurrentStreams + 4);
    server_->new_task_queue = [pool] {
      return new httplib::ThreadPool(pool);
    };
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
    auto decode_status = decodeOutgoing(message, decoded);
    if (!decode_status) {
      return decode_status;
    }
    if (decoded.kind != MessageKind::Response) {
      return Status::success();
    }

    std::shared_ptr<Pending> match;
    std::size_t matches = 0;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      for (auto& item : pending_) {
        auto& pending = item.second;
        if (!pending->done && pending->id == decoded.id) {
          match = pending;
          ++matches;
        }
      }
      if (matches == 1) {
        match->done = true;
        match->response = message;
      }
    }
    if (matches > 1) {
      return failure(ErrorCode::InternalError, "ambiguous route");
    }
    if (matches == 1) {
      pending_cv_.notify_all();
    }
    return Status::success();
  }

  Status sendTo(std::string_view route, const Json& message) {
    if (closed_.load(std::memory_order_acquire)) {
      return failure(ErrorCode::InternalError, "transport is closed");
    }

    Message decoded;
    auto decode_status = decodeOutgoing(message, decoded);
    if (!decode_status) {
      return decode_status;
    }

    if (decoded.kind == MessageKind::Response) {
      std::shared_ptr<SseStream> stream;
      bool completed = false;
      {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        const auto pending_it = pending_.find(std::string(route));
        if (pending_it != pending_.end()) {
          auto& pending = pending_it->second;
          if (!pending->done) {
            pending->done = true;
            pending->response = message;
          }
          completed = true;
        } else {
          const auto stream_it = streams_.find(std::string(route));
          if (stream_it != streams_.end()) {
            stream = stream_it->second;
          }
        }
      }
      if (completed) {
        pending_cv_.notify_all();
        return Status::success();
      }
      if (stream) {
        return enqueueStream(stream, message, true);
      }
      return routeNotFound();
    }

    if (decoded.kind == MessageKind::Notification) {
      std::shared_ptr<SseStream> stream;
      bool json_route = false;
      {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        const auto stream_it = streams_.find(std::string(route));
        if (stream_it != streams_.end()) {
          stream = stream_it->second;
        } else {
          json_route = pending_.find(std::string(route)) != pending_.end();
        }
      }
      if (stream) {
        return enqueueStream(stream, message, false);
      }
      if (json_route) {
        return Status::success();
      }
      return routeNotFound();
    }

    return failure(ErrorCode::InternalError, "unexpected outgoing message");
  }

  Status sendToSession(std::string_view sessionId, const Json& message) {
    sweepSessions();
    ReplayEvent event;
    {
      std::lock_guard<std::mutex> session_lock(sessions_mutex_);
      const auto it = sessions_.find(std::string(sessionId));
      if (it == sessions_.end() || !it->second->stream) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "no session stream",
            Json{{"reason", "no_session_stream"}}));
      }
      auto& session = it->second;
      event.id = session->next_event_id++;
      event.message = message;
      session->last_seen = now();

      std::lock_guard<std::mutex> stream_lock(session->stream->mutex);
      if (session->stream->closed) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "no session stream",
            Json{{"reason", "no_session_stream"}}));
      }
      session->stream->frames.push_back(event);
      session->replay.push_back(event);
      while (session->replay.size() > 256) {
        session->replay.pop_front();
      }
      session->stream->cv.notify_all();
    }
    return Status::success();
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

  void setOnRouteClosed(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_route_closed_ = std::move(handler);
  }

  void setOnSessionClosed(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_session_closed_ = std::move(handler);
  }

  void close() noexcept {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
      notifyClose();
      return;
    }

    std::vector<std::shared_ptr<SseStream>> streams;
    std::vector<std::shared_ptr<GetStream>> get_streams;
    std::vector<std::string> pending_routes;
    std::vector<std::string> session_ids;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      for (auto& item : pending_) {
        auto& pending = item.second;
        if (!pending->done) {
          pending->done = true;
          pending->closed = true;
        }
        pending_routes.push_back(item.first);
      }
      streams.reserve(streams_.size());
      for (auto& item : streams_) {
        streams.push_back(item.second);
      }
      get_streams.reserve(get_streams_.size());
      for (auto& item : get_streams_) {
        get_streams.push_back(item.second);
      }
    }
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      session_ids.reserve(sessions_.size());
      for (const auto& item : sessions_) {
        session_ids.push_back(item.first);
      }
      sessions_.clear();
    }
    for (const auto& id : session_ids) {
      notifySessionClosed(id);
    }
    pending_cv_.notify_all();
    for (const auto& route : pending_routes) {
      notifyRouteClosed(route);
    }

    for (const auto& stream : streams) {
      std::lock_guard<std::mutex> lock(stream->mutex);
      stream->closed = true;
      stream->cv.notify_all();
    }
    for (const auto& stream : get_streams) {
      closeGetStream(stream);
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

    {
      std::unique_lock<std::mutex> lock(routes_mutex_);
      active_streams_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
        return active_streams_.load(std::memory_order_acquire) == 0;
      });
    }

    notifyClose();
  }

  int boundPort() const {
    return bound_port_.load(std::memory_order_acquire);
  }

private:
  struct Pending {
    RequestId id;
    bool done = false;
    bool closed = false;
    Json response;
  };

  struct SseStream {
    std::string route;
    RequestId request_id;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Json> frames;
    bool done = false;
    bool overflow = false;
    bool closed = false;
    std::atomic<bool> released{false};
  };

  struct ReplayEvent {
    std::uint64_t id = 0;
    Json message;
  };

  struct GetStream {
    std::string route;
    std::string session_id;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<ReplayEvent> frames;
    bool closed = false;
    bool finished = false;
    std::atomic<bool> released{false};
  };

  struct Session {
    std::string id;
    std::chrono::steady_clock::time_point last_seen;
    std::shared_ptr<GetStream> stream;
    Json clientCapabilities;
    std::uint64_t next_event_id = 1;
    std::deque<ReplayEvent> replay;
  };

  static Status failure(ErrorCode code, std::string message) {
    return Status::failure(McpError::make(code, std::move(message)));
  }

  Status routeNotFound() const {
    return Status::failure(McpError::make(
        ErrorCode::InternalError, "route not found",
        Json{{"reason", "route_not_found"}}));
  }

  static Status decodeOutgoing(const Json& message, Message& decoded) {
    try {
      auto result = decode(message);
      if (!result.hasValue()) {
        return failure(ErrorCode::InternalError, "invalid response message");
      }
      decoded = std::move(result.value());
      return Status::success();
    } catch (...) {
      return failure(ErrorCode::InternalError, "invalid response message");
    }
  }

  std::string generateSessionId() {
    const std::uint64_t sequence =
        session_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
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

  void closeGetStream(const std::shared_ptr<GetStream>& stream) noexcept {
    if (!stream) {
      return;
    }
    try {
      std::lock_guard<std::mutex> lock(stream->mutex);
      stream->closed = true;
      stream->cv.notify_all();
    } catch (...) {
    }
  }

  void sweepSessions() {
    if (options_.sessionTimeout.count() <= 0) {
      return;
    }
    std::vector<std::shared_ptr<GetStream>> expired;
    std::vector<std::string> expired_ids;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      const auto current = now();
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (current - it->second->last_seen >
            options_.sessionTimeout) {
          if (it->second->stream) {
            expired.push_back(it->second->stream);
          }
          expired_ids.push_back(it->first);
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const auto& stream : expired) {
      closeGetStream(stream);
    }
    for (const auto& id : expired_ids) {
      notifySessionClosed(id);
    }
  }

  std::shared_ptr<Session> findSession(const std::string& id) {
    sweepSessions();
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
      return nullptr;
    }
    it->second->last_seen = now();
    return it->second;
  }

  std::shared_ptr<Session> createSession() {
    sweepSessions();
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (options_.maxSessions != 0 &&
        sessions_.size() >= options_.maxSessions) {
      return nullptr;
    }
    auto session = std::make_shared<Session>();
    session->id = generateSessionId();
    session->last_seen = now();
    sessions_[session->id] = session;
    return session;
  }

  void removeSession(const std::string& id) {
    std::shared_ptr<GetStream> stream;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) {
        return;
      }
      stream = it->second->stream;
      sessions_.erase(it);
    }
    closeGetStream(stream);
    notifySessionClosed(id);
  }

  void releaseGetStream(const std::string& route,
                        const std::shared_ptr<GetStream>& stream) noexcept {
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      const auto it = get_streams_.find(route);
      if (it != get_streams_.end() && it->second == stream) {
        get_streams_.erase(it);
      }
    }
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      const auto it = sessions_.find(stream->session_id);
      if (it != sessions_.end() && it->second->stream == stream) {
        it->second->stream.reset();
      }
    }
    notifyRouteClosed(route);
    bool expected = false;
    if (stream->released.compare_exchange_strong(expected, true)) {
      active_streams_.fetch_sub(1, std::memory_order_acq_rel);
      active_streams_cv_.notify_all();
    }
  }

  bool getStreamProvider(const std::shared_ptr<GetStream>& stream,
                         httplib::DataSink& sink) {
    auto next_keepalive =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
      ReplayEvent event;
      bool have_event = false;
      {
        std::unique_lock<std::mutex> lock(stream->mutex);
        if (stream->frames.empty() && !stream->closed) {
          stream->cv.wait_for(lock, std::chrono::milliseconds(25), [&] {
            return stream->closed || !stream->frames.empty();
          });
        }
        if (stream->closed) {
          return false;
        }
        if (stream->frames.empty() && sink.is_writable &&
            !sink.is_writable()) {
          stream->closed = true;
          stream->cv.notify_all();
          return false;
        }
        if (!stream->frames.empty()) {
          event = std::move(stream->frames.front());
          stream->frames.pop_front();
          have_event = true;
        }
      }

      if (!have_event) {
        const auto current = std::chrono::steady_clock::now();
        if (current >= next_keepalive) {
          next_keepalive = current + std::chrono::milliseconds(500);
          static const char keepalive[] = ": keepalive\n\n";
          if (!sink.write(keepalive, sizeof(keepalive) - 1)) {
            closeGetStream(stream);
            return false;
          }
        }
        continue;
      }

      std::string encoded;
      try {
        encoded = "id: " + std::to_string(event.id) +
                  "\nevent: message\ndata: " + event.message.dump() +
                  "\n\n";
      } catch (...) {
        closeGetStream(stream);
        return false;
      }
      if (!sink.write(encoded.data(), encoded.size())) {
        closeGetStream(stream);
        return false;
      }
    }
  }

  void handleGet(const httplib::Request& request,
                 httplib::Response& response) {
    try {
      if (!hostAllowed(request.get_header_value("Host"),
                       options_.allowedHosts) ||
          !originAllowed(request.get_header_value("Origin"),
                         options_.allowedOrigins)) {
        response.status = 403;
        return;
      }
      const std::string version =
          request.get_header_value("MCP-Protocol-Version");
      if (version == "2026-07-28") {
        response.status = 405;
        return;
      }
      const std::string session_id =
          request.get_header_value("MCP-Session-Id");
      if (session_id.empty()) {
        response.status = 400;
        return;
      }
      auto session = findSession(session_id);
      if (!session) {
        response.status = 404;
        return;
      }

      const std::string last_event =
          request.get_header_value("Last-Event-ID");
      std::uint64_t last_id = 0;
      if (!last_event.empty()) {
        bool valid = true;
        std::uint64_t value = 0;
        for (const char character : last_event) {
          if (character < '0' || character > '9') {
            valid = false;
            break;
          }
          if (value > (std::numeric_limits<std::uint64_t>::max() -
                       static_cast<std::uint64_t>(character - '0')) /
                          10) {
            valid = false;
            break;
          }
          value = value * 10 + static_cast<std::uint64_t>(character - '0');
        }
        last_id = valid ? value : 0;
      }

      auto stream = std::make_shared<GetStream>();
      stream->route = nextRoute();
      stream->session_id = session_id;

      std::vector<ReplayEvent> replay;
      std::shared_ptr<GetStream> previous;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
          response.status = 404;
          return;
        }
        auto& current = it->second;
        current->last_seen = now();
        if (!current->replay.empty()) {
          const std::uint64_t first = current->replay.front().id;
          const bool replay_all = last_id == 0 || last_id + 1 < first;
          for (const auto& event : current->replay) {
            if (replay_all || event.id > last_id) {
              replay.push_back(event);
            }
          }
        }
        previous = current->stream;
        current->stream = stream;
      }

      const std::uint64_t max_replay =
          replay.empty() ? 0 : replay.back().id;
      if (previous) {
        std::unique_lock<std::mutex> old_lock(previous->mutex);
        std::lock_guard<std::mutex> new_lock(stream->mutex);
        for (auto& event : replay) {
          stream->frames.push_back(std::move(event));
        }
        for (auto& event : previous->frames) {
          if (event.id > max_replay) {
            stream->frames.push_back(std::move(event));
          }
        }
        previous->frames.clear();
        old_lock.unlock();
        closeGetStream(previous);
      } else {
        std::lock_guard<std::mutex> lock(stream->mutex);
        for (auto& event : replay) {
          stream->frames.push_back(std::move(event));
        }
      }
      active_streams_.fetch_add(1, std::memory_order_acq_rel);
      {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        get_streams_[stream->route] = stream;
      }

      response.set_header("Cache-Control", "no-cache");
      response.status = 200;
      const std::string route = stream->route;
      response.set_chunked_content_provider(
          "text/event-stream",
          [this, stream](std::size_t, httplib::DataSink& sink) {
            return getStreamProvider(stream, sink);
          },
          [this, route, stream](bool) { releaseGetStream(route, stream); });
    } catch (...) {
      response.status = 500;
    }
  }

  void handleDelete(const httplib::Request& request,
                    httplib::Response& response) {
    try {
      if (!hostAllowed(request.get_header_value("Host"),
                       options_.allowedHosts) ||
          !originAllowed(request.get_header_value("Origin"),
                         options_.allowedOrigins)) {
        response.status = 403;
        return;
      }
      const std::string version =
          request.get_header_value("MCP-Protocol-Version");
      if (version == "2026-07-28") {
        response.status = 405;
        return;
      }
      const std::string session_id =
          request.get_header_value("MCP-Session-Id");
      if (session_id.empty()) {
        response.status = 400;
        return;
      }
      auto session = findSession(session_id);
      if (!session) {
        response.status = 404;
        return;
      }
      removeSession(session_id);
      response.status = 204;
    } catch (...) {
      response.status = 500;
    }
  }

  void registerHandlers() {
    server_->Post("/mcp",
                  [this](const httplib::Request& request,
                         httplib::Response& response) {
                    handlePost(request, response);
                  });
    server_->Get("/mcp",
                 [this](const httplib::Request& request,
                        httplib::Response& response) {
                   handleGet(request, response);
                 });
    server_->Delete("/mcp",
                    [this](const httplib::Request& request,
                           httplib::Response& response) {
                      handleDelete(request, response);
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

  std::string nextRoute() {
    const std::uint64_t value =
        route_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    return "r" + std::to_string(value);
  }

  bool tryAcquireStream() {
    const std::size_t maximum = options_.maxConcurrentStreams;
    if (maximum == 0) {
      active_streams_.fetch_add(1, std::memory_order_acq_rel);
      return true;
    }
    std::size_t current = active_streams_.load(std::memory_order_acquire);
    for (;;) {
      if (current >= maximum) {
        return false;
      }
      if (active_streams_.compare_exchange_weak(
              current, current + 1, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return true;
      }
    }
  }

  void notifySessionClosed(const std::string& session_id) noexcept {
    std::function<void(const std::string&)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_session_closed_;
    }
    if (handler) {
      try {
        handler(session_id);
      } catch (...) {
      }
    }
  }

  void notifyRouteClosed(const std::string& route) noexcept {
    std::function<void(const std::string&)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_route_closed_;
    }
    if (handler) {
      try {
        handler(route);
      } catch (...) {
      }
    }
  }

  void releaseStream(const std::string& route,
                     const std::shared_ptr<SseStream>& stream) noexcept {
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      const auto it = streams_.find(route);
      if (it != streams_.end() && it->second == stream) {
        streams_.erase(it);
      }
    }
    notifyRouteClosed(route);
    bool expected = false;
    if (stream->released.compare_exchange_strong(expected, true)) {
      active_streams_.fetch_sub(1, std::memory_order_acq_rel);
      active_streams_cv_.notify_all();
    }
  }

  Status enqueueStream(const std::shared_ptr<SseStream>& stream,
                       const Json& message, bool final) {
    bool overflowed = false;
    {
      std::lock_guard<std::mutex> lock(stream->mutex);
      if (stream->closed) {
        return failure(ErrorCode::InternalError, "stream closed");
      }
      if (stream->done) {
        if (stream->overflow) {
          return Status::failure(McpError::make(
              ErrorCode::InternalError, "stream overflow",
              Json{{"reason", "stream_overflow"}}));
        }
        return failure(ErrorCode::InternalError, "stream closed");
      }
      if (!final && options_.perStreamQueue != 0 &&
          stream->frames.size() >= options_.perStreamQueue) {
        stream->frames.push_back(errorResponseJson(
            stream->request_id,
            McpError::make(ErrorCode::InternalError, "stream overflow",
                           Json{{"reason", "stream_overflow"}})));
        stream->overflow = true;
        stream->done = true;
        stream->cv.notify_all();
        overflowed = true;
      } else {
        stream->frames.push_back(message);
        if (final) {
          stream->done = true;
        }
        stream->cv.notify_all();
      }
    }

    if (overflowed) {
      reportError(McpError::make(
          ErrorCode::InternalError, "stream overflow",
          Json{{"reason", "stream_overflow"}}));
      return failure(ErrorCode::InternalError, "stream overflow");
    }
    return Status::success();
  }

  bool streamProvider(const std::shared_ptr<SseStream>& stream,
                      httplib::DataSink& sink) {
    auto next_keepalive =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
      Json frame;
      bool have_frame = false;
      {
        std::unique_lock<std::mutex> lock(stream->mutex);
        if (stream->frames.empty() && !stream->done && !stream->closed) {
          stream->cv.wait_for(lock, std::chrono::milliseconds(25), [&] {
            return stream->closed || !stream->frames.empty() || stream->done;
          });
        }
        if (stream->closed) {
          return false;
        }
        if (stream->frames.empty() && !stream->done &&
            sink.is_writable && !sink.is_writable()) {
          stream->closed = true;
          stream->cv.notify_all();
          return false;
        }
        if (!stream->frames.empty()) {
          frame = std::move(stream->frames.front());
          stream->frames.pop_front();
          have_frame = true;
        } else if (stream->done || stream->overflow) {
          // cpp-httplib requires sink.done() to emit the terminating chunk;
          // returning false without it makes the peer see a read error.
          sink.done();
          return true;
        }
      }

      if (!have_frame) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_keepalive) {
          next_keepalive = now + std::chrono::milliseconds(500);
          static const char keepalive[] = ": keepalive\n\n";
          if (!sink.write(keepalive, sizeof(keepalive) - 1)) {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->closed = true;
            stream->cv.notify_all();
            return false;
          }
        }
        continue;
      }

      std::string encoded;
      try {
        encoded = "event: message\ndata: " + frame.dump() + "\n\n";
      } catch (...) {
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->closed = true;
        stream->cv.notify_all();
        return false;
      }
      if (!sink.write(encoded.data(), encoded.size())) {
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->closed = true;
        stream->cv.notify_all();
        return false;
      }
    }
  }

  void respondJson(httplib::Response& response, int status,
                   const Json& body) {
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
      on_message_ex_ = nullptr;
      on_error_ = nullptr;
      on_close_ = nullptr;
      on_route_closed_ = nullptr;
      on_session_closed_ = nullptr;
    }
    if (!notify || !handler) {
      return;
    }
    try {
      handler();
    } catch (...) {
    }
  }

  bool hasMessageHandler() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return static_cast<bool>(on_message_ex_) ||
           static_cast<bool>(on_message_);
  }

  bool invokeMessage(const Json& message, const MessageMetadata& metadata) {
    MessageHandlerEx extended;
    std::function<void(Json)> simple;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      extended = on_message_ex_;
      simple = on_message_;
    }
    if (!extended && !simple) {
      return false;
    }
    try {
      if (extended) {
        extended(message, metadata);
      } else {
        simple(message);
      }
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

  static Json errorResponseJson(const RequestId& id, const McpError& error) {
    Json body;
    body["jsonrpc"] = "2.0";
    body["id"] = id.toJson();
    body["error"] = errorToJson(error);
    return body;
  }

  void erasePending(const std::string& route) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    pending_.erase(route);
  }

  void handlePost(const httplib::Request& request,
                  httplib::Response& response) {
    try {
      if (!hostAllowed(request.get_header_value("Host"),
                       options_.allowedHosts) ||
          !originAllowed(request.get_header_value("Origin"),
                         options_.allowedOrigins)) {
        response.status = 403;
        return;
      }

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

      auto decoded = decode(request.body);
      if (!decoded.hasValue()) {
        respondError(response, 400, decoded.error());
        return;
      }

      const Message& message = decoded.value();
      const bool is_2025 =
          version.empty() || version == "2025-11-25";
      const std::string resolved_version =
          version.empty() ? std::string("2025-11-25") : version;
      if (!isSupportedVersion(resolved_version)) {
        respondError(response, 400,
                     McpError::make(
                         ErrorCode::UnsupportedVersion,
                         "unsupported protocol version: " + version,
                         Json{{"requested", version},
                              {"supported", supportedVersions()}}),
                     message.id.toJson());
        return;
      }
      if (message.kind == MessageKind::Response && !is_2025) {
        respondError(response, 400,
                     McpError::make(ErrorCode::InvalidRequest,
                                    "response is not a request"),
                     message.id.toJson());
        return;
      }

      if (version == "2026-07-28") {
        // Transport-level precheck mirrors the dispatcher validation so D18
        // HTTP status mapping can happen before dispatch.
        auto reject_meta = [&](const McpError& error) {
          if (message.kind == MessageKind::Notification) {
            response.status = 400;
            response.set_content("", "application/json");
          } else {
            respondError(response, 400, error, message.id.toJson());
          }
        };
        if (!message.params.is_object() ||
            !message.params.contains("_meta") ||
            !message.params.at("_meta").is_object()) {
          reject_meta(McpError::make(
              ErrorCode::InvalidParams,
              "2026 requests require params._meta"));
          return;
        }
        const Json& meta = message.params.at("_meta");
        if (!meta.contains("io.modelcontextprotocol/protocolVersion") ||
            !meta.at("io.modelcontextprotocol/protocolVersion")
                 .is_string() ||
            !meta.contains(
                "io.modelcontextprotocol/clientCapabilities") ||
            !meta.at("io.modelcontextprotocol/clientCapabilities")
                 .is_object()) {
          reject_meta(McpError::make(
              ErrorCode::InvalidParams,
              "params._meta is missing required 2026 fields"));
          return;
        }
        if (meta.at("io.modelcontextprotocol/protocolVersion")
                .get<std::string>() != version) {
          reject_meta(McpError::make(
              ErrorCode::HeaderMismatch,
              "params._meta protocolVersion mismatch"));
          return;
        }
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

      auto parsed = parseStrict(request.body);
      if (!parsed.hasValue()) {
        respondError(response, 400, parsed.error());
        return;
      }
      const Json& document = parsed.value();

      std::string session_id;
      std::shared_ptr<Session> session_state;
      if (is_2025 && message.method != "initialize") {
        session_id = request.get_header_value("MCP-Session-Id");
        if (session_id.empty()) {
          respondError(response, 400,
                       McpError::make(ErrorCode::InvalidRequest,
                                      "missing session id",
                                      Json{{"reason", "missing_session"}}),
                       message.id.toJson());
          return;
        }
        session_state = findSession(session_id);
        if (!session_state) {
          response.status = 404;
          return;
        }
      }
      const Json client_capabilities =
          session_state ? session_state->clientCapabilities
                        : Json::object();

      bool emergency = false;
      if (options_.admission) {
        RequestContext context;
        context.id = message.id;
        context.method = message.method;
        AdmissionDecision decision = AdmissionDecision::Accept;
        try {
          decision = options_.admission->admit(context, document);
        } catch (...) {
          respondError(response, 500,
                       McpError::make(ErrorCode::InternalError,
                                      "admission hook failed"));
          return;
        }
        if (decision == AdmissionDecision::Reject) {
          response.status = 429;
          return;
        }
        emergency = decision == AdmissionDecision::Emergency;
      }

      if (message.kind == MessageKind::Response) {
        if (hasMessageHandler()) {
          MessageMetadata metadata;
          metadata.protocolVersion = version;
          metadata.sessionId = session_id;
          metadata.clientCapabilities = client_capabilities;
          metadata.emergency = emergency;
          invokeMessage(document, metadata);
        }
        response.status = 202;
        response.set_content("", "application/json");
        return;
      }

      if (!hasMessageHandler()) {
        if (message.kind == MessageKind::Notification) {
          response.status = 202;
          response.set_content("", "application/json");
          return;
        }
        respondError(response, 500,
                     McpError::make(ErrorCode::InternalError,
                                    "message handler unavailable"),
                     message.id.toJson());
        return;
      }

      if (message.kind == MessageKind::Notification) {
        const std::string route = nextRoute();
        {
          std::lock_guard<std::mutex> lock(routes_mutex_);
          if (closed_.load(std::memory_order_acquire)) {
            respondError(response, 503,
                         McpError::make(ErrorCode::InternalError,
                                        "transport is closed"),
                         message.id.toJson());
            return;
          }
          auto pending = std::make_shared<Pending>();
          pending->id = message.id;
          pending_[route] = std::move(pending);
        }

        MessageMetadata metadata;
        metadata.protocolVersion = version;
        metadata.route = route;
        metadata.sessionId = session_id;
          metadata.clientCapabilities = client_capabilities;
        metadata.emergency = emergency;
        const bool invoked = invokeMessage(document, metadata);
        erasePending(route);
        if (!invoked) {
          respondError(response, 500,
                       McpError::make(ErrorCode::InternalError,
                                      "message handler unavailable"));
          return;
        }
        response.status = 202;
        response.set_content("", "application/json");
        return;
      }

      const bool wants_sse =
          acceptsEventStream(request) &&
          (hasProgressToken(message) ||
           message.method == "subscriptions/listen");

      if (wants_sse) {
        auto stream = std::make_shared<SseStream>();
        stream->route = nextRoute();
        stream->request_id = message.id;
        const std::string route = stream->route;

        if (!tryAcquireStream()) {
          response.status = 429;
          return;
        }

        try {
          {
            std::lock_guard<std::mutex> lock(routes_mutex_);
            streams_[route] = stream;
          }

          response.set_header("Cache-Control", "no-cache");
          response.status = 200;
          response.set_chunked_content_provider(
              "text/event-stream",
              [this, stream](std::size_t, httplib::DataSink& sink) {
                return streamProvider(stream, sink);
              },
              [this, route, stream](bool) {
                releaseStream(route, stream);
              });
        } catch (...) {
          releaseStream(route, stream);
          throw;
        }

        MessageMetadata metadata;
        metadata.protocolVersion = version;
        metadata.route = route;
        metadata.sessionId = session_id;
          metadata.clientCapabilities = client_capabilities;
        metadata.emergency = emergency;
        if (!invokeMessage(document, metadata)) {
          const McpError error = McpError::make(
              ErrorCode::InternalError, "message handler unavailable");
          enqueueStream(stream, errorResponseJson(message.id, error), true);
        }
        return;
      }

      const std::string route = nextRoute();
      auto pending = std::make_shared<Pending>();
      pending->id = message.id;
      {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        if (closed_.load(std::memory_order_acquire)) {
          respondError(response, 503,
                       McpError::make(ErrorCode::InternalError,
                                      "transport is closed"),
                       message.id.toJson());
          return;
        }
        pending_[route] = pending;
      }

      const auto deadline = now() + options_.requestTimeout;

      MessageMetadata metadata;
      metadata.protocolVersion = version;
      metadata.route = route;
      metadata.sessionId = session_id;
          metadata.clientCapabilities = client_capabilities;
      metadata.emergency = emergency;
      if (!invokeMessage(document, metadata)) {
        erasePending(route);
        respondError(response, 500,
                     McpError::make(ErrorCode::InternalError,
                                    "message handler unavailable"),
                     message.id.toJson());
        return;
      }

      std::unique_lock<std::mutex> lock(routes_mutex_);
      const bool completed = pending_cv_.wait_until(
          lock, deadline, [&pending] { return pending->done; });

      if (!completed) {
        pending_.erase(route);
        lock.unlock();
        notifyRouteClosed(route);
        respondError(response, 504,
                     McpError::make(ErrorCode::InternalError,
                                    "request timed out"),
                     message.id.toJson());
        return;
      }
      if (pending->closed) {
        pending_.erase(route);
        lock.unlock();
        notifyRouteClosed(route);
        respondError(response, 503,
                     McpError::make(ErrorCode::InternalError,
                                    "transport is closed"),
                     message.id.toJson());
        return;
      }

      Json body = pending->response;
      pending_.erase(route);
      lock.unlock();

      if (is_2025 && message.kind == MessageKind::Request &&
          message.method == "initialize" && body.is_object() &&
          body.contains("result")) {
        auto session = createSession();
        if (!session) {
          response.status = 429;
          return;
        }
        if (message.params.is_object() &&
            message.params.contains("capabilities") &&
            message.params.at("capabilities").is_object()) {
          session->clientCapabilities =
              message.params.at("capabilities");
        }
        response.set_header("MCP-Session-Id", session->id);
      }

      int response_status = 200;
      if (version == "2026-07-28" && body.is_object() &&
          body.contains("error") && body.at("error").is_object() &&
          body.at("error").contains("code") &&
          body.at("error").at("code").is_number_integer()) {
        const int code = body.at("error").at("code").get<int>();
        if (code == -32601) {
          response_status = 404;
        } else if (code == -32602 || code == -32020 ||
                   code == -32021 || code == -32022) {
          response_status = 400;
        }
      }
      respondJson(response, response_status, body);
    } catch (...) {
      respondError(response, 500,
                   McpError::make(ErrorCode::InternalError,
                                  "internal server error"));
    }
  }

  StreamableHttpServerOptions options_;
  std::unique_ptr<httplib::Server> server_;

  mutable std::mutex lifecycle_mutex_;
  std::vector<std::string> supported_versions_;
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
  std::function<void(const std::string&)> on_route_closed_;
  std::function<void(const std::string&)> on_session_closed_;

  std::mutex routes_mutex_;
  std::condition_variable pending_cv_;
  std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
  std::unordered_map<std::string, std::shared_ptr<SseStream>> streams_;
  std::unordered_map<std::string, std::shared_ptr<GetStream>> get_streams_;
  std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::atomic<std::uint64_t> session_counter_{0};
  std::atomic<std::size_t> active_streams_{0};
  std::condition_variable active_streams_cv_;
  std::atomic<std::uint64_t> route_counter_{0};
};

HttpServerTransport::HttpServerTransport(StreamableHttpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

HttpServerTransport::~HttpServerTransport() = default;

Status HttpServerTransport::start() { return impl_->start(); }

Status HttpServerTransport::send(const Json& message) {
  return impl_->send(message);
}

Status HttpServerTransport::sendTo(std::string_view route,
                                   const Json& message) {
  return impl_->sendTo(route, message);
}

Status HttpServerTransport::sendToSession(std::string_view sessionId,
                                          const Json& message) {
  return impl_->sendToSession(sessionId, message);
}

void HttpServerTransport::setOnMessage(
    std::function<void(Json)> handler) {
  impl_->setOnMessage(std::move(handler));
}

void HttpServerTransport::setOnMessageEx(MessageHandlerEx handler) {
  impl_->setOnMessageEx(std::move(handler));
}

void HttpServerTransport::setOnError(
    std::function<void(McpError)> handler) {
  impl_->setOnError(std::move(handler));
}

void HttpServerTransport::setOnClose(std::function<void()> handler) {
  impl_->setOnClose(std::move(handler));
}

void HttpServerTransport::setOnRouteClosed(
    std::function<void(const std::string&)> handler) {
  impl_->setOnRouteClosed(std::move(handler));
}

void HttpServerTransport::setSupportedVersions(
    std::vector<std::string> versions) {
  impl_->setSupportedVersions(std::move(versions));
}

void HttpServerTransport::setOnSessionClosed(
    std::function<void(const std::string&)> handler) {
  impl_->setOnSessionClosed(std::move(handler));
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
  if (options.tls.enabled) {
#if !TUGMCP_TLS_ENABLED
    return optionFailure("tls_not_built",
                         "TLS is not enabled in this build");
#else
    if (options.tls.certChainPath.empty() ||
        options.tls.privateKeyPath.empty()) {
      return optionFailure("tls_not_configured",
                           "TLS requires a certificate chain and private key");
    }
    if (!isReadableFile(options.tls.certChainPath) ||
        !isReadableFile(options.tls.privateKeyPath) ||
        (!options.tls.clientCaFile.empty() &&
         !isReadableFile(options.tls.clientCaFile))) {
      return optionFailure("option_invalid",
                           "TLS certificate or key file is not readable");
    }
#endif
  }

  std::shared_ptr<ITransport> transport(new HttpServerTransport(options));
  return transport;
}

} // namespace v1
} // namespace mcp
