#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <tugmcp/executor.hpp>
#include <tugmcp/server.hpp>

#include "../helpers/fake_transport.hpp"

namespace mcp {
namespace v1 {
namespace test {}
} // namespace v1
} // namespace mcp

using namespace mcp::v1;
using namespace mcp::v1::test;

namespace {

// Deterministic wrapper: once the inner fake starts, it delivers one scripted
// message and then emits close on the same thread. This keeps server_test free
// of sleeps and polling while still using the shared FakeTransport helper.
class ScriptedTransport final : public ITransport {
public:
  ScriptedTransport(std::shared_ptr<FakeTransport> inner, Json request)
      : inner_(std::move(inner)), request_(std::move(request)) {}

  Status start() override {
    Status status = inner_->start();
    if (!status) {
      return status;
    }
    inner_->deliver(request_);
    inner_->emitClose();
    return status;
  }

  Status send(const Json& message) override {
    sent_.push_back(message);
    return inner_->send(message);
  }

  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }

  void setOnError(std::function<void(McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }

  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
  }

  void close() override { inner_->close(); }

  const std::vector<Json>& sent() const noexcept { return sent_; }

private:
  std::shared_ptr<FakeTransport> inner_;
  Json request_;
  std::vector<Json> sent_;
};

Json initializeRequest() {
  return Json{{"jsonrpc", "2.0"},
              {"id", 1},
              {"method", "initialize"},
              {"params", Json::object()}};
}

} // namespace

TEST_CASE("server sends a response and run returns after emitClose") {
  auto inner = std::make_shared<FakeTransport>();
  auto transport =
      std::make_shared<ScriptedTransport>(inner, initializeRequest());

  ServerOptions options;
  options.name = "integration-server";
  options.version = "2.0.0";
  McpServer server(options, transport);

  Status status = server.run();
  REQUIRE(status.ok);

  REQUIRE(transport->sent().size() == 1);
  const Json& response = transport->sent().front();
  CHECK(response.at("jsonrpc") == "2.0");
  CHECK(response.at("id") == 1);
  CHECK_FALSE(response.contains("error"));
  CHECK(response.at("result").at("protocolVersion") == "2025-11-25");
  CHECK(response.at("result").at("serverInfo").at("name") ==
        "integration-server");
  CHECK(response.at("result").at("serverInfo").at("version") == "2.0.0");
}

TEST_CASE("notification delivery produces no response") {
  auto inner = std::make_shared<FakeTransport>();
  auto transport = std::make_shared<ScriptedTransport>(
      inner, Json{{"jsonrpc", "2.0"},
                  {"method", "notifications/initialized"},
                  {"params", Json::object()}});

  McpServer server(ServerOptions{}, transport);
  Status status = server.run();
  REQUIRE(status.ok);
  CHECK(transport->sent().empty());
}

TEST_CASE("emitClose after run returns is harmless") {
  auto inner = std::make_shared<FakeTransport>();
  auto transport =
      std::make_shared<ScriptedTransport>(inner, initializeRequest());

  McpServer server(ServerOptions{}, transport);
  Status status = server.run();
  REQUIRE(status.ok);
  REQUIRE(transport->sent().size() == 1);

  // run() must have cleared callbacks and closed the transport; late framing
  // events must be ignored rather than reaching a destroyed server.
  inner->emitClose();
  inner->deliver(initializeRequest());
  inner->emitError(McpError::make(ErrorCode::InternalError, "late"));

  CHECK(transport->sent().size() == 1);
  CHECK(inner->closed());
}

TEST_CASE("server run fails when transport is null") {
  McpServer server(ServerOptions{}, nullptr);
  Status status = server.run();
  REQUIRE_FALSE(status.ok);
  CHECK(static_cast<int>(status.error.code) ==
        static_cast<int>(ErrorCode::InternalError));
}

namespace {

class ScopedToolHandler final : public IToolHandler {
public:
  using Invoke = std::function<Result<ToolOutcome>(const Json&,
                                                   RequestContext&,
                                                   IProgressSink&)>;

  explicit ScopedToolHandler(Invoke invoke) : invoke_(std::move(invoke)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    return invoke_(arguments, context, progress);
  }

private:
  Invoke invoke_;
};

ToolDescriptor handlerTool(std::string name) {
  ToolDescriptor descriptor;
  descriptor.name = std::move(name);
  return descriptor;
}

Json toolsCallRequest(std::string name, std::int64_t id = 1) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", Json{{"name", std::move(name)},
                              {"arguments", Json::object()}}}};
}

