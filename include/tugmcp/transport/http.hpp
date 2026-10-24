#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <tugmcp/request_context.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

struct TlsOptions {
  bool enabled = false;
  std::string certChainPath;
  std::string privateKeyPath;
  std::string privateKeyPassword;
  std::string clientCaFile;
  bool verifyPeer = true;
  std::string caBundlePath;
  std::string serverName;
  std::string minVersion = "TLS1.2";
};

enum class AdmissionDecision { Accept, Reject, Emergency };

class IAdmissionHook {
public:
  virtual ~IAdmissionHook() = default;
  virtual AdmissionDecision admit(const RequestContext& context,
                                  const Json& request) = 0;
};

// Stateless M2 HTTP server. In-flight requests are associated by RequestId only;
// concurrent clients must therefore keep ids globally unique. This is an M2
// limitation; P3 will scope pending requests per executor/connection.
struct StreamableHttpServerOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  std::size_t maxBodyBytes = 1u << 20;
  std::size_t maxConcurrentStreams = 8; // not wired yet (M2 remaining)
  std::size_t perStreamQueue = 256;     // not wired yet (M2 remaining)
  bool enable2025Session = true;        // not wired yet (M2 remaining)
  std::shared_ptr<IAdmissionHook> admission; // not wired yet (M2 remaining)
  std::string emergencyPath = "/emergency";  // not wired yet (M2 remaining)
  bool dedicatedEmergencyListener = false;   // not wired yet (M2 remaining)
  TlsOptions tls;
  std::shared_ptr<ITimeSource> timeSource;
  // Before P3/IExecutor, tool execution runs synchronously on the HTTP thread,
  // so this bounds only the wait for send(response); there is no execution
  // timeout yet.
  std::chrono::milliseconds requestTimeout{30000};
};

class HttpServerTransport final : public ITransport {
public:
  ~HttpServerTransport() override;

  Status start() override;
  Status send(const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
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
