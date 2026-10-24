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
#include <utility>

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
  explicit ResponseStreamParser(std::function<void(Json)> dispatch)
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

    if (field == "data") {
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
    dispatch_(std::move(parsed.value()));
    dispatched_ = true;
    return true;
  }

  std::function<void(Json)> dispatch_;
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
          client_ = std::make_unique<httplib::Client>(
              base, options_.tls.certChainPath, options_.tls.privateKeyPath);
        } else {
          client_ = std::make_unique<httplib::Client>(base);
        }
        if (!options_.tls.caBundlePath.empty()) {
          client_->set_ca_cert_path(options_.tls.caBundlePath);
        }
        client_->enable_server_certificate_verification(
            options_.tls.verifyPeer);
        client_->enable_server_hostname_verification(options_.tls.verifyPeer);
      } else {
        client_ = std::make_unique<httplib::Client>(base);
      }
#else
      client_ = std::make_unique<httplib::Client>(base);
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

  Status send(const Json& message) override {
    std::unique_lock<std::mutex> send_lock(send_mutex_);

    httplib::Client* client = nullptr;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (closed_) {
        return Status::failure(
            reasonError("transport is closed", "network_error"));
      }
      if (!started_ || !client_) {
        return Status::failure(
            reasonError("transport is not started", "network_error"));
      }
      client = client_.get();
    }

    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/event-stream");
    headers.emplace("MCP-Protocol-Version", options_.protocolVersion);

    if (options_.protocolVersion == "2026-07-28" &&
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

    Json document = message;
    if (options_.protocolVersion == "2026-07-28" &&
        document.contains("method") && document.at("method").is_string()) {
      if (!document.contains("params") ||
          !document.at("params").is_object()) {
        document["params"] = Json::object();
      }
      Json& params = document["params"];
      if (!params.contains("_meta") || !params.at("_meta").is_object()) {
        params["_meta"] = Json::object();
      }
      Json& meta = params["_meta"];
      if (!meta.contains("protocolVersion")) {
        meta["protocolVersion"] = options_.protocolVersion;
      }
    }

    std::string body;
    try {
      body = document.dump();
    } catch (...) {
      return Status::failure(
          reasonError("failed to encode request", "network_error"));
    }

    ResponseStreamParser parser(
        [this](Json parsed) { invokeMessage(std::move(parsed)); });

    httplib::Result result;
    try {
      result = client->Post(
          "/mcp", headers, body, "application/json",
          [&parser](const char* data, std::size_t length) {
            return parser.feed(data, length);
          });
    } catch (...) {
      send_lock.unlock();
      reportError(reasonError("http client threw", "network_error"));
      return Status::failure(
          reasonError("http client threw", "network_error"));
    }

    if (parser.error().has_value()) {
      send_lock.unlock();
      reportError(*parser.error());
      return Status::failure(*parser.error());
    }

    if (!result) {
      send_lock.unlock();
      const bool tls_failure = isTlsError(result.error());
      const char* reason = tls_failure ? "tls_handshake_failed" : "network_error";
      const char* error_message =
          tls_failure ? "TLS handshake failed" : "http request failed";
      reportError(reasonError(error_message, reason));
      return Status::failure(reasonError(error_message, reason));
    }

    const int status = result->status;
    send_lock.unlock();

    if (status < 200 || status >= 300) {
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

  void cancelPending(const RequestId&) override {
    // Do not take send_mutex_: the blocked sender owns it. cpp-httplib's
    // stop() is thread-safe and aborts the in-flight socket operation.
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (client_) {
      try {
        client_->stop();
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

  void close() noexcept {
    {
      std::lock_guard<std::mutex> send_lock(send_mutex_);
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      closed_ = true;
      started_ = false;
      if (client_) {
        try {
          client_->stop();
        } catch (...) {
        }
        client_.reset();
      }
    }

    notifyClose();
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
  std::unique_ptr<httplib::Client> client_;

  std::mutex lifecycle_mutex_;
  bool started_ = false;
  bool closed_ = false;
  std::atomic<bool> close_notified_{false};

  std::mutex send_mutex_;

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
