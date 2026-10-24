// Legacy HTTP+SSE adapter wire tests.

#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"

#include <tugmcp/client.hpp>
#include <tugmcp/security.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/legacy_sse.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using mcp::v1::AuthContext;
using mcp::v1::ContentBlock;
using mcp::v1::ContentType;
using mcp::v1::Headers;
using mcp::v1::IAuthenticator;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::LegacySseClientOptions;
using mcp::v1::LegacySseClientTransport;
using mcp::v1::LegacySseServerOptions;
using mcp::v1::LegacySseServerTransport;
using mcp::v1::McpClient;
using mcp::v1::McpServer;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolResult;
using mcp::v1::makeLegacySseClientTransport;
using mcp::v1::makeLegacySseServerTransport;

namespace {

std::string baseUrlFor(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

class EchoTool final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json&, RequestContext&,
                             mcp::v1::IProgressSink&) override {
    ToolResult result;
    result.content.push_back(ContentBlock{ContentType::Text, "ok"});
    return ToolOutcome(std::move(result));
  }
};

class GoodAuthenticator final : public IAuthenticator {
public:
  Result<AuthContext> authenticate(const Headers& headers) override {
    const auto it = headers.find("Authorization");
    if (it == headers.end() || it->second != "Bearer good") {
      return Result<AuthContext>::failure(mcp::v1::McpError::make(
          mcp::v1::ErrorCode::InternalError, "unauthorized"));
    }
    AuthContext auth;
    auth.authenticated = true;
    return auth;
  }
};

class RunningLegacyServer {
public:
  explicit RunningLegacyServer(LegacySseServerOptions options) {
    options.bind = "127.0.0.1";
    options.port = 0;
    auto created = makeLegacySseServerTransport(options);
    REQUIRE(created.hasValue());
    transport_ = created.value();
    concrete_ =
        std::dynamic_pointer_cast<LegacySseServerTransport>(transport_);
    REQUIRE(concrete_ != nullptr);

    ServerOptions core_options;
    core_options.allowUnvalidatedTools = true;
    server_ = std::make_unique<McpServer>(core_options, transport_);
    ToolDescriptor descriptor;
    descriptor.name = "echo";
    descriptor.inputSchema = Json{{"type", "object"}};
    server_->tools().add(std::move(descriptor), std::make_shared<EchoTool>());
  }

  ~RunningLegacyServer() { stop(); }

  McpServer& server() { return *server_; }
  void start() {
    thread_ = std::thread([this] { server_->run(); });
    for (int i = 0; i < 200 && concrete_->boundPort() == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(concrete_->boundPort() != 0);
  }
  void stop() {
    if (server_) {
      server_->stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  int port() const { return concrete_->boundPort(); }

private:
  std::shared_ptr<ITransport> transport_;
  std::shared_ptr<LegacySseServerTransport> concrete_;
  std::unique_ptr<McpServer> server_;
  std::thread thread_;
};

struct Reader {
  std::shared_ptr<httplib::Client> client;
  std::mutex mutex;
  std::condition_variable cv;
  std::string data;
  std::thread thread;
};

void startReader(int port, Reader& reader) {
  reader.client = std::make_shared<httplib::Client>(baseUrlFor(port));
  reader.client->set_read_timeout(std::chrono::milliseconds(200));
  auto client = reader.client;
  reader.thread = std::thread([client, &reader] {
    client->Get("/sse", [&reader](const char* data, std::size_t size) {
      std::lock_guard<std::mutex> lock(reader.mutex);
      reader.data.append(data, size);
      reader.cv.notify_all();
      return true;
    });
    client->stop();
  });
}

void stopReader(Reader& reader) {
  if (reader.client) {
    reader.client->stop();
  }
  if (reader.thread.joinable()) {
    reader.thread.join();
  }
}

bool waitForText(Reader& reader, const std::string& needle,
                 std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(reader.mutex);
  return reader.cv.wait_for(lock, timeout, [&] {
    return reader.data.find(needle) != std::string::npos;
  });
}

std::string snapshot(Reader& reader) {
  std::lock_guard<std::mutex> lock(reader.mutex);
  return reader.data;
}

std::string extractEndpoint(const std::string& data) {
  const std::string marker = "event: endpoint\ndata: ";
  const std::size_t start = data.find(marker);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t end = data.find('\n', value_start);
  return data.substr(value_start, end - value_start);
}

int postMessages(int port, const std::string& path, const std::string& body) {
  httplib::Client client(baseUrlFor(port));
  client.set_connection_timeout(std::chrono::seconds(2));
  client.set_read_timeout(std::chrono::seconds(2));
  client.set_write_timeout(std::chrono::seconds(2));
  auto response = client.Post(path, body, "application/json");
  const int status = response ? response->status : -1;
  client.stop();
  return status;
}

Json initializeBody(int id) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "initialize"},
              {"params", Json{{"protocolVersion", "2025-11-25"}}}};
}

