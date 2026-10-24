#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <tugmcp/executor.hpp>
#include <tugmcp/schema.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport.hpp>

namespace mcp {
namespace v1 {

struct ServerOptions {
  std::string name = "tugmcp";
  std::string version = "0.1.0";
  std::shared_ptr<ITimeSource> timeSource;

  // Schema validation policy. With no validator and a tool that declares an
  // inputSchema, registration fails unless allowUnvalidatedTools is true.
  SchemaValidatorPtr schemaValidator;
  bool allowUnvalidatedTools = false;

  // Production hosts should inject an executor. A null executor keeps the
  // legacy synchronous dispatch path for compatibility (handlers then run on
  // the transport thread).
  std::shared_ptr<IExecutor> executor;
};

class McpServer {
public:
  McpServer(ServerOptions options, std::shared_ptr<ITransport> transport);
  ~McpServer();

  ToolRegistry& tools() noexcept;

  // Starts the transport and blocks until onClose()/stop(), then closes it.
  Status run();

  // Thread-safe and idempotent.
  void stop() noexcept;

  // Last transport/protocol error observed while running (test aid).
  std::optional<McpError> lastError() const;

private:
  // transport_ is declared first so it is destroyed last.
  std::shared_ptr<ITransport> transport_;
  ServerOptions options_;
  ToolRegistry tools_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::optional<McpError> last_error_;
};

} // namespace v1
} // namespace mcp
