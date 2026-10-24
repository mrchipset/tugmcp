#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <tugmcp/security.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/transport.hpp>
#include <tugmcp/transport/http.hpp>

namespace mcp {
namespace v1 {

struct LegacySseServerOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  std::size_t maxBodyBytes = 1024 * 1024;
  std::size_t maxSessions = 128;
  std::size_t perStreamQueue = 256;
  std::vector<std::string> allowedHosts;
  std::vector<std::string> allowedOrigins;
  TlsOptions tls;
  std::shared_ptr<IAuthenticator> auth;
  std::shared_ptr<IAuditHook> audit;
  std::shared_ptr<ITimeSource> timeSource;
};

struct LegacySseClientOptions {
  std::string baseUrl;
  int connectTimeoutMs = 10000;
  int readTimeoutMs = 30000;
  TlsOptions tls;
};

class LegacySseServerTransport final : public ITransport {
public:
  explicit LegacySseServerTransport(LegacySseServerOptions options);
  ~LegacySseServerTransport() override;
  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void setAuthenticator(std::shared_ptr<IAuthenticator> auth) override;
  void setAuditHook(std::shared_ptr<IAuditHook> audit) override;
  void setSupportedVersions(std::vector<std::string> versions) override;
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override;
  Status sendToSession(std::string_view sessionId,
                          const Json& message) override;
  void close() override;
  int boundPort() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class LegacySseClientTransport final : public ITransport {
public:
  explicit LegacySseClientTransport(LegacySseClientOptions options);
  ~LegacySseClientTransport() override;
  Status start() override;
  Status send(const Json& message) override;
  Status sendTo(std::string_view route, const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnMessageEx(MessageHandlerEx handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void close() override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

Result<std::shared_ptr<ITransport>> makeLegacySseServerTransport(
    const LegacySseServerOptions& options);
Result<std::shared_ptr<ITransport>> makeLegacySseClientTransport(
    const LegacySseClientOptions& options);

} // namespace v1
} // namespace mcp
