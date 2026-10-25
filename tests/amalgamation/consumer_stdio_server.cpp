// Standalone stdio echo server built from the generated core single header.
//
// No split-library headers, no static library. The implementation lives in this
// single TU via TUGMCP_IMPLEMENTATION.

#define TUGMCP_IMPLEMENTATION
#include "tugmcp.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace mcp::v1;

class EchoHandler final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&,
                             IProgressSink&) override {
    ToolResult result;
    ContentBlock block;
    block.type = ContentType::Text;
    block.text = arguments.dump();
    result.content.push_back(std::move(block));
    return ToolOutcome(std::move(result));
  }
};

} // namespace

int main() {
  auto transport = makeStdioTransport();

  ServerOptions options;
  options.name = "amalgamation-echo";
  options.version = "0.1.0";
  options.allowUnvalidatedTools = true;
  options.supportedVersions = {"2026-07-28", "2025-11-25"};

  ToolDescriptor echo;
  echo.name = "echo";
  echo.description = "Echoes its arguments as JSON text";
  echo.inputSchema = Json{{"type", "object"},
                          {"properties", Json{{"text", Json{{"type", "string"}}}}}};

  McpServer server(options, transport);
  const auto added = server.tools().add(echo, std::make_shared<EchoHandler>());
  if (!added.ok) {
    std::cerr << "amalgamation-echo: failed to register echo tool\n";
    return 1;
  }

  std::cerr << "amalgamation-echo stdio ready" << std::endl;
  const Status status = server.run();
  return status ? 0 : 1;
}