class ManualTransport final : public ITransport {
public:
  Status start() override {
    ready_.set_value();
    return Status::success();
  }

  Status send(const Json& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sent_.push_back(message);
    return Status::success();
  }

  void setOnMessage(std::function<void(Json)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(handler);
  }

  void setOnMessageEx(MessageHandlerEx handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ex_ = std::move(handler);
  }

  void setOnError(std::function<void(McpError)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_close_ = std::move(handler);
  }

  void close() override {}

  void waitReady() { ready_.get_future().wait(); }

  void deliver(Json message) { deliver(std::move(message), {}); }

  void deliver(Json message, MessageMetadata metadata) {
    MessageHandlerEx extended;
    std::function<void(Json)> simple;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      extended = on_message_ex_;
      simple = on_message_;
    }
    if (extended) {
      extended(std::move(message), metadata);
    } else if (simple) {
      simple(std::move(message));
    }
  }

  std::vector<Json> sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
  }

private:
  mutable std::mutex mutex_;
  std::promise<void> ready_;
  std::vector<Json> sent_;
  std::function<void(Json)> on_message_;
  MessageHandlerEx on_message_ex_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
};

class RejectingExecutor final : public IExecutor {
public:
  explicit RejectingExecutor(ExecutorSubmitResult::Kind kind) : kind_(kind) {}

  ExecutorSubmitResult post(std::function<void()>,
                            Priority) noexcept override {
    return ExecutorSubmitResult{kind_};
  }

  void drain() noexcept override {}
  void stop() noexcept override {}
  std::size_t workerCount() const noexcept override { return 0; }

private:
  ExecutorSubmitResult::Kind kind_;
};

class ThrowingStartTransport final : public ITransport {
public:
  Status start() override { throw std::runtime_error("boom"); }
  Status send(const Json&) override { return Status::success(); }
  void setOnMessage(std::function<void(Json)>) override {}
  void setOnError(std::function<void(McpError)>) override {}
  void setOnClose(std::function<void()>) override {}
  void close() override {}
};

} // namespace

TEST_CASE("injected executor runs the handler off the run thread") {
  auto inner = std::make_shared<FakeTransport>();
  auto transport = std::make_shared<ScriptedTransport>(
      inner, toolsCallRequest("work"));
  auto executor = makeThreadPoolExecutor(1, 0);

  ServerOptions options;
  options.allowUnvalidatedTools = true;
  options.executor = executor;
  McpServer server(options, transport);

  std::atomic<std::thread::id> handler_thread;
  REQUIRE(server.tools()
              .add(handlerTool("work"),
                   std::make_shared<ScopedToolHandler>(
                       [&handler_thread](const Json&, RequestContext&,
                                         IProgressSink&) {
                         handler_thread.store(std::this_thread::get_id());
                         ToolResult result;
                         result.content.push_back(
                             ContentBlock{mcp::v1::ContentType::Text, "done"});
                         return Result<ToolOutcome>(
                             ToolOutcome(std::move(result)));
                       }))
              .ok);

  const Status status = server.run();
  REQUIRE(status.ok);
  REQUIRE(transport->sent().size() == 1);
  CHECK(handler_thread.load() != std::this_thread::get_id());
  CHECK(transport->sent().front().at("id") == 1);
  CHECK(transport->sent().front().at("result").at("content").at(0).at(
            "text") == "done");
}

TEST_CASE("stop waits for a blocking synchronous handler to finish") {
  auto transport = std::make_shared<ManualTransport>();

  ServerOptions options;
  options.allowUnvalidatedTools = true;
  McpServer server(options, transport);

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  std::shared_ptr<CancellationToken> captured_token;
  REQUIRE(server.tools()
              .add(handlerTool("block"),
                   std::make_shared<ScopedToolHandler>(
                       [&entered, &release_future,
                        &captured_token](const Json&,
                                         RequestContext& context,
                                         IProgressSink&) {
                         captured_token = context.cancellation;
                         entered.set_value();
                         release_future.wait();
                         ToolResult result;
                         result.content.push_back(
                             ContentBlock{mcp::v1::ContentType::Text, "released"});
                         return Result<ToolOutcome>(
                             ToolOutcome(std::move(result)));
                       }))
              .ok);

  auto run_future = std::async(std::launch::async,
                               [&server] { return server.run(); });
  transport->waitReady();

  std::thread handler_thread(
      [&transport] { transport->deliver(toolsCallRequest("block")); });
  entered_future.wait();

  server.stop();
  CHECK(captured_token != nullptr);
  CHECK(captured_token->cancelled());
  CHECK(run_future.wait_for(std::chrono::milliseconds(0)) ==
        std::future_status::timeout);

  release.set_value();
  handler_thread.join();
  const Status status = run_future.get();
  REQUIRE(status.ok);

  const std::vector<Json> sent = transport->sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent.front().at("id") == 1);
  CHECK(sent.front().at("result").at("content").at(0).at("text") ==
        "released");
}

