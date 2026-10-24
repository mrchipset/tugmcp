#if TUGMCP_TLS_ENABLED

// TLS loopback: real OpenSSL via cpp-httplib, McpClient + McpServer over
// https://localhost. No sleeps are used.

#include <doctest/doctest.h>

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using mcp::v1::ClientOptions;
using mcp::v1::ContentBlock;
using mcp::v1::HttpServerTransport;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpClient;
using mcp::v1::McpError;
using mcp::v1::McpServer;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StreamableHttpClientOptions;
using mcp::v1::StreamableHttpServerOptions;
using mcp::v1::ToolDescriptor;
using mcp::v1::IProgressSink;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolResult;
using mcp::v1::makeStreamableHttpClientTransport;
using mcp::v1::makeStreamableHttpServerTransport;

namespace {

std::string fixtureDir() {
  return std::string(TUGMCP_TEST_DATA_DIR) + "/fixtures/tls/";
}

std::string httpsUrlFor(int port) {
  // The server binds 127.0.0.1 and the test certificate carries an IP SAN for
  // it; using the literal avoids localhost resolving to ::1 first.
  return "https://127.0.0.1:" + std::to_string(port);
}

StreamableHttpServerOptions tlsServerOptions() {
  StreamableHttpServerOptions options;
  options.bind = "127.0.0.1";
  options.port = 0;
  options.tls.enabled = true;
  options.tls.certChainPath = fixtureDir() + "server.pem";
  options.tls.privateKeyPath = fixtureDir() + "server.key";
  return options;
}

// Forwards to the real HTTP transport and signals once McpServer has installed
// all callbacks, so the client can connect without sleeping.
class SignalTransport final : public ITransport {
public:
  SignalTransport(std::shared_ptr<ITransport> inner, std::promise<void> ready)
      : inner_(std::move(inner)), ready_(std::move(ready)) {}

  Status start() override { return inner_->start(); }
  Status send(const Json& message) override { return inner_->send(message); }

  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }

  void setOnError(std::function<void(McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }

  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
    if (!signaled_) {
      signaled_ = true;
      ready_.set_value();
    }
  }

  void close() override { inner_->close(); }

private:
  std::shared_ptr<ITransport> inner_;
  std::promise<void> ready_;
  bool signaled_ = false;
};

class EchoHandler final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&, IProgressSink&) override {
    ToolResult result;
    ContentBlock block;
    block.type = mcp::v1::ContentType::Text;
    block.text = arguments.value("text", std::string{});
    result.content.push_back(block);
    result.structuredContent = Json{{"text", block.text}};
    return ToolOutcome(std::move(result));
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

TEST_CASE("TLS client and server round-trip discover, listTools and callTool") {
  auto created_server = makeStreamableHttpServerTransport(tlsServerOptions());
  REQUIRE(created_server.hasValue());
  std::shared_ptr<ITransport> server_transport = created_server.value();
  REQUIRE(server_transport->start().ok);
  auto concrete =
      std::dynamic_pointer_cast<HttpServerTransport>(server_transport);
  REQUIRE(concrete != nullptr);
  const int port = concrete->boundPort();
  REQUIRE(port > 0);

  std::promise<void> ready;
  auto ready_future = ready.get_future();
  auto signal =
      std::make_shared<SignalTransport>(server_transport, std::move(ready));

  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  McpServer server(core_options, signal);
  REQUIRE(server.tools().add(echoDescriptor(), std::make_shared<EchoHandler>()));
  std::thread server_thread([&server] { server.run(); });
  ready_future.wait();

  StreamableHttpClientOptions client_options;
  client_options.baseUrl = httpsUrlFor(port);
  client_options.requestTimeout = std::chrono::milliseconds(5000);
  client_options.tls.enabled = true;
  client_options.tls.caBundlePath = fixtureDir() + "ca.pem";
  client_options.protocolVersion = "2026-07-28";

  client_options.tls.verifyPeer = true;

  auto created_client = makeStreamableHttpClientTransport(client_options);
  REQUIRE(created_client.hasValue());

  ClientOptions core_client_options;
  core_client_options.timeoutMs = 5000;
  core_client_options.protocolVersion = "2026-07-28";
  McpClient client(core_client_options, created_client.value());

  const Status connected = client.connect();
  if (!connected.ok) {
    MESSAGE("connect failed: " << connected.error.message << " data="
                               << connected.error.data.dump());
    client.close();
    server.stop();
    server_thread.join();
  }
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

  auto called = client.callTool("echo", Json{{"text", "tls"}});
  REQUIRE(called.hasValue());
  CHECK(called.value().at("structuredContent").at("text") == "tls");
  CHECK(called.value().at("content").at(0).at("text") == "tls");

  client.close();
  server.stop();
  server_thread.join();
}

TEST_CASE("TLS client with an untrusted CA reports tls_handshake_failed") {
  auto created_server = makeStreamableHttpServerTransport(tlsServerOptions());
  REQUIRE(created_server.hasValue());
  std::shared_ptr<ITransport> server_transport = created_server.value();
  REQUIRE(server_transport->start().ok);
  auto concrete =
      std::dynamic_pointer_cast<HttpServerTransport>(server_transport);
  REQUIRE(concrete != nullptr);
  const int port = concrete->boundPort();
  REQUIRE(port > 0);

  StreamableHttpClientOptions client_options;
  client_options.baseUrl = httpsUrlFor(port);
  client_options.requestTimeout = std::chrono::milliseconds(5000);
  client_options.tls.enabled = true;
  // Deliberately trust only the system store; the loopback server uses a
  // private CA. Verification must fail before any JSON-RPC response exists.
  client_options.tls.verifyPeer = true;
  client_options.tls.caBundlePath.clear();

  auto created_client = makeStreamableHttpClientTransport(client_options);
  REQUIRE(created_client.hasValue());
  std::shared_ptr<ITransport> client_transport = created_client.value();
  REQUIRE(client_transport->start().ok);

  const Status status = client_transport->send(
      Json{{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "ping"},
           {"params", Json::object()}});
  REQUIRE_FALSE(status.ok);
  REQUIRE(status.error.data.is_object());
  CHECK(status.error.data.at("reason") == "tls_handshake_failed");

  client_transport->close();
  server_transport->close();
}

#endif // TUGMCP_TLS_ENABLED
