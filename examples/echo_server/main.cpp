#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/stdio.hpp>

#if TUGMCP_ECHO_HTTP
#include <tugmcp/transport/http.hpp>
#endif

namespace {

using namespace mcp::v1;

class EchoHandler final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&,
                             IProgressSink&) override {
    std::string text;
    if (arguments.contains("text") && arguments.at("text").is_string()) {
      text = arguments.at("text").get<std::string>();
    }

    ToolResult result;
    result.content.push_back(ContentBlock{ContentType::Text, text});
    result.structuredContent = Json{{"text", text}};
    return ToolOutcome(std::move(result));
  }
};

bool configureServer(McpServer& server) {
  ToolDescriptor descriptor;
  descriptor.name = "echo";
  descriptor.title = "Echo";
  descriptor.description = "Return the provided text unchanged";
  descriptor.inputSchema = Json::parse(
      R"({"type":"object","properties":{"text":{"type":"string"}}})");
  return server.tools().add(descriptor, std::make_shared<EchoHandler>()).ok;
}

void printUsage() {
  std::cerr
      << "usage: tugmcp_echo_server [--http [--port N] [--bind ADDR]]\n"
      << "       tugmcp_echo_server --help\n"
      << "\n"
      << "With no arguments the server uses stdio. --http starts Streamable "
         "HTTP.\n";
}

} // namespace

int main(int argc, char** argv) {
  using namespace mcp::v1;

  bool http_mode = false;
  int port = 0;
  std::string bind = "127.0.0.1";

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      printUsage();
      return 0;
    }
    if (argument == "--http") {
      http_mode = true;
      continue;
    }
    if (argument == "--port") {
      if (index + 1 >= argc || !http_mode) {
        printUsage();
        return 2;
      }
      char* end = nullptr;
      const long parsed = std::strtol(argv[++index], &end, 10);
      if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 65535) {
        printUsage();
        return 2;
      }
      port = static_cast<int>(parsed);
      continue;
    }
    if (argument == "--bind") {
      if (index + 1 >= argc || !http_mode) {
        printUsage();
        return 2;
      }
      bind = argv[++index];
      continue;
    }
    printUsage();
    return 2;
  }

  ServerOptions options;
  options.name = "tugmcp-echo";
  options.version = "0.1.0";
  // This demo deliberately opts out of schema validation so it can run without
  // an extra schema target. Production hosts should inject a validator via
  // options.schemaValidator and leave this false (the fail-closed default).
  options.allowUnvalidatedTools = true;

  if (!http_mode) {
    McpServer server(options, makeStdioTransport());
    if (!configureServer(server)) {
      return 1;
    }
    const Status status = server.run();
    return status ? 0 : 1;
  }

#if TUGMCP_ECHO_HTTP
  StreamableHttpServerOptions http_options;
  http_options.bind = bind;
  http_options.port = port;

  auto created = makeStreamableHttpServerTransport(http_options);
  if (!created.hasValue()) {
    std::cerr << "tugmcp-echo: " << created.error().message;
    if (created.error().data.is_object() &&
        created.error().data.contains("reason")) {
      std::cerr << " (" << created.error().data.at("reason").dump() << ")";
    }
    std::cerr << "\n";
    return 1;
  }

  std::shared_ptr<ITransport> transport = created.value();
  auto concrete = std::dynamic_pointer_cast<HttpServerTransport>(transport);
  if (!concrete) {
    std::cerr << "tugmcp-echo: unexpected HTTP transport type\n";
    return 1;
  }

  McpServer server(options, transport);
  if (!configureServer(server)) {
    return 1;
  }

  const Status started = transport->start();
  if (!started) {
    std::cerr << "tugmcp-echo: failed to start HTTP transport: "
              << started.error.message << "\n";
    return 1;
  }
  std::cerr << "tugmcp-echo listening on http://" << bind << ":"
            << concrete->boundPort() << "/mcp\n";

  const Status status = server.run();
  return status ? 0 : 1;
#else
  std::cerr << "tugmcp-echo: HTTP mode is unavailable in this build "
               "(TUGMCP_WITH_HTTP=OFF)\n";
  return 2;
#endif
}
