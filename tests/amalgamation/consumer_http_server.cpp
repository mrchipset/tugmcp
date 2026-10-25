// Standalone Streamable HTTP echo server built from the generated HTTP single
// header.
//
// No split-library headers, no static library. The implementation lives in this
// single TU via TUGMCP_IMPLEMENTATION.

#define TUGMCP_IMPLEMENTATION
#include "tugmcp_http.hpp"

#include <cstdlib>
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

int main(int argc, char** argv) {
  bool http = false;
  int port = 0;
  std::string bind = "127.0.0.1";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--http") {
      http = true;
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--bind" && i + 1 < argc) {
      bind = argv[++i];
    }
  }
  if (!http) {
    std::cerr << "amalgamation-echo: --http is required\n";
    return 2;
  }

  StreamableHttpServerOptions http_options;
  http_options.bind = bind;
  http_options.port = port;
  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created.hasValue()) {
    std::cerr << "amalgamation-echo: " << created.error().message << "\n";
    return 1;
  }
  auto concrete = std::dynamic_pointer_cast<HttpServerTransport>(created.value());
  if (!concrete) {
    std::cerr << "amalgamation-echo: unexpected transport\n";
    return 1;
  }

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

  McpServer server(options, created.value());
  const auto added = server.tools().add(echo, std::make_shared<EchoHandler>());
  if (!added.ok) {
    std::cerr << "amalgamation-echo: failed to register echo tool\n";
    return 1;
  }

  const Status started = created.value()->start();
  if (!started) {
    std::cerr << "amalgamation-echo: " << started.error.message << "\n";
    return 1;
  }
  std::cerr << "amalgamation-echo listening on http://" << bind << ":"
            << concrete->boundPort() << "/mcp" << std::endl;

  const Status status = server.run();
  return status ? 0 : 1;
}