TEST_CASE("executor rejection returns a JSON-RPC error to the caller") {
  for (const auto kind : {ExecutorSubmitResult::Kind::RejectedQueueFull,
                          ExecutorSubmitResult::Kind::RejectedStopped}) {
    auto inner = std::make_shared<FakeTransport>();
    auto transport = std::make_shared<ScriptedTransport>(
        inner, toolsCallRequest("work", 9));

    ServerOptions options;
    options.allowUnvalidatedTools = true;
    options.executor = std::make_shared<RejectingExecutor>(kind);
    McpServer server(options, transport);
    REQUIRE(server.tools()
                .add(handlerTool("work"),
                     std::make_shared<ScopedToolHandler>(
                         [](const Json&, RequestContext&, IProgressSink&) {
                           return Result<ToolOutcome>(
                               ToolOutcome(ToolResult{}));
                         }))
                .ok);

    const Status status = server.run();
    REQUIRE(status.ok);
    const std::vector<Json> sent = transport->sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent.front().at("id") == 9);
    REQUIRE(sent.front().contains("error"));
    CHECK(sent.front().at("error").at("code") == -32603);
    const std::string expected =
        kind == ExecutorSubmitResult::Kind::RejectedQueueFull ? "queue_full"
                                                              : "stopped";
    CHECK(sent.front().at("error").at("data").at("reason") == expected);
  }
}

TEST_CASE("executor rejection of a notification emits nothing") {
  auto inner = std::make_shared<FakeTransport>();
  auto transport = std::make_shared<ScriptedTransport>(
      inner, Json{{"jsonrpc", "2.0"},
                  {"method", "notifications/initialized"},
                  {"params", Json::object()}});

  ServerOptions options;
  options.executor = std::make_shared<RejectingExecutor>(
      ExecutorSubmitResult::Kind::RejectedQueueFull);
  McpServer server(options, transport);

  const Status status = server.run();
  REQUIRE(status.ok);
  CHECK(transport->sent().empty());
}

TEST_CASE("transport start exception is contained by run") {
  auto transport = std::make_shared<ThrowingStartTransport>();
  McpServer server(ServerOptions{}, transport);

  Status status;
  REQUIRE_NOTHROW(status = server.run());
  REQUIRE_FALSE(status.ok);
  CHECK(static_cast<int>(status.error.code) ==
        static_cast<int>(ErrorCode::InternalError));
  CHECK(status.error.message == "server run failed");
}
TEST_CASE("same request id on two routes both observe stop cancellation") {
  auto transport = std::make_shared<ManualTransport>();

  ServerOptions options;
  options.allowUnvalidatedTools = true;
  McpServer server(options, transport);

  std::atomic<int> entered{0};
  std::promise<void> both_entered;
  auto both_entered_future = both_entered.get_future();
  std::atomic<int> observed_cancelled{0};
  REQUIRE(server.tools()
              .add(handlerTool("block"),
                   std::make_shared<ScopedToolHandler>(
                       [&](const Json&, RequestContext& context,
                           IProgressSink&) {
                         if (entered.fetch_add(1) + 1 == 2) {
                           both_entered.set_value();
                         }
                         while (context.cancellation &&
                                !context.cancellation->cancelled()) {
                           std::this_thread::sleep_for(
                               std::chrono::milliseconds(1));
                         }
                         observed_cancelled.fetch_add(1);
                         ToolResult result;
                         result.content.push_back(ContentBlock{
                             mcp::v1::ContentType::Text, "done"});
                         return Result<ToolOutcome>(
                             ToolOutcome(std::move(result)));
                       }))
              .ok);

  auto run_future =
      std::async(std::launch::async, [&server] { return server.run(); });
  transport->waitReady();

  auto call = toolsCallRequest("block", 1);
  std::thread first([&] {
    MessageMetadata metadata;
    metadata.route = "route-a";
    transport->deliver(call, metadata);
  });
  std::thread second([&] {
    MessageMetadata metadata;
    metadata.route = "route-b";
    transport->deliver(call, metadata);
  });

  both_entered_future.wait();
  server.stop();
  first.join();
  second.join();

  const Status status = run_future.get();
  CHECK(status.ok);
  CHECK(observed_cancelled.load() == 2);
}
