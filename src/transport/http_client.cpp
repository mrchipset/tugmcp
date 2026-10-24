#include "tugmcp/transport/http.hpp"

#include "tugmcp/detail/http_httplib.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <tugmcp/error.hpp>
#include <tugmcp/message.hpp>

namespace mcp {
namespace v1 {
namespace {

std::string normalizeBaseUrl(const std::string& url) {
  std::string result = url;
  while (!result.empty() && result.back() == '/') {
    result.pop_back();
  }
  return result;
}

std::string schemeOf(const std::string& url) {
  const auto pos = url.find("://");
  if (pos == std::string::npos) {
    return {};
  }
  std::string scheme = url.substr(0, pos);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return scheme;
}

McpError reasonError(const char* message, const char* reason) {
  return McpError::make(ErrorCode::InternalError, message,
                        Json{{"reason", reason}});
}

Result<std::shared_ptr<ITransport>> optionFailure(const char* reason,
                                                  std::string message) {
  return Result<std::shared_ptr<ITransport>>::failure(McpError::make(
      ErrorCode::InvalidParams, std::move(message),
      Json{{"reason", reason}}));
}

bool isTlsError(httplib::Error error) {
  switch (error) {
  case httplib::Error::SSLConnection:
  case httplib::Error::SSLLoadingCerts:
  case httplib::Error::SSLServerVerification:
  case httplib::Error::SSLServerHostnameVerification:
    return true;
  default:
    return false;
  }
}

// Incrementally parses either a single JSON body or an SSE stream. SSE rules:
// CRLF/LF line endings, ':' comments ignored, only `data:` fields contribute,
// multiple data lines are joined with '\n', and a blank line dispatches the
// accumulated event. Unknown fields (including `event:`) are ignored.
class ResponseStreamParser {
public:
  using EventCallback =
      std::function<void(Json, std::uint64_t last_event_id)>;

  explicit ResponseStreamParser(EventCallback dispatch)
      : dispatch_(std::move(dispatch)) {}

  bool feed(const char* data, std::size_t length) {
    if (error_.has_value()) {
      return false;
    }

    if (!decided_) {
      pending_.append(data, length);
      std::size_t index = 0;
      while (index < pending_.size() &&
             std::isspace(static_cast<unsigned char>(pending_[index])) != 0) {
        ++index;
      }
      if (index == pending_.size()) {
        return true;
      }
      decided_ = true;
      if (pending_[index] == '{') {
        json_mode_ = true;
        json_.append(pending_);
        pending_.clear();
        return true;
      }
      std::string initial;
      initial.swap(pending_);
      return feedSse(initial);
    }

    if (json_mode_) {
      json_.append(data, length);
      return true;
    }
    return feedSse(std::string(data, length));
  }

  bool finish() {
    if (error_.has_value()) {
      return false;
    }
    if (!decided_) {
      // Nothing but whitespace was received.
      return true;
    }
    if (json_mode_) {
      return json_.empty() ? true : parseAndDispatch(json_);
    }
    if (!line_buffer_.empty()) {
      std::string line;
      line.swap(line_buffer_);
      if (!processLine(line)) {
        return false;
      }
    }
    return dispatchEvent();
  }

  bool dispatched() const noexcept { return dispatched_; }

  const std::optional<McpError>& error() const noexcept { return error_; }

private:
  bool feedSse(std::string input) {
    line_buffer_.append(input);
    for (;;) {
      const std::size_t newline = line_buffer_.find('\n');
      if (newline == std::string::npos) {
        return true;
      }
      std::string line = line_buffer_.substr(0, newline);
      line_buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!processLine(line)) {
        return false;
      }
    }
  }

  bool processLine(const std::string& line) {
    if (line.empty()) {
      return dispatchEvent();
    }
    if (line.front() == ':') {
      return true;
    }

    std::string field = line;
    std::string value;
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      field = line.substr(0, colon);
      value = line.substr(colon + 1);
      if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
    }

