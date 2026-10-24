// 2026 loopback: McpClient discover/call/SSE/MRTR against a real McpServer.

#include <doctest/doctest.h>

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
#include <vector>

using mcp::v1::CallOptions;
using mcp::v1::CancelMode;
using mcp::v1::ClientOptions;
using mcp::v1::ContentBlock;
using mcp::v1::HttpServerTransport;
using mcp::v1::IProgressSink;
using mcp::v1::IPromptProvider;
using mcp::v1::IResourceProvider;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpClient;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::Page;
using mcp::v1::Progress;
using mcp::v1::PromptDescriptor;
using mcp::v1::PromptOutcome;
using mcp::v1::PromptResult;
using mcp::v1::RequestContext;
using mcp::v1::RequestId;
using mcp::v1::ResourceContent;
using mcp::v1::ResourceDescriptor;
using mcp::v1::ResourceOutcome;
using mcp::v1::ResourceTemplate;
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
  void setSupportedVersions(std::vector<std::string> versions) override {
    inner_->setSupportedVersions(std::move(versions));
  }

  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnSessionClosed(std::move(handler));
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
  explicit RunningServer(ServerOptions core_options) {
    StreamableHttpServerOptions http_options;
    http_options.bind = "127.0.0.1";
    http_options.port = 0;
    auto created = makeStreamableHttpServerTransport(http_options);
    REQUIRE(created.hasValue());
    concrete_ = std::dynamic_pointer_cast<HttpServerTransport>(created.value());
    REQUIRE(concrete_ != nullptr);
    transport_ = std::make_shared<ReadyTransport>(created.value());
    server_ = std::make_unique<McpServer>(std::move(core_options), transport_);
  }

  ~RunningServer() { stop(); }

  McpServer& server() { return *server_; }

  void start() {
    thread_ = std::thread([this] {
      Status status = server_->run();
      std::lock_guard<std::mutex> lock(mutex_);
      run_status_ = std::move(status);
    });
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
  std::mutex mutex_;
  std::optional<Status> run_status_;
};

class TestTool final : public IToolHandler {
public:
  using Invoke = std::function<Result<ToolOutcome>(const Json&,
                                                   RequestContext&,
                                                   IProgressSink&)>;

  explicit TestTool(Invoke invoke) : invoke_(std::move(invoke)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    return invoke_(arguments, context, progress);
  }

private:
  Invoke invoke_;
};

ToolDescriptor toolDescriptor(std::string name) {
  ToolDescriptor descriptor;
  descriptor.name = std::move(name);
  return descriptor;
}

void addTool(McpServer& server, const std::string& name,
             TestTool::Invoke invoke) {
  REQUIRE(server.tools()
              .add(toolDescriptor(name),
                   std::make_shared<TestTool>(std::move(invoke)))
              .ok);
}

Result<ToolOutcome> textOutcome(std::string text) {
  ToolResult result;
  result.content.push_back(ContentBlock{mcp::v1::ContentType::Text, std::move(text)});
  result.structuredContent = Json{{"text", result.content.front().text}};
  return Result<ToolOutcome>(ToolOutcome(std::move(result)));
}

class FakeResources final : public IResourceProvider {
public:
  Result<Page<ResourceDescriptor>> list(const mcp::v1::ListParams&,
                                        RequestContext&) override {
    return Page<ResourceDescriptor>{};
  }

  Result<Page<ResourceTemplate>> templates(const mcp::v1::ListParams&,
                                           RequestContext&) override {
    Page<ResourceTemplate> page;
    ResourceTemplate value;
    value.uriTemplate = "file:///{path}";
    value.name = "files";
    page.items.push_back(value);
    return page;
  }

  Result<ResourceOutcome> read(std::string_view uri,
                               RequestContext&) override {
    ResourceContent content;
    content.uri = std::string(uri);
    content.mimeType = "text/plain";
    content.text = "file body";
    return ResourceOutcome(std::move(content));
  }
};

