// End-to-end test: a real McpServer over StdioTransport connected to the test
// through anonymous pipes. No sleeps are used; the server exits on pipe EOF.

#include <doctest/doctest.h>

#include <tugmcp/message.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/stdio.hpp>

#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using mcp::v1::ContentBlock;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::McpServer;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::StdioOptions;
using mcp::v1::ToolDescriptor;

namespace {
ServerOptions permissiveServerOptions() {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  return options;
}

} // namespace

using mcp::v1::ToolHandlerPtr;
using mcp::v1::ToolResult;
using mcp::v1::decode;
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

std::string writeLine(std::FILE* stream, const std::string& line) {
  const std::string payload = line + "\n";
  std::fwrite(payload.data(), 1, payload.size(), stream);
  std::fflush(stream);
  return payload;
}

std::string readLine(std::FILE* stream) {
  char buffer[8192];
  if (std::fgets(buffer, sizeof(buffer), stream) == nullptr) {
    return {};
  }
  std::string line(buffer);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  return line;
}

} // namespace

TEST_CASE("stdio server answers tools/call end to end") {
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

  StdioOptions options;
  options.input = server_in;
  options.output = server_out;
  options.setBinaryMode = false;

  McpServer server(permissiveServerOptions(), makeStdioTransport(options));
  REQUIRE(server.tools().add(echoDescriptor(), std::make_shared<EchoHandler>()));
  std::thread server_thread([&server] { server.run(); });

  writeLine(client_out,
            R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})");

  const std::string response = readLine(client_in);
  const auto decoded = decode(response);
  REQUIRE(decoded.hasValue());
  const auto& message = decoded.value();
  REQUIRE(message.id == mcp::v1::RequestId{std::int64_t{7}});
  REQUIRE(!message.error.has_value());
  CHECK(message.result.at("structuredContent").at("text") == "hello");
  CHECK(message.result.at("content").at(0).at("text") == "hello");

  // Closing the client's write end makes the server reader observe EOF.
  std::fclose(client_out);
  server_thread.join();
  std::fclose(client_in);
  std::fclose(server_in);
  std::fclose(server_out);
}

TEST_CASE("stdio server survives a malformed line and keeps serving") {
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

  StdioOptions options;
  options.input = server_in;
  options.output = server_out;
  options.setBinaryMode = false;

  McpServer server(permissiveServerOptions(), makeStdioTransport(options));
  std::thread server_thread([&server] { server.run(); });

  writeLine(client_out, R"({"jsonrpc":"2.0","id":)");
  writeLine(client_out, R"({"jsonrpc":"2.0","id":8,"method":"ping"})");

  const std::string response = readLine(client_in);
  const auto decoded = decode(response);
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().id == mcp::v1::RequestId{std::int64_t{8}});
  CHECK(decoded.value().result.is_object());

  std::fclose(client_out);
  server_thread.join();
  std::fclose(client_in);
  std::fclose(server_in);
  std::fclose(server_out);
}