    if (field == "id") {
      bool valid = !value.empty();
      std::uint64_t parsed = 0;
      for (const char character : value) {
        if (character < '0' || character > '9') {
          valid = false;
          break;
        }
        parsed = parsed * 10 +
                 static_cast<std::uint64_t>(character - '0');
      }
      if (valid) {
        last_event_id_ = parsed;
        has_event_id_ = true;
      }
    } else if (field == "data") {
      if (has_data_) {
        data_.push_back('\n');
      }
      data_ += value;
      has_data_ = true;
    }
    return true;
  }

  bool dispatchEvent() {
    if (!has_data_) {
      return true;
    }
    std::string payload;
    payload.swap(data_);
    has_data_ = false;
    return parseAndDispatch(payload);
  }

  bool parseAndDispatch(const std::string& payload) {
    auto parsed = parseStrict(payload);
    if (!parsed.hasValue()) {
      error_ = parsed.error();
      return false;
    }
    dispatch_(std::move(parsed.value()), last_event_id_);
    dispatched_ = true;
    return true;
  }

  EventCallback dispatch_;
  std::uint64_t last_event_id_ = 0;
  bool has_event_id_ = false;
  bool decided_ = false;
  bool json_mode_ = false;
  bool dispatched_ = false;
  std::string pending_;
  std::string json_;
  std::string line_buffer_;
  std::string data_;
  bool has_data_ = false;
  std::optional<McpError> error_;
};

} // namespace

class HttpClientTransport final : public ITransport {
public:
  explicit HttpClientTransport(StreamableHttpClientOptions options)
      : options_(std::move(options)) {}

  ~HttpClientTransport() override { close(); }

  Status start() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (closed_) {
      return Status::failure(
          reasonError("transport is closed", "network_error"));
    }
    if (started_ && client_) {
      return Status::success();
    }

    const std::string base = normalizeBaseUrl(options_.baseUrl);
    if (base.empty()) {
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams, "baseUrl must not be empty",
                         Json{{"reason", "unsupported_scheme"}}));
    }

    const std::string scheme = schemeOf(base);
    if (!scheme.empty() && scheme != "http" && scheme != "https") {
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams,
                         "unsupported_scheme",
                         Json{{"reason", "unsupported_scheme"}}));
    }

    // These TLS knobs are not wired into cpp-httplib yet; accepting them would
    // silently change security expectations, so fail loudly instead.
    if (options_.tls.enabled &&
        (!options_.tls.serverName.empty() ||
         options_.tls.minVersion != "TLS1.2" ||
         !options_.tls.privateKeyPassword.empty())) {
      return Status::failure(McpError::make(
          ErrorCode::InvalidParams,
          "tls.serverName/minVersion/privateKeyPassword are not supported yet",
          Json{{"reason", "option_invalid"}}));
    }
#if !TUGMCP_TLS_ENABLED
    if (scheme == "https") {
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams,
                         "unsupported_scheme",
                         Json{{"reason", "unsupported_scheme"}}));
    }
