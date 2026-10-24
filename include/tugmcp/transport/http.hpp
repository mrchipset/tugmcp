#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <tugmcp/request_context.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

struct TlsOptions {
  bool enabled = false;
  std::string certChainPath;
  std::string privateKeyPath;
  std::string privateKeyPassword;  // not wired yet (M2 remaining)
  std::string clientCaFile;
  bool verifyPeer = true;
  std::string caBundlePath;
  std::string serverName;          // client-only; not wired (SNI uses the URL host)
  std::string minVersion = "TLS1.2";  // client-only; not wired
};

enum class AdmissionDecision { Accept, Reject, Emergency };

class IAdmissionHook {
public:
  virtual ~IAdmissionHook() = default;
  virtual AdmissionDecision admit(const RequestContext& context,
                                  const Json& request) = 0;
};

// Streamable HTTP server. Each POST is routed independently; concurrent
// clients may reuse JSON-RPC ids safely. Each open SSE stream parks one
// cpp-httplib worker thread; the pool is sized above maxConcurrentStreams
// (max(8, maxConcurrentStreams + 4), or 16 workers when
// maxConcurrentStreams == 0) to keep JSON traffic responsive.
struct StreamableHttpServerOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  std::size_t maxBodyBytes = 1u << 20;
  std::size_t maxConcurrentStreams = 8; // 0 = unbounded
  std::size_t perStreamQueue = 256;     // 0 = unbounded
  bool enable2025Session = true;        // not wired yet (M3)
  std::shared_ptr<IAdmissionHook> admission;
  std::string emergencyPath = "/emergency";  // not wired yet (P3)
  bool dedicatedEmergencyListener = false;   // not wired yet (P3)
  TlsOptions tls;
  std::shared_ptr<ITimeSource> timeSource;
  // Before P3/IExecutor, tool execution runs synchronously on the HTTP thread,
  // so this bounds only the wait for send(response); there is no execution
  // timeout yet.
  std::chrono::milliseconds requestTimeout{30000};
  // 2025 session limits. A new session over maxSessions gets HTTP 429; expiry
  // uses timeSource (steady clock by default).
  std::size_t maxSessions = 128;
  std::chrono::milliseconds sessionTimeout{std::chrono::minutes(30)};
  std::vector<std::string> supportedVersions{"2026-07-28",
                                             "2025-11-25"};
  // Host/Origin allowlists. Empty means loopback-only. Use "*" to allow any.
  std::vector<std::string> allowedHosts;
  std::vector<std::string> allowedOrigins;
};

class HttpServerTransport final : public ITransport {
public:
  ~HttpServerTransport() override;

  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  Status sendToSession(std::string_view sessionId, const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void setOnRouteClosed(
      std::function<void(const std::string&)> handler) override;
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override;
  bool supportsStreaming() const override { return true; }
  void setSupportedVersions(std::vector<std::string> versions) override;
  void setAuthenticator(std::shared_ptr<IAuthenticator> authenticator) override;
  void setAuditHook(std::shared_ptr<IAuditHook> audit) override;
  void close() override;

  int boundPort() const;

private:
  friend Result<std::shared_ptr<ITransport>>
  makeStreamableHttpServerTransport(const StreamableHttpServerOptions&);

  explicit HttpServerTransport(StreamableHttpServerOptions options);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

Result<std::shared_ptr<ITransport>> makeStreamableHttpServerTransport(
    const StreamableHttpServerOptions& options);

struct StreamableHttpClientOptions {
  std::string baseUrl;
  std::chrono::milliseconds requestTimeout{30000};
  std::size_t maxSseQueue = 256;
  TlsOptions tls;
  std::shared_ptr<ITimeSource> timeSource;
  // Sent as MCP-Protocol-Version. Mcp-Method/Mcp-Name are added only for
  // "2026-07-28".
  std::string protocolVersion = "2025-11-25";
};

Result<std::shared_ptr<ITransport>> makeStreamableHttpClientTransport(
    const StreamableHttpClientOptions& options);

} // namespace v1
} // namespace mcp
