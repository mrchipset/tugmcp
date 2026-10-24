// Streamable HTTP loopback: McpClient + McpServer over real localhost sockets,
// plus raw HTTP checks for the protocol surface.

#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using mcp::v1::ClientOptions;
using mcp::v1::ContentBlock;
using mcp::v1::ErrorCode;
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

std::string baseUrlFor(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

struct StartedServer {
  std::shared_ptr<ITransport> transport;
  std::shared_ptr<HttpServerTransport> concrete;
  int port = 0;
};

StartedServer startServer(const StreamableHttpServerOptions& options) {
  auto created = makeStreamableHttpServerTransport(options);
  REQUIRE(created.hasValue());
  auto transport = created.value();
  REQUIRE(transport->start().ok);
  auto concrete = std::dynamic_pointer_cast<HttpServerTransport>(transport);
  REQUIRE(concrete != nullptr);
  return StartedServer{transport, concrete, concrete->boundPort()};
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

TEST_CASE("HTTP client and server round-trip discover, listTools and callTool") {
  StreamableHttpServerOptions server_options;
  server_options.bind = "127.0.0.1";
  server_options.port = 0;

  StartedServer started = startServer(server_options);

  std::promise<void> ready;
  auto ready_future = ready.get_future();
  auto signal = std::make_shared<SignalTransport>(started.transport,
                                                  std::move(ready));

  ServerOptions server_options_core;
  server_options_core.allowUnvalidatedTools = true;
  McpServer server(server_options_core, signal);
  REQUIRE(server.tools().add(echoDescriptor(), std::make_shared<EchoHandler>()));
  std::thread server_thread([&server] { server.run(); });
  ready_future.wait();

  StreamableHttpClientOptions client_options;
  client_options.baseUrl = baseUrlFor(started.port);
  client_options.requestTimeout = std::chrono::milliseconds(2000);
  client_options.protocolVersion = "2026-07-28";
  auto client_transport = makeStreamableHttpClientTransport(client_options);
  REQUIRE(client_transport.hasValue());

  ClientOptions core_client_options;
  core_client_options.timeoutMs = 2000;
  core_client_options.protocolVersion = "2026-07-28";
  McpClient client(core_client_options, client_transport.value());

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

  client.close();
  server.stop();
  server_thread.join();
}

TEST_CASE("2026 GET and DELETE on /mcp return 405") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  httplib::Client client(baseUrlFor(started.port));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  auto get = client.Get("/mcp", headers);
  REQUIRE(get);
  CHECK(get->status == 405);

  auto remove = client.Delete("/mcp", headers);
  REQUIRE(remove);
  CHECK(remove->status == 405);

  client.stop();
  started.transport->close();
}

TEST_CASE("malformed JSON returns 400 with ParseError") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  httplib::Client client(baseUrlFor(started.port));
  auto response = client.Post("/mcp", R"({"jsonrpc":)", "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);

  const Json body = Json::parse(response->body);
  CHECK(body.at("error").at("code") == -32700);

  client.stop();
  started.transport->close();
}

TEST_CASE("unsupported protocol version returns -32022") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2020-01-01");
  httplib::Client client(baseUrlFor(started.port));
  auto response = client.Post(
      "/mcp", headers,
      R"({"jsonrpc":"2.0","id":4,"method":"ping","params":{}})",
      "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);

  const Json body = Json::parse(response->body);
  CHECK(body.at("error").at("code") == -32022);

  client.stop();
  started.transport->close();
}

TEST_CASE("Mcp-Method mismatch returns -32020") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  headers.emplace("Mcp-Method", "tools/list");
  httplib::Client client(baseUrlFor(started.port));
  auto response = client.Post(
      "/mcp", headers,
      R"({"jsonrpc":"2.0","id":5,"method":"ping","params":{}})",
      "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);

  const Json body = Json::parse(response->body);
  CHECK(body.at("error").at("code") == -32020);

  client.stop();
  started.transport->close();
}

TEST_CASE("body larger than maxBodyBytes returns 413") {
  StreamableHttpServerOptions options;
  options.maxBodyBytes = 64;
  StartedServer started = startServer(options);

  std::string body =
      R"({"jsonrpc":"2.0","id":6,"method":"ping","params":{"pad":")";
  body.append(200, 'a');
  body += R"("}})";

  httplib::Client client(baseUrlFor(started.port));
  auto response = client.Post("/mcp", body, "application/json");
  REQUIRE(response);
  CHECK(response->status == 413);

  client.stop();
  started.transport->close();
}

namespace {

class DummyAdmission final : public mcp::v1::IAdmissionHook {
public:
  mcp::v1::AdmissionDecision admit(const RequestContext&,
                                   const Json&) override {
    return mcp::v1::AdmissionDecision::Accept;
  }
};

} // namespace

