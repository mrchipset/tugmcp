// 2025 client session loopback: GET stream, replay, server requests, cancel.

#include <doctest/doctest.h>

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include "tugmcp/detail/http_httplib.hpp"

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
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnSessionClosed(std::move(handler));
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

  // Re-establish a server-side stream without the client, emit event 2 into
  // the replay buffer, then reconnect the real client with Last-Event-ID 1.
  std::thread raw_reader([&] {
    httplib::Client raw(baseUrlFor(running.port()));
    raw.set_read_timeout(std::chrono::seconds(10));
    httplib::Headers raw_headers;
    raw_headers.emplace("Accept", "text/event-stream");
    raw_headers.emplace("MCP-Protocol-Version", "2025-11-25");
    raw_headers.emplace("MCP-Session-Id", client.sessionId());
    raw_headers.emplace("Last-Event-ID", "1");
    raw.Get("/mcp", raw_headers,
            [](const char*, std::size_t) { return true; });
    raw.stop();
  });

  REQUIRE(sendNotificationWithRetry(
      running.server(), client.sessionId(), Json{{"data", "second"}}));
  REQUIRE(client.openStream().ok);
  REQUIRE(capture.waitCount(2, std::chrono::seconds(2)));
  raw_reader.join();

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
TEST_CASE("session A remains uncancelled when session B cancels id 1") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);

  std::atomic<bool> a_observed{false};
  std::atomic<bool> b_observed{false};
  std::promise<void> a_entered;
  auto a_entered_future = a_entered.get_future();
  std::promise<void> b_entered;
  auto b_entered_future = b_entered.get_future();
  addTool(running.server(), "block-a",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            a_entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            a_observed.store(true);
            return textOutcome("a");
          });
  addTool(running.server(), "block-b",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            b_entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            b_observed.store(true);
            return textOutcome("b");
          });
  running.start();

  auto client_a_storage = makeClient(running.port());
  McpClient& client_a = *client_a_storage;
  auto client_b_storage = makeClient(running.port());
  McpClient& client_b = *client_b_storage;
  REQUIRE(client_a.connect().ok);
  REQUIRE(client_b.connect().ok);

  RequestId id_a;
  CallOptions options_a;
  options_a.out_id = &id_a;
  std::optional<Result<Json>> outcome_a;
  std::thread caller_a([&] {
    outcome_a = client_a.callTool("block-a", Json::object(), options_a);
  });
  a_entered_future.wait();

  RequestId id_b;
  CallOptions options_b;
  options_b.out_id = &id_b;
  std::optional<Result<Json>> outcome_b;
  std::thread caller_b([&] {
    outcome_b = client_b.callTool("block-b", Json::object(), options_b);
  });
  b_entered_future.wait();

  REQUIRE(client_b.cancel(id_b, mcp::v1::CancelMode::Notification).ok);
  caller_b.join();
  REQUIRE(outcome_b.has_value());
  REQUIRE_FALSE(outcome_b->hasValue());
  CHECK(outcome_b->error().data.at("reason") == "cancelled");
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!b_observed.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  CHECK(b_observed.load());
  CHECK_FALSE(a_observed.load());

  REQUIRE(client_a.cancel(id_a, mcp::v1::CancelMode::Notification).ok);
  caller_a.join();
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!a_observed.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  CHECK(a_observed.load());

  client_a.close();
  client_b.close();
  running.stop();
}

TEST_CASE("response posted by another session does not complete A") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_a_storage = makeClient(running.port());
  McpClient& client_a = *client_a_storage;
  auto client_b_storage = makeClient(running.port());
  McpClient& client_b = *client_b_storage;
  std::promise<void> handler_entered;
  auto handler_entered_future = handler_entered.get_future();
  std::promise<void> release_handler;
  auto release_future = release_handler.get_future();
  client_a.setRequestHandler("roots/list",
                             [&](const Json&) {
                               handler_entered.set_value();
                               release_future.wait();
                               return Result<Json>(
                                   Json{{"roots", Json::array()}});
                             });
  REQUIRE(client_a.connect().ok);
  REQUIRE(client_b.connect().ok);
  REQUIRE(waitForStream(running.server(), client_a.sessionId()));

  auto future = std::async(std::launch::async, [&] {
    return running.server().requestClient(
        client_a.sessionId(), "roots/list", Json::object(),
        std::chrono::milliseconds(500));
  });
  handler_entered_future.wait();

  httplib::Client raw(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", client_b.sessionId());
  const Json response = Json{{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"result", Json{{"roots", Json::array()}}}};
  auto posted = raw.Post("/mcp", headers, response.dump(),
                         "application/json");
  REQUIRE(posted);
  CHECK(posted->status == 202);

  auto result = future.get();
  REQUIRE_FALSE(result.hasValue());
  CHECK(result.error().data.at("reason") == "timeout");
  const auto last_error = running.server().lastError();
  REQUIRE(last_error.has_value());
  CHECK(last_error->data.at("reason") == "session_mismatch");

  release_handler.set_value();
  client_a.close();
  client_b.close();
  running.stop();
}