class FakePrompts final : public IPromptProvider {
public:
  Result<Page<PromptDescriptor>> list(const mcp::v1::ListParams&,
                                      RequestContext&) override {
    Page<PromptDescriptor> page;
    PromptDescriptor value;
    value.name = "greet";
    value.description = "Greeting";
    page.items.push_back(value);
    return page;
  }

  Result<PromptOutcome> get(std::string_view name, const Json& arguments,
                            RequestContext&) override {
    PromptResult prompt;
    prompt.description = "Greeting";
    prompt.messages = Json::array(
        {Json{{"role", "user"},
              {"content", Json{{"type", "text"},
                               {"text", arguments.value("who", std::string{
                                                                  "world"})}}}}});
    (void)name;
    return PromptOutcome(std::move(prompt));
  }
};

std::unique_ptr<McpClient> makeClient(int port) {
  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = baseUrlFor(port);
  transport_options.requestTimeout = std::chrono::milliseconds(5000);
  transport_options.protocolVersion = "2026-07-28";
  auto transport = makeStreamableHttpClientTransport(transport_options);
  REQUIRE(transport.hasValue());

  ClientOptions options;
  options.timeoutMs = 5000;
  options.protocolVersion = "2026-07-28";
  return std::make_unique<McpClient>(options, transport.value());
}

} // namespace

TEST_CASE("2026 connect uses discover and reports both supported versions") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  const Status connected = client.connect();
  REQUIRE(connected.ok);

  auto discovered = client.discover();
  REQUIRE(discovered.hasValue());
  const Json& versions = discovered.value().at("supportedVersions");
  REQUIRE(versions.is_array());
  bool has_2026 = false;
  bool has_2025 = false;
  for (const auto& version : versions) {
    if (version == "2026-07-28") {
      has_2026 = true;
    }
    if (version == "2025-11-25") {
      has_2025 = true;
    }
  }
  CHECK(has_2026);
  CHECK(has_2025);

  client.close();
  running.stop();
}

