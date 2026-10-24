// 2025 client session loopback: GET stream, replay, server requests, cancel.

#include <doctest/doctest.h>

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using mcp::v1::CallOptions;
using mcp::v1::CancellationToken;
using mcp::v1::ClientOptions;
using mcp::v1::ContentBlock;
using mcp::v1::ContentType;
using mcp::v1::ErrorCode;
using mcp::v1::HttpServerTransport;
using mcp::v1::IProgressSink;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpClient;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::RequestContext;
using mcp::v1::RequestId;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StreamableHttpClientOptions;
using mcp::v1::StreamableHttpServerOptions;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolResult;
using mcp::v1::makeStreamableHttpClientTransport;
using mcp::v1::makeStreamableHttpServerTransport;

namespace {

std::string baseUrlFor(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

class ReadyTransport final : public ITransport {
public:
  explicit ReadyTransport(std::shared_ptr<ITransport> inner)
      : inner_(std::move(inner)) {}

  Status start() override {
    Status status = inner_->start();
    if (status && !signaled_.exchange(true)) {
      ready_.set_value();
    }
    return status;
  }

  Status send(const Json& message) override { return inner_->send(message); }
  Status sendTo(std::string_view route, const Json& message) override {
    return inner_->sendTo(route, message);
  }
  Status sendToSession(std::string_view id, const Json& message) override {
    return inner_->sendToSession(id, message);
  }
  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }
  void setOnMessageEx(MessageHandlerEx handler) override {
    inner_->setOnMessageEx(std::move(handler));
  }
  void setOnError(std::function<void(mcp::v1::McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }
  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
  }
  void setOnRouteClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnRouteClosed(std::move(handler));
  }
  void close() override { inner_->close(); }
  void waitReady() { ready_.get_future().wait(); }

private:
  std::shared_ptr<ITransport> inner_;
  std::promise<void> ready_;
  std::atomic<bool> signaled_{false};
};

class RunningServer {
public:
  explicit RunningServer(ServerOptions options) {
    StreamableHttpServerOptions http_options;
    http_options.bind = "127.0.0.1";
    http_options.port = 0;
    auto created = makeStreamableHttpServerTransport(http_options);
    REQUIRE(created.hasValue());
    concrete_ = std::dynamic_pointer_cast<HttpServerTransport>(created.value());
    REQUIRE(concrete_ != nullptr);
    transport_ = std::make_shared<ReadyTransport>(created.value());
    server_ = std::make_unique<McpServer>(std::move(options), transport_);
  }

  ~RunningServer() { stop(); }

  McpServer& server() { return *server_; }

  void start() {
    thread_ = std::thread([this] { server_->run(); });
    transport_->waitReady();
  }

  void stop() {
    if (!server_) {
      return;
    }
    server_->stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  int port() const { return concrete_->boundPort(); }

private:
  std::shared_ptr<HttpServerTransport> concrete_;
  std::shared_ptr<ReadyTransport> transport_;
  std::unique_ptr<McpServer> server_;
  std::thread thread_;
};

class TestTool final : public IToolHandler {
public:
  using Invoke = std::function<Result<ToolOutcome>(
      const Json&, RequestContext&, IProgressSink&)>;

  explicit TestTool(Invoke invoke) : invoke_(std::move(invoke)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    return invoke_(arguments, context, progress);
  }

private:
  Invoke invoke_;
};

void addTool(McpServer& server, const std::string& name,
             TestTool::Invoke invoke) {
  ToolDescriptor descriptor;
  descriptor.name = name;
  REQUIRE(server.tools()
              .add(std::move(descriptor),
                   std::make_shared<TestTool>(std::move(invoke)))
              .ok);
}

Result<ToolOutcome> textOutcome(std::string text) {
  ToolResult result;
  result.content.push_back(ContentBlock{ContentType::Text, std::move(text)});
  return Result<ToolOutcome>(ToolOutcome(std::move(result)));
}

std::unique_ptr<McpClient> makeClient(int port) {
  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = baseUrlFor(port);
  transport_options.requestTimeout = std::chrono::milliseconds(5000);
  transport_options.protocolVersion = "2025-11-25";
  auto transport = makeStreamableHttpClientTransport(transport_options);
  REQUIRE(transport.hasValue());

  ClientOptions options;
  options.timeoutMs = 5000;
  options.protocolVersion = "2025-11-25";
  return std::make_unique<McpClient>(options, transport.value());
}

class NotificationCapture {
public:
  void push(const Json& notification) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifications_.push_back(notification);
    cv_.notify_all();
  }