TEST_CASE("DELETE completes pending server requests with session_closed") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  std::promise<void> handler_entered;
  auto handler_entered_future = handler_entered.get_future();
  std::promise<void> release_handler;
  auto release_future = release_handler.get_future();
  client.setRequestHandler("roots/list",
                           [&](const Json&) {
                             handler_entered.set_value();
                             release_future.wait();
                             return Result<Json>(
                                 Json{{"roots", Json::array()}});
                           });
  REQUIRE(client.connect().ok);
  REQUIRE(waitForStream(running.server(), client.sessionId()));

  auto future = std::async(std::launch::async, [&] {
    return running.server().requestClient(
        client.sessionId(), "roots/list", Json::object(),
        std::chrono::seconds(2));
  });
  handler_entered_future.wait();

  httplib::Client raw(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", client.sessionId());
  auto removed = raw.Delete("/mcp", headers);
  REQUIRE(removed);
  CHECK(removed->status == 204);

  auto result = future.get();
  REQUIRE_FALSE(result.hasValue());
  CHECK(result.error().data.at("reason") == "session_closed");

  release_handler.set_value();
  client.close();
  running.stop();
}

TEST_CASE("2026-preferring client falls back to a 2025-only server") {
  ServerOptions options;
  options.supportedVersions = {"2025-11-25"};
  RunningServer running(options);
  addTool(running.server(), "echo",
          [](const Json&, RequestContext&, IProgressSink&) {
            return textOutcome("ok");
          });
  running.start();

  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = baseUrlFor(running.port());
  transport_options.requestTimeout = std::chrono::milliseconds(5000);
  transport_options.protocolVersion = "2026-07-28";
  auto transport = makeStreamableHttpClientTransport(transport_options);
  REQUIRE(transport.hasValue());
  ClientOptions client_options;
  client_options.timeoutMs = 5000;
  client_options.protocolVersion = "2026-07-28";
  client_options.allow2025Fallback = true;
  McpClient client(client_options, transport.value());

  REQUIRE(client.connect().ok);
  CHECK_FALSE(client.sessionId().empty());
  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  CHECK(listed.value().at("tools").at(0).at("name") == "echo");

  client.close();
  running.stop();
}

TEST_CASE("2025 per-call timeout notifies the server") {
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
            return textOutcome("late");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  CallOptions call_options;
  call_options.timeout = std::chrono::milliseconds(500);
  std::optional<Result<Json>> outcome;
  std::thread caller([&] {
    outcome = client.callTool("slow", Json::object(), call_options);
  });
  entered_future.wait();
  caller.join();
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  CHECK(outcome->error().data.at("reason") == "timeout");
  CHECK(observed_future.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready);

  client.close();
  running.stop();
}
TEST_CASE("cancel id 1 does not cancel id 11 in the same session") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::atomic<bool> a_observed{false};
  std::atomic<bool> b_cancelled{false};
  std::promise<void> a_entered;
  auto a_entered_future = a_entered.get_future();
  std::promise<void> b_entered;
  auto b_entered_future = b_entered.get_future();
  std::promise<void> release_b;
  auto release_b_future = release_b.get_future();
  addTool(running.server(), "block-a",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            a_entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            a_observed.store(true);
            return textOutcome("a");
          });
  addTool(running.server(), "block-b",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            b_entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              if (release_b_future.wait_for(std::chrono::milliseconds(0)) ==
                  std::future_status::ready) {
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context.cancellation && context.cancellation->cancelled()) {
              b_cancelled.store(true);
            }
            return textOutcome("b");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);
  const std::string session_id = client.sessionId();
  REQUIRE_FALSE(session_id.empty());

  auto raw_call = [&](int id, const std::string& tool, int& status,
                      std::string& body) {
    httplib::Client raw(baseUrlFor(running.port()));
    httplib::Headers headers;
    headers.emplace("MCP-Protocol-Version", "2025-11-25");
    headers.emplace("MCP-Session-Id", session_id);
    headers.emplace("Accept", "application/json");
    const Json request = Json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", Json{{"name", tool},
                                              {"arguments", Json::object()}}}};
    auto response = raw.Post("/mcp", headers, request.dump(),
                             "application/json");
    status = response ? response->status : -1;
    body = response ? response->body : std::string{};
    raw.stop();
  };

  int status_a = 0;
  int status_b = 0;
  std::string body_a;
  std::string body_b;
  std::thread caller_a([&] { raw_call(1, "block-a", status_a, body_a); });
  a_entered_future.wait();
  std::thread caller_b([&] { raw_call(11, "block-b", status_b, body_b); });
  b_entered_future.wait();

  httplib::Client raw(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", session_id);
  const Json cancel = Json{{"jsonrpc", "2.0"},
                           {"method", "notifications/cancelled"},
                           {"params", Json{{"requestId", 1},
                                           {"reason", "cancelled"}}}};
  auto cancelled = raw.Post("/mcp", headers, cancel.dump(),
                            "application/json");
  REQUIRE(cancelled);
  CHECK(cancelled->status == 202);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!a_observed.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(a_observed.load());
  release_b.set_value();
  caller_a.join();
  caller_b.join();

  CHECK_FALSE(b_cancelled.load());
  CHECK(status_b == 200);
  CHECK(Json::parse(body_b).at("result").at("content").at(0).at("text") ==
        "b");

  raw.stop();
  client.close();
  running.stop();
}