#endif

    try {
#if TUGMCP_TLS_ENABLED
      if (scheme == "https") {
        // cpp-httplib derives SNI and hostname verification from the URL host;
        // this version has no separate set_server_name() API.
        // Pass client cert/key only for mutual TLS: the 3-arg constructor loads
        // them unconditionally, so empty paths would leave the SSL context
        // invalid.
        if (!options_.tls.certChainPath.empty() &&
            !options_.tls.privateKeyPath.empty()) {
          client_ = std::make_shared<httplib::Client>(
              base, options_.tls.certChainPath, options_.tls.privateKeyPath);
        } else {
          client_ = std::make_shared<httplib::Client>(base);
        }
        if (!options_.tls.caBundlePath.empty()) {
          client_->set_ca_cert_path(options_.tls.caBundlePath);
        }
        client_->enable_server_certificate_verification(
            options_.tls.verifyPeer);
        client_->enable_server_hostname_verification(options_.tls.verifyPeer);
      } else {
        client_ = std::make_shared<httplib::Client>(base);
      }
#else
      client_ = std::make_shared<httplib::Client>(base);
#endif
    } catch (const std::exception& error) {
      client_.reset();
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams,
                         std::string("invalid baseUrl: ") + error.what(),
                         Json{{"reason", "unsupported_scheme"}}));
    } catch (...) {
      client_.reset();
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams, "invalid baseUrl",
                         Json{{"reason", "unsupported_scheme"}}));
    }

    if (!client_ || !client_->is_valid()) {
      client_.reset();
      return Status::failure(
          McpError::make(ErrorCode::InvalidParams,
                         "SSL client is not valid (TLS unavailable)",
                         Json{{"reason", "tls_not_built"}}));
    }

    client_->set_connection_timeout(options_.requestTimeout);
    client_->set_read_timeout(options_.requestTimeout);
    client_->set_write_timeout(options_.requestTimeout);
    started_ = true;
    return Status::success();
  }

  std::shared_ptr<httplib::Client> createRequestClient() const {
    const std::string base = normalizeBaseUrl(options_.baseUrl);
    try {
      std::shared_ptr<httplib::Client> client;
#if TUGMCP_TLS_ENABLED
      const std::string scheme = schemeOf(base);
      if (scheme == "https") {
        if (!options_.tls.certChainPath.empty() &&
            !options_.tls.privateKeyPath.empty()) {
          client = std::make_shared<httplib::Client>(
              base, options_.tls.certChainPath, options_.tls.privateKeyPath);
        } else {
          client = std::make_shared<httplib::Client>(base);
        }
        if (!options_.tls.caBundlePath.empty()) {
          client->set_ca_cert_path(options_.tls.caBundlePath);
        }
        client->enable_server_certificate_verification(options_.tls.verifyPeer);
        client->enable_server_hostname_verification(options_.tls.verifyPeer);
      } else {
        client = std::make_shared<httplib::Client>(base);
      }
#else
      client = std::make_shared<httplib::Client>(base);
#endif
      if (!client || !client->is_valid()) {
        return nullptr;
      }
      client->set_connection_timeout(options_.requestTimeout);
      client->set_read_timeout(options_.requestTimeout);
      client->set_write_timeout(options_.requestTimeout);
      return client;
    } catch (...) {
      return nullptr;
    }
  }

  void unregisterClient(const std::shared_ptr<httplib::Client>& client,
                        bool has_request_id, const RequestId& request_id) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    for (auto it = active_clients_.begin(); it != active_clients_.end();) {
      auto locked = it->lock();
      if (!locked || locked == client) {
        it = active_clients_.erase(it);
      } else {
        ++it;
      }
    }
    if (has_request_id) {
      const auto it = active_by_id_.find(request_id);
      if (it != active_by_id_.end()) {
        const auto locked = it->second.lock();
        if (!locked || locked == client) {
          active_by_id_.erase(it);
        }
      }
    }
  }

  Status send(const Json& message) override {
    std::shared_ptr<httplib::Client> client = createRequestClient();
    if (!client) {
      return Status::failure(
          reasonError("failed to create http client", "network_error"));
    }

    bool has_request_id = false;
    RequestId request_id;
    if (message.contains("id")) {
      auto parsed_id = RequestId::fromJson(message.at("id"));
      if (parsed_id.hasValue()) {
        request_id = parsed_id.value();
        has_request_id = true;
      }
    }

    std::string protocol_version;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      protocol_version = options_.protocolVersion;
      if (closed_) {
        return Status::failure(
            reasonError("transport is closed", "network_error"));
      }
      if (!started_) {
        return Status::failure(
            reasonError("transport is not started", "network_error"));
      }
      if (has_request_id) {
        const auto it = cancelled_before_send_.find(request_id);
        if (it != cancelled_before_send_.end()) {
          cancelled_before_send_.erase(it);
          return Status::failure(
              reasonError("request cancelled", "cancelled"));
        }
      }
      active_clients_.push_back(client);
      if (has_request_id) {
        active_by_id_[request_id] = client;
      }
    }
    struct ActiveGuard {
      HttpClientTransport* owner;
      std::shared_ptr<httplib::Client> client;
      bool has_request_id;
      RequestId request_id;
      ~ActiveGuard() {
        owner->unregisterClient(client, has_request_id, request_id);
      }
    } active_guard{this, client, has_request_id, request_id};

    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/event-stream");
    headers.emplace("MCP-Protocol-Version", protocol_version);
    if (protocol_version == "2025-11-25") {
      std::string session;
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        session = session_id_;
      }
      if (!session.empty()) {
        headers.emplace("MCP-Session-Id", session);
      }
    }

    if (protocol_version == "2026-07-28" &&
        message.contains("method") && message.at("method").is_string()) {
      const std::string method = message.at("method").get<std::string>();
      headers.emplace("Mcp-Method", method);
      if (method == "tools/call") {
        std::string name;
        if (message.contains("params") && message.at("params").is_object() &&
            message.at("params").contains("name") &&
            message.at("params").at("name").is_string()) {
          name = message.at("params").at("name").get<std::string>();
        }
        if (!name.empty()) {
          headers.emplace("Mcp-Name", name);
        }
      }
    }

    const Json& document = message;

    std::string body;
    try {
      body = document.dump();
    } catch (...) {
      return Status::failure(
          reasonError("failed to encode request", "network_error"));
    }

    ResponseStreamParser parser(
        [this](Json parsed, std::uint64_t) {
          invokeMessage(std::move(parsed));
        });

    httplib::Result result;
    try {
      result = client->Post(
          "/mcp", headers, body, "application/json",
          [&parser](const char* data, std::size_t length) {
            return parser.feed(data, length);
          });
    } catch (...) {
      reportError(reasonError("http client threw", "network_error"));
      return Status::failure(
          reasonError("http client threw", "network_error"));
    }

    if (parser.error().has_value()) {
      reportError(*parser.error());
      return Status::failure(*parser.error());
    }

    if (!result) {
      const bool tls_failure = isTlsError(result.error());
      const char* reason = tls_failure ? "tls_handshake_failed" : "network_error";
      const char* error_message =
          tls_failure ? "TLS handshake failed" : "http request failed";
      reportError(reasonError(error_message, reason));
      return Status::failure(reasonError(error_message, reason));
    }

    const int status = result->status;
    if (status >= 200 && status < 300) {
      const std::string session =
          result->get_header_value("MCP-Session-Id");
      if (!session.empty()) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!closed_) {
          session_id_ = session;
        }
      }
    }

    if (status < 200 || status >= 300) {
      // D18 maps JSON-RPC errors to 4xx statuses. If the body carries a
      // JSON-RPC message, deliver it so callers see the real error code.
      if (parser.finish() && parser.dispatched()) {
        return Status::success();
      }
      reportError(McpError::make(
          ErrorCode::InternalError, "unexpected HTTP status",
          Json{{"reason", "http_status"}, {"status", status}}));
      return Status::failure(McpError::make(
          ErrorCode::InternalError, "unexpected HTTP status",
          Json{{"reason", "http_status"}, {"status", status}}));
    }

    if (status == 202) {
      return Status::success();
    }

    if (!parser.finish()) {
      if (parser.error().has_value()) {
        reportError(*parser.error());
        return Status::failure(*parser.error());
      }
      return Status::failure(
          reasonError("incomplete response stream", "network_error"));
    }

    if (!parser.dispatched()) {
      const McpError error =
          reasonError("incomplete response stream", "network_error");
      reportError(error);
      return Status::failure(error);
    }

    return Status::success();
  }

  void cancelPending(const RequestId& id) override {
    std::shared_ptr<httplib::Client> client;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      const auto it = active_by_id_.find(id);
      if (it != active_by_id_.end()) {
        client = it->second.lock();
      }
      if (!client) {
        if (cancelled_before_send_.size() > 1024) {
          cancelled_before_send_.clear();
        }
        cancelled_before_send_.insert(id);
      }
    }
    if (client) {
      try {
        client->stop();
      } catch (...) {
      }
    }
  }

  void setOnMessage(std::function<void(Json)> handler) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(McpError)> handler) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_close_ = std::move(handler);
  }

  bool supportsServerStream() const override { return true; }

  Status setProtocolVersion(std::string_view version) override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    options_.protocolVersion = std::string(version);
    return Status::success();
  }

  std::string sessionId() const override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return session_id_;
  }

  Status openStream() override {
    std::string session;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (closed_) {
        return Status::failure(
            reasonError("transport is closed", "network_error"));
      }
      session = session_id_;
    }
    if (session.empty()) {
      return Status::failure(McpError::make(
          ErrorCode::InternalError, "missing session id",
          Json{{"reason", "missing_session"}}));
    }
    std::thread finished;
    {
      std::lock_guard<std::mutex> lock(stream_control_mutex_);
      if (stream_thread_.joinable()) {
        if (stream_running_.load(std::memory_order_acquire)) {
          return Status::success();
        }
        finished = std::move(stream_thread_);
      }
      stream_stop_.store(false, std::memory_order_release);
      stream_running_.store(false, std::memory_order_release);
      try {
        stream_thread_ =
            std::thread([this, session] { streamLoop(session); });
      } catch (...) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "failed to start stream thread",
            Json{{"reason", "network_error"}}));
      }
      stream_running_.store(true, std::memory_order_release);
    }
    if (finished.joinable()) {
      finished.join();
    }
    return Status::success();
  }

  void closeStream() override {
    stream_stop_.store(true, std::memory_order_release);
    std::shared_ptr<httplib::Client> client;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      client = stream_client_;
    }
    if (client) {
      try {
        client->stop();
      } catch (...) {
      }
    }
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(stream_control_mutex_);
      if (stream_thread_.joinable() &&
          stream_thread_.get_id() != std::this_thread::get_id()) {
        thread = std::move(stream_thread_);
      }
    }
    if (thread.joinable()) {
      thread.join();
    }
    stream_running_.store(false, std::memory_order_release);
  }

  void close() noexcept {
    closeStream();
    std::vector<std::shared_ptr<httplib::Client>> clients;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      closed_ = true;
      started_ = false;
      cancelled_before_send_.clear();
      session_id_.clear();
      last_event_id_.store(0);
      if (client_) {
        clients.push_back(std::move(client_));
        client_.reset();
      }
      for (const auto& weak : active_clients_) {
        if (auto client = weak.lock()) {
          clients.push_back(std::move(client));
        }
      }
      active_clients_.clear();
    }
    for (const auto& client : clients) {
      try {
        client->stop();
      } catch (...) {
      }
    }

    notifyClose();
  }

