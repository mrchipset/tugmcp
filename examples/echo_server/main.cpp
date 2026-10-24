#include <memory>
#include <string>
#include <utility>

#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/stdio.hpp>

namespace {

using namespace mcp::v1;

class EchoHandler final : public IToolHandler {
public:
  Result<ToolResult> call(const Json& arguments,
                          RequestContext&) override {
    std::string text;
    if (arguments.contains("text") && arguments.at("text").is_string()) {
      text = arguments.at("text").get<std::string>();
    }

    ToolResult result;
    result.content.push_back(ContentBlock{"text", text});
    result.structuredContent = Json{{"text", text}};
    return result;
  }
};

} // namespace

int main() {
  using namespace mcp::v1;

  ServerOptions options;
  options.name = "tugmcp-echo";
  options.version = "0.1.0";

  McpServer server(options, makeStdioTransport());

  ToolDescriptor descriptor;
  descriptor.name = "echo";
  descriptor.title = "Echo";
  descriptor.description = "Return the provided text unchanged";
  descriptor.inputSchema = Json::parse(
      R"({"type":"object","properties":{"text":{"type":"string"}}})");

  if (!server.tools().add(descriptor, std::make_shared<EchoHandler>())) {
    return 1;
  }

  const Status status = server.run();
  return status ? 0 : 1;
}