TEST_CASE("2026 listTools and callTool round-trip") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  addTool(running.server(), "echo",
          [](const Json& arguments, RequestContext&, IProgressSink&) {
            ToolResult result;
            const std::string text =
                arguments.value("text", std::string{});
            result.content.push_back(ContentBlock{mcp::v1::ContentType::Text, text});
            result.structuredContent = Json{{"text", text}};
            return ToolOutcome(std::move(result));
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  CHECK(listed.value().at("tools").at(0).at("name") == "echo");

  auto called = client.callTool("echo", Json{{"text", "hi"}});
  REQUIRE(called.hasValue());
  CHECK(called.value().at("content").at(0).at("text") == "hi");
  CHECK(called.value().at("structuredContent").at("text") == "hi");

  client.close();
  running.stop();
}

TEST_CASE("2026 progress callbacks arrive before the final result") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  addTool(running.server(), "progress",
          [](const Json&, RequestContext&, IProgressSink& progress) {
            progress.report(0.25, "one");
            progress.report(0.75, "two");
            return textOutcome("done");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  CallOptions call_options;
  call_options.progressToken = std::string("tok-progress");
  std::vector<Progress> seen;
  call_options.onProgress = [&seen](const Progress& progress) {
    seen.push_back(progress);
  };

  auto called = client.callTool("progress", Json::object(), call_options);
  REQUIRE(called.hasValue());
  REQUIRE(seen.size() == 2);
  CHECK(seen.at(0).token == mcp::v1::ProgressToken{std::string("tok-progress")});
  CHECK(seen.at(1).token == mcp::v1::ProgressToken{std::string("tok-progress")});
  CHECK(seen.at(0).progress == 0.25);
  CHECK(seen.at(1).progress == 0.75);

  client.close();
  running.stop();
}

TEST_CASE("2026 MRTR retry carries requestState and inputResponses") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::string seen_state;
  Json seen_responses = nullptr;
  addTool(running.server(), "ask",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            if (context.requestState.has_value()) {
              seen_state = *context.requestState;
              seen_responses = context.inputResponses;
              return textOutcome("answered");
            }
            mcp::v1::InputRequired input;
            input.inputRequests = Json{{"answer", "boolean"}};
            input.requestState = "state-2026";
            return Result<ToolOutcome>(ToolOutcome(std::move(input)));
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  auto first = client.callTool("ask");
  REQUIRE(first.hasValue());
  CHECK(first.value().at("resultType") == "input_required");
  CHECK(first.value().at("requestState") == "state-2026");

  CallOptions retry;
  retry.requestState = "state-2026";
  retry.inputResponses = Json{{"answer", true}};
  auto second = client.callTool("ask", Json::object(), retry);
  REQUIRE(second.hasValue());
  CHECK(seen_state == "state-2026");
  CHECK(seen_responses == Json{{"answer", true}});
  CHECK(second.value().at("resultType") == "complete");

  client.close();
  running.stop();
}

TEST_CASE("2026 resources and prompts round-trip") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  auto resources = std::make_shared<FakeResources>();
  auto prompts = std::make_shared<FakePrompts>();
  options.resources = resources;
  options.prompts = prompts;
  RunningServer running(options);
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  auto templates = client.listResourceTemplates();
  REQUIRE(templates.hasValue());
  CHECK(templates.value().at("resourceTemplates").at(0).at("uriTemplate") ==
        "file:///{path}");

  auto content = client.readResource("file:///a.txt");
  REQUIRE(content.hasValue());
  CHECK(content.value().at("contents").at(0).at("text") == "file body");
  CHECK(content.value().at("resultType") == "complete");

  auto prompts_listed = client.listPrompts();
  REQUIRE(prompts_listed.hasValue());
  CHECK(prompts_listed.value().at("prompts").at(0).at("name") == "greet");

  auto prompt = client.getPrompt("greet", Json{{"who", "tester"}});
  REQUIRE(prompt.hasValue());
  CHECK(prompt.value().at("description") == "Greeting");
  CHECK(prompt.value().at("messages").at(0).at("content").at("text") ==
        "tester");

  client.close();
  running.stop();
}

TEST_CASE("2026 per-call timeout aborts and leaves the client usable") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext&, IProgressSink&) {
            entered.set_value();
            release_future.wait();
            return textOutcome("late");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  CallOptions call_options;
  call_options.timeout = std::chrono::milliseconds(100);
  auto timed_out = client.callTool("slow", Json::object(), call_options);
  entered_future.wait();
  REQUIRE_FALSE(timed_out.hasValue());
  CHECK(timed_out.error().data.at("reason") == "timeout");
  release.set_value();

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());

  client.close();
  running.stop();
}

TEST_CASE("2026 cancellation token aborts the waiting call") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext&, IProgressSink&) {
            entered.set_value();
            release_future.wait();
            return textOutcome("late");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  auto token = std::make_shared<mcp::v1::CancellationToken>();
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
  release.set_value();

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());

  client.close();
  running.stop();
}

TEST_CASE("2026 cancel(CloseStream) aborts an in-flight call") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext&, IProgressSink&) {
            entered.set_value();
            release_future.wait();
            return textOutcome("late");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  RequestId id;
  CallOptions call_options;
  call_options.out_id = &id;
  std::optional<Result<Json>> outcome;
  std::thread caller([&] {
    outcome = client.callTool("slow", Json::object(), call_options);
  });
  entered_future.wait();

  const Status cancelled = client.cancel(id, CancelMode::CloseStream);
  REQUIRE(cancelled.ok);
  caller.join();
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  CHECK(outcome->error().data.at("reason") == "cancelled");
  release.set_value();

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());

  client.close();
  running.stop();
}