private:
  void streamLoop(std::string session) {
    std::uint64_t last_event = last_event_id_.load();
    std::chrono::milliseconds backoff{100};
    while (!stream_stop_.load(std::memory_order_acquire)) {
      auto client = createRequestClient();
      if (!client) {
        waitForStop(backoff);
        backoff = nextBackoff(backoff);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (closed_ || stream_stop_.load(std::memory_order_acquire)) {
          break;
        }
        stream_client_ = client;
      }

      httplib::Headers headers;
      headers.emplace("Accept", "text/event-stream");
      headers.emplace("MCP-Protocol-Version", "2025-11-25");
      headers.emplace("MCP-Session-Id", session);
      if (last_event > 0) {
        headers.emplace("Last-Event-ID", std::to_string(last_event));
      }

      ResponseStreamParser parser(
          [this, &last_event](Json message, std::uint64_t event_id) {
            if (event_id > 0) {
              last_event = event_id;
              last_event_id_.store(event_id);
            }
            invokeMessage(std::move(message));
          });

      httplib::Result result;
      try {
        result = client->Get(
            "/mcp", headers,
            [&parser](const char* data, std::size_t length) {
              return parser.feed(data, length);
            });
      } catch (...) {
        result = httplib::Result(nullptr, httplib::Error::Connection);
      }

      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (stream_client_ == client) {
          stream_client_.reset();
        }
      }
      if (stream_stop_.load(std::memory_order_acquire)) {
        break;
      }
      if (result && (result->status == 404 || result->status == 405)) {
        reportError(McpError::make(
            ErrorCode::InternalError, "stream request rejected",
            Json{{"reason", "http_status"}, {"status", result->status}}));
        break;
      }
      if (result && result->status == 200) {
        backoff = std::chrono::milliseconds(100);
      }
      waitForStop(backoff);
      backoff = nextBackoff(backoff);
    }
    stream_running_.store(false, std::memory_order_release);
  }

  static std::chrono::milliseconds nextBackoff(
      std::chrono::milliseconds current) {
    if (current < std::chrono::milliseconds(250)) {
      return std::chrono::milliseconds(250);
    }
    if (current < std::chrono::milliseconds(1000)) {
      return std::chrono::milliseconds(1000);
    }
    return std::chrono::milliseconds(1000);
  }

  void waitForStop(std::chrono::milliseconds duration) const {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!stream_stop_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

private:
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

  void invokeMessage(Json message) noexcept {
    std::function<void(Json)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_message_;
    }
    if (!handler) {
      return;
    }
    try {
      handler(std::move(message));
    } catch (...) {
    }
  }

  void reportError(McpError error) noexcept {
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

  StreamableHttpClientOptions options_;
  std::shared_ptr<httplib::Client> client_;

  mutable std::mutex lifecycle_mutex_;
  bool started_ = false;
  bool closed_ = false;
  std::string session_id_;
  std::shared_ptr<httplib::Client> stream_client_;
  std::atomic<bool> close_notified_{false};
  std::atomic<std::uint64_t> last_event_id_{0};

  std::mutex stream_control_mutex_;
  std::thread stream_thread_;
  std::atomic<bool> stream_stop_{false};
  std::atomic<bool> stream_running_{false};

  std::vector<std::weak_ptr<httplib::Client>> active_clients_;
  std::unordered_map<RequestId, std::weak_ptr<httplib::Client>,
                     std::hash<RequestId>>
      active_by_id_;
  std::unordered_set<RequestId, std::hash<RequestId>> cancelled_before_send_;

  std::mutex callback_mutex_;
  std::function<void(Json)> on_message_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
};

Result<std::shared_ptr<ITransport>> makeStreamableHttpClientTransport(
    const StreamableHttpClientOptions& options) {
  if (options.baseUrl.empty()) {
    return Result<std::shared_ptr<ITransport>>::failure(
        McpError::make(ErrorCode::InvalidParams, "baseUrl must not be empty"));
  }

  const std::string scheme = schemeOf(options.baseUrl);
  const bool https = scheme == "https";
#if !TUGMCP_TLS_ENABLED
  if (https) {
    return optionFailure("unsupported_scheme",
                         "https is not supported in this build");
  }
#endif
  if (https != options.tls.enabled) {
    return optionFailure(
        "option_invalid",
        "tls.enabled must match the baseUrl scheme (https or http)");
  }

  std::shared_ptr<ITransport> transport(new HttpClientTransport(options));
  return transport;
}

} // namespace v1
} // namespace mcp
