#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <tugmcp/message.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

struct ClientOptions {
  std::string name = "tugmcp-client";
  std::string version = "0.1.0";
  std::uint32_t timeoutMs = 30000;
  std::shared_ptr<ITimeSource> timeSource;
};

class McpClient {
public:
  McpClient(ClientOptions options, std::shared_ptr<ITransport> transport);
  ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  Status connect();
  Result<Json> call(std::string_view method, Json params = Json::object());
  Result<Json> listTools();
  Result<Json> callTool(std::string_view name, Json arguments = Json::object());
  void close() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace mcp