TEST_CASE("concurrent requests may reuse a JSON-RPC id across routes") {
  StartedServer started = startServer(StreamableHttpServerOptions{});
  ITransport* transport = started.transport.get();

  std::promise<void> both_entered;
  auto both_entered_future = both_entered.get_future();
  std::atomic<int> invocations{0};

  transport->setOnMessageEx(
      [&, transport](Json message, const mcp::v1::MessageMetadata& metadata) {
        if (invocations.fetch_add(1) + 1 == 2) {
          both_entered.set_value();
        }
        both_entered_future.wait();

        Json response;
        response["jsonrpc"] = "2.0";
        response["id"] = message.at("id");
        response["result"] = Json{{"echo", message.at("params").at("tag")}};
        transport->sendTo(metadata.route, response);
      });

  std::string first_body =
      R"({"jsonrpc":"2.0","id":77,"method":"ping","params":{"tag":"first"}})";
  std::string second_body =
      R"({"jsonrpc":"2.0","id":77,"method":"ping","params":{"tag":"second"}})";

  struct PostResult {
    int status = -1;
    std::string body;
  };
  auto post = [&started](const std::string& body) {
    httplib::Client client(baseUrlFor(started.port));
    auto response = client.Post("/mcp", body, "application/json");
    PostResult result;
    if (response) {
      result.status = response->status;
      result.body = response->body;
    }
    client.stop();
    return result;
  };

  PostResult first;
  PostResult second;
  std::thread first_thread([&] { first = post(first_body); });
  std::thread second_thread([&] { second = post(second_body); });
  first_thread.join();
  second_thread.join();

  REQUIRE(first.status == 200);
  REQUIRE(second.status == 200);
  CHECK(Json::parse(first.body).at("result").at("echo") == "first");
  CHECK(Json::parse(second.body).at("result").at("echo") == "second");
  CHECK(invocations.load() == 2);

  started.transport->close();
}

TEST_CASE("HTTP transport close notifies onClose once") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  std::atomic<int> notifications{0};
  started.transport->setOnClose([&notifications] {
    notifications.fetch_add(1, std::memory_order_relaxed);
  });

  started.transport->close();
  started.transport->close();

  CHECK(notifications.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("server run returns when HTTP transport is closed") {
  StartedServer started = startServer(StreamableHttpServerOptions{});

  std::promise<void> ready;
  auto ready_future = ready.get_future();
  auto signal = std::make_shared<SignalTransport>(started.transport,
                                                  std::move(ready));

  McpServer server(ServerOptions{}, signal);
  std::optional<Status> run_status;
  std::thread server_thread([&server, &run_status] {
    run_status = server.run();
  });

  ready_future.wait();
  started.transport->close();
  server_thread.join();

  REQUIRE(run_status.has_value());
  CHECK(run_status->ok);
}

TEST_CASE("client protocol version controls Mcp-Method header") {
  httplib::Server capture;
  std::mutex mutex;
  std::string seen_version;
  std::string seen_method;

  capture.Post("/mcp", [&](const httplib::Request& request,
                           httplib::Response& response) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      seen_version = request.get_header_value("MCP-Protocol-Version");
      seen_method = request.get_header_value("Mcp-Method");
    }
    response.status = 202;
  });

  const int port = capture.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);
  std::thread capture_thread([&capture] { capture.listen_after_bind(); });

  auto send_ping = [&](const std::string& protocol, int id) {
    StreamableHttpClientOptions options;
    options.baseUrl = baseUrlFor(port);
    options.protocolVersion = protocol;
    auto created = makeStreamableHttpClientTransport(options);
    REQUIRE(created.hasValue());
    auto transport = created.value();
    REQUIRE(transport->start().ok);
    const Status status = transport->send(
        Json{{"jsonrpc", "2.0"},
             {"id", id},
             {"method", "ping"},
             {"params", Json::object()}});
    CHECK(status.ok);
    transport->close();
  };

  send_ping("2026-07-28", 1);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(seen_version == "2026-07-28");
    CHECK(seen_method == "ping");
  }

  send_ping("2025-11-25", 2);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(seen_version == "2025-11-25");
    CHECK(seen_method.empty());
  }

  capture.stop();
  capture_thread.join();
}

TEST_CASE("admission hook option is accepted") {
  StreamableHttpServerOptions options;
  options.admission = std::make_shared<DummyAdmission>();

  auto created = makeStreamableHttpServerTransport(options);
  REQUIRE(created.hasValue());
  created.value()->close();
}

#if !TUGMCP_TLS_ENABLED
TEST_CASE("https client transport is rejected when TLS is not built") {
  StreamableHttpClientOptions options;
  options.baseUrl = "https://127.0.0.1:1";

  auto created = makeStreamableHttpClientTransport(options);
  REQUIRE_FALSE(created.hasValue());
  CHECK(static_cast<int>(created.error().code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  REQUIRE(created.error().data.is_object());
  CHECK(created.error().data.at("reason") == "unsupported_scheme");
}
#endif