Json listBody(int id) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/list"},
              {"params", Json::object()}};
}

} // namespace

TEST_CASE("legacy SSE endpoint, initialize, tools/list and batch") {
  RunningLegacyServer running(LegacySseServerOptions{});
  running.start();

  Reader reader;
  startReader(running.port(), reader);
  REQUIRE(waitForText(reader, "event: endpoint", std::chrono::seconds(2)));
  const std::string endpoint = extractEndpoint(snapshot(reader));
  REQUIRE(endpoint.find("/messages?sessionId=") == 0);
  CHECK(endpoint.size() == std::string("/messages?sessionId=").size() + 32);

  CHECK(postMessages(running.port(), endpoint, initializeBody(1).dump()) ==
        202);
  REQUIRE(waitForText(reader, "\"id\":1", std::chrono::seconds(2)));
  CHECK(snapshot(reader).find("2025-11-25") != std::string::npos);

  CHECK(postMessages(running.port(), endpoint, listBody(2).dump()) == 202);
  REQUIRE(waitForText(reader, "\"id\":2", std::chrono::seconds(2)));

  const Json batch = Json::array({listBody(3), listBody(4)});
  CHECK(postMessages(running.port(), endpoint, batch.dump()) == 202);
  REQUIRE(waitForText(reader, "\"id\":3", std::chrono::seconds(2)));
  REQUIRE(waitForText(reader, "\"id\":4", std::chrono::seconds(2)));
  const std::string data = snapshot(reader);
  CHECK(data.find("data: [") == std::string::npos);

  stopReader(reader);
  running.stop();
}

TEST_CASE("legacy SSE session errors and closed transport") {
  RunningLegacyServer running(LegacySseServerOptions{});
  running.start();

  CHECK(postMessages(running.port(), "/messages", listBody(1).dump()) == 400);
  CHECK(postMessages(running.port(), "/messages?sessionId=deadbeef",
                     listBody(2).dump()) == 404);

  Reader reader;
  startReader(running.port(), reader);
  REQUIRE(waitForText(reader, "event: endpoint", std::chrono::seconds(2)));
  const std::string endpoint = extractEndpoint(snapshot(reader));
  REQUIRE_FALSE(endpoint.empty());

  stopReader(reader);
  running.stop();

  httplib::Client client(baseUrlFor(running.port()));
  client.set_connection_timeout(std::chrono::milliseconds(500));
  client.set_read_timeout(std::chrono::milliseconds(200));
  auto get = client.Get("/sse");
  if (get) {
    CHECK(get->status == 410);
  }
  const int closed_status =
      postMessages(running.port(), endpoint, listBody(3).dump());
  CHECK((closed_status == 410 || closed_status == -1));
  client.stop();
}

TEST_CASE("legacy SSE body limit") {
  LegacySseServerOptions limited;
  limited.maxBodyBytes = 64;
  RunningLegacyServer running(limited);
  running.start();

  Reader reader;
  startReader(running.port(), reader);
  REQUIRE(waitForText(reader, "event: endpoint", std::chrono::seconds(2)));
  const std::string endpoint = extractEndpoint(snapshot(reader));
  REQUIRE_FALSE(endpoint.empty());
  const std::string large(200, 'x');
  CHECK(postMessages(running.port(), endpoint, large) == 413);
  stopReader(reader);
  running.stop();
}

TEST_CASE("legacy SSE authentication") {
  LegacySseServerOptions secured;
  secured.auth = std::make_shared<GoodAuthenticator>();
  RunningLegacyServer secured_running(secured);
  secured_running.start();
  httplib::Client client(baseUrlFor(secured_running.port()));
  auto response = client.Get("/sse");
  REQUIRE(response);
  CHECK(response->status == 401);
  client.stop();
  secured_running.stop();
}
TEST_CASE("legacy SSE client transport loopback") {
  RunningLegacyServer running(LegacySseServerOptions{});
  running.start();

  LegacySseClientOptions options;
  options.baseUrl = baseUrlFor(running.port());
  auto transport = makeLegacySseClientTransport(options);
  REQUIRE(transport.hasValue());

  mcp::v1::ClientOptions client_options;
  client_options.protocolVersion = "2025-11-25";
  client_options.timeoutMs = 5000;
  McpClient client(client_options, transport.value());
  REQUIRE(client.connect().ok);

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  CHECK(listed.value().at("tools").at(0).at("name") == "echo");
  auto called = client.callTool("echo");
  REQUIRE(called.hasValue());
  CHECK(called.value().at("content").at(0).at("text") == "ok");

  client.close();
  running.stop();
}