TEST_CASE("2026 client request shape leaves no server-side header errors") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  addTool(running.server(), "ping-tool",
          [](const Json&, RequestContext&, IProgressSink&) {
            return textOutcome("pong");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);
  auto called = client.callTool("ping-tool");
  REQUIRE(called.hasValue());
  CHECK_FALSE(running.server().lastError().has_value());

  client.close();
  running.stop();
}
TEST_CASE("cancelling one call keeps subscriptions and other calls alive") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);

  std::promise<void> slow_entered;
  auto slow_entered_future = slow_entered.get_future();
  std::promise<void> slow_release;
  auto slow_release_future = slow_release.get_future();
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext&, IProgressSink&) {
            slow_entered.set_value();
            slow_release_future.wait();
            return textOutcome("late");
          });
  addTool(running.server(), "echo",
          [](const Json&, RequestContext&, IProgressSink&) {
            return textOutcome("ok");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  mcp::v1::SubscriptionFilter filter;
  filter.listChanged = {mcp::v1::ListKind::Tools};
  std::promise<void> notified;
  auto notified_future = notified.get_future();
  auto subscription = client.subscribe(filter, [&](const Json&) {
    try {
      notified.set_value();
    } catch (...) {
    }
  });
  REQUIRE(subscription.hasValue());

  auto token = std::make_shared<mcp::v1::CancellationToken>();
  CallOptions slow_options;
  slow_options.cancellation = token;
  std::optional<Result<Json>> slow_outcome;
  std::thread slow_caller([&] {
    slow_outcome = client.callTool("slow", Json::object(), slow_options);
  });
  slow_entered_future.wait();

  // A second concurrent call must still succeed while the slow call is
  // blocked and the subscription stream is open.
  auto echo = client.callTool("echo");
  REQUIRE(echo.hasValue());
  CHECK(echo.value().at("structuredContent").at("text") == "ok");

  token->cancel();
  slow_caller.join();
  REQUIRE(slow_outcome.has_value());
  REQUIRE_FALSE(slow_outcome->hasValue());
  CHECK(slow_outcome->error().data.at("reason") == "cancelled");
  slow_release.set_value();

  running.server().notifyListChanged(mcp::v1::ListKind::Tools);
  notified_future.wait();

  client.close();
  running.stop();
}
TEST_CASE("cancelPending before send short-circuits the request") {
  StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = "http://127.0.0.1:1";
  transport_options.protocolVersion = "2026-07-28";
  auto created = mcp::v1::makeStreamableHttpClientTransport(transport_options);
  REQUIRE(created.hasValue());
  std::shared_ptr<ITransport> transport = created.value();
  REQUIRE(transport->start().ok);

  RequestId id;
  id.value = std::int64_t{77};
  transport->cancelPending(id);

  const Json request = Json{{"jsonrpc", "2.0"},
                            {"id", 77},
                            {"method", "ping"},
                            {"params", Json::object()}};
  const Status sent = transport->send(request);
  REQUIRE_FALSE(sent.ok);
  CHECK(sent.error.data.at("reason") == "cancelled");
  transport->close();
}
TEST_CASE("2026 CancelMode::Notification is rejected without sending") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  RunningServer running(options);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::atomic<bool> observed{false};
  addTool(running.server(), "slow",
          [&](const Json&, RequestContext& context, IProgressSink&) {
            entered.set_value();
            while (context.cancellation &&
                   !context.cancellation->cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            observed.store(true);
            return textOutcome("done");
          });
  running.start();

  auto client_storage = makeClient(running.port());
  McpClient& client = *client_storage;
  REQUIRE(client.connect().ok);

  RequestId id;
  CallOptions call_options;
  call_options.out_id = &id;
  std::optional<Result<Json>> outcome;
  std::thread caller([&] {
    outcome = client.callTool("slow", Json::object(), call_options);
  });
  entered_future.wait();

  const Status rejected =
      client.cancel(id, mcp::v1::CancelMode::Notification);
  REQUIRE_FALSE(rejected.ok);
  CHECK(static_cast<int>(rejected.error.code) == -32602);
  CHECK(rejected.error.data.at("reason") ==
        "notification_cancel_unsupported");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK_FALSE(observed.load());

  REQUIRE(client.cancel(id, mcp::v1::CancelMode::CloseStream).ok);
  caller.join();
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  running.stop();
  CHECK(observed.load());

  client.close();
}