  bool waitCount(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return notifications_.size() >= count; });
  }

  std::vector<Json> snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return notifications_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Json> notifications_;
};

bool sendNotificationWithRetry(McpServer& server,
                               const std::string& session_id,
                               const Json& params,
                               std::chrono::milliseconds timeout =
                                   std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (server
            .sendNotification(session_id, "notifications/message", params)
            .ok) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool waitForStream(McpServer& server, const std::string& session_id) {
  return sendNotificationWithRetry(server, session_id,
                                   Json{{"probe", true}});
}

} // namespace

TEST_CASE("2025 client connect creates a session and calls work") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  addTool(running.server(), "echo",
          [](const Json& arguments, RequestContext&, IProgressSink&) {
            return textOutcome(arguments.value("text", std::string{}));
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  const std::string session_id = client.sessionId();
  REQUIRE(session_id.size() == 32);
  for (const char character : session_id) {
    CHECK(((character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f')));
  }

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  CHECK(listed.value().at("tools").at(0).at("name") == "echo");

  auto called = client.callTool("echo", Json{{"text", "hi"}});
  REQUIRE(called.hasValue());
  CHECK(called.value().at("content").at(0).at("text") == "hi");

  client.close();
  running.stop();
}

TEST_CASE("2025 server-to-client requests use the client handler") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  client.setRequestHandler("roots/list",
                           [](const Json&) {
                             return Result<Json>(Json{{"roots", Json::array()}});
                           });
  REQUIRE(client.connect().ok);
  REQUIRE_FALSE(client.sessionId().empty());
  REQUIRE(waitForStream(running.server(), client.sessionId()));

  auto result = running.server().requestClient(
      client.sessionId(), "roots/list", Json::object(),
      std::chrono::seconds(2));
  REQUIRE(result.hasValue());
  CHECK(result.value().at("roots").is_array());

  client.close();
  running.stop();
}

TEST_CASE("2025 server notifications reach setOnNotification") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  NotificationCapture capture;
  client.setOnNotification(
      [&capture](const Json& notification) { capture.push(notification); });
  REQUIRE(client.connect().ok);

  REQUIRE(sendNotificationWithRetry(
      running.server(), client.sessionId(),
      Json{{"level", "info"}, {"data", "hello"}}));
  REQUIRE(capture.waitCount(1, std::chrono::seconds(2)));
  const auto notifications = capture.snapshot();
  CHECK(notifications.at(0).at("method") == "notifications/message");
  CHECK(notifications.at(0).at("params").at("data") == "hello");

  client.close();
  running.stop();
}

TEST_CASE("2025 client stream reconnect replays missed notifications") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  NotificationCapture capture;
  client.setOnNotification(
      [&capture](const Json& notification) { capture.push(notification); });
  REQUIRE(client.connect().ok);

  REQUIRE(sendNotificationWithRetry(
      running.server(), client.sessionId(), Json{{"data", "first"}}));
  REQUIRE(capture.waitCount(1, std::chrono::seconds(2)));

  client.closeStream();
  REQUIRE(running.server()
              .sendNotification(client.sessionId(), "notifications/message",
                                Json{{"data", "second"}})
              .ok);
  REQUIRE(client.openStream().ok);
  REQUIRE(capture.waitCount(2, std::chrono::seconds(2)));

  const auto notifications = capture.snapshot();
  REQUIRE(notifications.size() == 2);
  CHECK(notifications.at(0).at("params").at("data") == "first");
  CHECK(notifications.at(1).at("params").at("data") == "second");

  client.close();
  running.stop();
}

TEST_CASE("2025 token cancellation notifies the server") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> observed;
  auto observed_future = observed.get_future();
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            observed.set_value();
            return textOutcome("cancelled");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  auto token = std::make_shared<CancellationToken>();
  CallOptions call_options;
  call_options.cancellation = token;
  std::optional<Result<Json>> outcome;
  std::thread caller([&] {
    outcome = client.callTool("slow", Json::object(), call_options);
  });
  entered_future.wait();
  token->cancel();
  caller.join();

  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  CHECK(outcome->error().data.at("reason") == "cancelled");
  CHECK(observed_future.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready);

  client.close();
  running.stop();
}

TEST_CASE("2025 unknown client handler returns MethodNotFound") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);
  REQUIRE(waitForStream(running.server(), client.sessionId()));

  auto result = running.server().requestClient(
      client.sessionId(), "unknown/method", Json::object(),
      std::chrono::milliseconds(500));
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::MethodNotFound));

  client.close();
  running.stop();
}
