// End-to-end loopback: a real McpServer and McpClient connected through two
// anonymous pipes. No sleeps are used.

#include <doctest/doctest.h>

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/stdio.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using mcp::v1::ClientOptions;
using mcp::v1::ContentBlock;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::McpClient;
using mcp::v1::McpServer;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StdioOptions;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolResult;
using mcp::v1::makeStdioTransport;

namespace {

#ifdef _WIN32
int makePipe(int fds[2]) { return _pipe(fds, 8192, _O_BINARY); }
std::FILE* openRead(int fd) { return _fdopen(fd, "rb"); }
std::FILE* openWrite(int fd) { return _fdopen(fd, "wb"); }
#else
int makePipe(int fds[2]) { return ::pipe(fds); }
std::FILE* openRead(int fd) { return ::fdopen(fd, "rb"); }
std::FILE* openWrite(int fd) { return ::fdopen(fd, "wb"); }
#endif

class EchoHandler final : public IToolHandler {
public:
  Result<ToolResult> call(const Json& arguments, RequestContext&) override {
    ToolResult result;
    ContentBlock block;
    block.type = "text";
    block.text = arguments.value("text", std::string{});
    result.content.push_back(block);
    result.structuredContent = Json{{"text", block.text}};
    return result;
  }
};

ToolDescriptor echoDescriptor() {
  ToolDescriptor descriptor;
  descriptor.name = "echo";
  descriptor.description = "Echo the provided text back";
  descriptor.inputSchema = Json{{"type", "object"}};
  return descriptor;
}

} // namespace

TEST_CASE("client and server communicate over stdio pipes") {
  int client_to_server[2];
  int server_to_client[2];
  REQUIRE(makePipe(client_to_server) == 0);
  REQUIRE(makePipe(server_to_client) == 0);

  std::FILE* server_in = openRead(client_to_server[0]);
  std::FILE* client_out = openWrite(client_to_server[1]);
  std::FILE* server_out = openWrite(server_to_client[1]);
  std::FILE* client_in = openRead(server_to_client[0]);
  REQUIRE(server_in != nullptr);
  REQUIRE(client_out != nullptr);
  REQUIRE(server_out != nullptr);
  REQUIRE(client_in != nullptr);

  StdioOptions server_stdio;
  server_stdio.input = server_in;
  server_stdio.output = server_out;
  server_stdio.setBinaryMode = false;

  McpServer server(ServerOptions{}, makeStdioTransport(server_stdio));
  REQUIRE(server.tools().add(echoDescriptor(), std::make_shared<EchoHandler>()));
  std::thread server_thread([&server] { server.run(); });

  StdioOptions client_stdio;
  client_stdio.input = client_in;
  client_stdio.output = client_out;
  client_stdio.setBinaryMode = false;

  ClientOptions client_options;
  client_options.name = "loopback-client";
  client_options.timeoutMs = 2000;
  McpClient client(client_options, makeStdioTransport(client_stdio));

  const Status connected = client.connect();
  REQUIRE(connected.ok);

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  bool found_echo = false;
  for (const auto& tool : listed.value().at("tools")) {
    if (tool.value("name", std::string{}) == "echo") {
      found_echo = true;
    }
  }
  CHECK(found_echo);

  auto called = client.callTool("echo", Json{{"text", "hi"}});
  REQUIRE(called.hasValue());
  CHECK(called.value().at("structuredContent").at("text") == "hi");
  CHECK(called.value().at("content").at(0).at("text") == "hi");

  // Start close first, then close the client's write end so the server reader
  // observes EOF. On POSIX the client's reader is woken after the server side
  // output stream is closed below; no sleep or polling is involved.
  std::thread close_thread([&client] { client.close(); });
  std::fclose(client_out);
  server_thread.join();
  std::fclose(server_out);
  close_thread.join();

  std::fclose(client_in);
  std::fclose(server_in);
}
