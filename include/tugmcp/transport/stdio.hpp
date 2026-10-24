#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>

#include "tugmcp/transport.hpp"

namespace mcp {
namespace v1 {

// Newline-delimited JSON over a pair of C streams. The streams are not owned:
// StdioTransport never fclose()s input or output (in particular stdin/stdout).
struct StdioOptions {
  std::FILE* input = stdin;
  std::FILE* output = stdout;
  std::size_t maxMessageBytes = 4u * 1024u * 1024u;
  bool setBinaryMode = true;
};

class StdioTransport final : public ITransport {
public:
  explicit StdioTransport(StdioOptions options = {});
  ~StdioTransport() override;

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  Status start() override;
  Status send(const Json& message) override;
  void setOnMessage(std::function<void(Json)> handler) override;
  void setOnError(std::function<void(McpError)> handler) override;
  void setOnClose(std::function<void()> handler) override;
  void close() override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

std::shared_ptr<ITransport> makeStdioTransport(StdioOptions options = {});

} // namespace v1
} // namespace mcp
