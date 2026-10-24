// 2026 subscriptions/listen loopback: server push notifications and client routing.

#include <doctest/doctest.h>

#include <tugmcp/client.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/transport/http.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using mcp::v1::ClientOptions;
using mcp::v1::HttpServerTransport;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::ListKind;
using mcp::v1::McpClient;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::RequestId;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StreamableHttpClientOptions;
using mcp::v1::StreamableHttpServerOptions;
using mcp::v1::SubscriptionFilter;
using mcp::v1::SubscriptionId;
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

  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }

  void setOnMessageEx(MessageHandlerEx handler) override {
    inner_->setOnMessageEx(std::move(handler));
  }

  void setOnError(std::function<void(mcp::v1::McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }

  void setOnRouteClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnRouteClosed(std::move(handler));
  }

  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
  }

  void cancelPending(const RequestId& id) override {
    inner_->cancelPending(id);
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

std::unique_ptr<McpClient> makeClient(int port, const std::string& protocol) {
  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = baseUrlFor(port);
  transport_options.requestTimeout = std::chrono::milliseconds(5000);
  transport_options.protocolVersion = protocol;
  auto transport = makeStreamableHttpClientTransport(transport_options);
  REQUIRE(transport.hasValue());

  ClientOptions options;
  options.timeoutMs = 5000;
  options.protocolVersion = protocol;
  return std::make_unique<McpClient>(options, transport.value());
}

} // namespace

TEST_CASE("subscription resource updates are routed by uri and id") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.resources = {"mem://a"};

  std::mutex events_mutex;
  std::vector<Json> events;
  std::promise<void> first_event;
  auto first_event_future = first_event.get_future();
  auto received = client.subscribe(
      filter, [&](const Json& notification) {
        {
          std::lock_guard<std::mutex> lock(events_mutex);
          events.push_back(notification);
        }
        try {
          first_event.set_value();
        } catch (...) {
        }
      });
  REQUIRE(received.hasValue());
  const SubscriptionId subscription_id = received.value();

  running.server().notifyResourceUpdated("mem://b");
  running.server().notifyResourceUpdated("mem://a");
  first_event_future.wait();

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    REQUIRE(events.size() == 1);
    CHECK(events.at(0).at("method") ==
          "notifications/resources/updated");
    CHECK(events.at(0).at("params").at("uri") == "mem://a");
    CHECK(events.at(0).at("params").at("subscriptionId") ==
          subscription_id);
  }

  client.close();
  running.stop();
}

TEST_CASE("subscription listChanged filters by kind") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.listChanged = {ListKind::Tools};

  std::mutex events_mutex;
  std::vector<Json> events;
  std::promise<void> first_event;
  auto first_event_future = first_event.get_future();
  auto received = client.subscribe(
      filter, [&](const Json& notification) {
        {
          std::lock_guard<std::mutex> lock(events_mutex);
          events.push_back(notification);
        }
        try {
          first_event.set_value();
        } catch (...) {
        }
      });
  REQUIRE(received.hasValue());

  running.server().notifyListChanged(ListKind::Prompts);
  running.server().notifyListChanged(ListKind::Tools);
  first_event_future.wait();

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    REQUIRE(events.size() == 1);
    CHECK(events.at(0).at("method") ==
          "notifications/tools/list_changed");
    CHECK(events.at(0).at("params").at("subscriptionId") ==
          received.value());
  }

  client.close();
  running.stop();
}

TEST_CASE("unsubscribe stops delivery and is idempotently unknown") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  std::mutex events_mutex;
  std::vector<Json> events;
  auto record = [&](const Json& notification) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(notification);
  };

  SubscriptionFilter first_filter;
  first_filter.resources = {"mem://a"};
  auto first = client.subscribe(first_filter, record);
  REQUIRE(first.hasValue());
  REQUIRE(client.unsubscribe(first.value()).ok);

  std::promise<void> second_event;
  auto second_event_future = second_event.get_future();
  SubscriptionFilter second_filter;
  second_filter.resources = {"mem://b"};
  auto second = client.subscribe(second_filter, [&](const Json& notification) {
    record(notification);
    try {
      second_event.set_value();
    } catch (...) {
    }
  });
  REQUIRE(second.hasValue());

  running.server().notifyResourceUpdated("mem://a");
  running.server().notifyResourceUpdated("mem://b");
  second_event_future.wait();

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    REQUIRE(events.size() == 1);
    CHECK(events.at(0).at("params").at("uri") == "mem://b");
  }

  auto second_unsubscribe = client.unsubscribe(first.value());
  REQUIRE_FALSE(second_unsubscribe.ok);
  CHECK(second_unsubscribe.error.data.at("reason") ==
        "unknown_subscription");

  client.close();
  running.stop();
}

TEST_CASE("invalid subscription filters return InvalidParams") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.listChanged = {static_cast<ListKind>(999)};
  auto subscribed = client.subscribe(filter, [](const Json&) {});
  REQUIRE_FALSE(subscribed.hasValue());
  CHECK(static_cast<int>(subscribed.error().code) == -32602);

  client.close();
  running.stop();
}

TEST_CASE("2025 subscriptions/listen returns MethodNotFound") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2025-11-25");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.resources = {"mem://a"};
  auto subscribed = client.subscribe(filter, [](const Json&) {});
  REQUIRE_FALSE(subscribed.hasValue());
  CHECK(static_cast<int>(subscribed.error().code) == -32601);

  client.close();
  running.stop();
}

TEST_CASE("server stop closes an open subscription without hanging") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.resources = {"mem://a"};
  auto subscribed = client.subscribe(filter, [](const Json&) {});
  REQUIRE(subscribed.hasValue());

  running.stop();
  client.close();
}
TEST_CASE("server drops a subscription when the client stream closes") {
  ServerOptions options;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port(), "2026-07-28");
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  SubscriptionFilter filter;
  filter.resources = {"mem://a"};
  auto subscribed = client.subscribe(filter, [](const Json&) {});
  REQUIRE(subscribed.hasValue());

  client.close();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (running.server().activeSubscriptions() != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(running.server().activeSubscriptions() == 0);

  running.stop();
}
