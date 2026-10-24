// Wire-level Streamable HTTP/SSE checks using a raw httplib client.

#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"

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

using mcp::v1::AdmissionDecision;
using mcp::v1::ContentBlock;
using mcp::v1::HttpServerTransport;
using mcp::v1::IAdmissionHook;
using mcp::v1::IProgressSink;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::MessageMetadata;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StreamableHttpServerOptions;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolResult;
using mcp::v1::makeStreamableHttpServerTransport;

namespace {

std::string baseUrlFor(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

httplib::Headers sessionHeaders(const std::string& session_id) {
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", session_id);
  headers.emplace("Accept", "application/json");
  return headers;
}

std::string initializeSession(int port) {
  httplib::Client client(baseUrlFor(port));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("Accept", "application/json");
  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "initialize"},
           {"params", Json{{"protocolVersion", "2025-11-25"}}}};
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  REQUIRE(response->status == 200);
  const std::string id = response->get_header_value("MCP-Session-Id");
  REQUIRE_FALSE(id.empty());
  client.stop();
  return id;
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

  void close() override { inner_->close(); }

  void waitReady() { ready_.get_future().wait(); }

private:
  std::shared_ptr<ITransport> inner_;
  std::promise<void> ready_;
  std::atomic<bool> signaled_{false};
};

class RunningServer {
public:
  RunningServer(StreamableHttpServerOptions http_options,
                ServerOptions core_options = {}) {
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

  std::optional<Status> runStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return run_status_;
  }

private:
  std::shared_ptr<HttpServerTransport> concrete_;
  std::shared_ptr<ReadyTransport> transport_;
  std::unique_ptr<McpServer> server_;
  std::thread thread_;
  std::mutex mutex_;
  std::optional<Status> run_status_;
};

class TextHandler final : public IToolHandler {
public:
  using Invoke = std::function<void(const Json&, RequestContext&,
                                    IProgressSink&)>;

  explicit TextHandler(Invoke invoke) : invoke_(std::move(invoke)) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    invoke_(arguments, context, progress);
    ToolResult result;
    result.content.push_back(ContentBlock{mcp::v1::ContentType::Text, "ok"});
    return ToolOutcome(std::move(result));
  }

private:
  Invoke invoke_;
};

ToolDescriptor descriptor(std::string name) {
  ToolDescriptor value;
  value.name = std::move(name);
  return value;
}

void addTool(McpServer& server, const std::string& name,
             TextHandler::Invoke invoke) {
  REQUIRE(server.tools()
              .add(descriptor(name),
                   std::make_shared<TextHandler>(std::move(invoke)))
              .ok);
}

httplib::Headers sseHeaders(const std::string& method,
                            const std::string& name) {
  httplib::Headers headers;
  headers.emplace("Accept", "application/json, text/event-stream");
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  headers.emplace("Mcp-Method", method);
  if (!name.empty()) {
    headers.emplace("Mcp-Name", name);
  }
  return headers;
}

httplib::Headers jsonHeaders(const std::string& method,
                             const std::string& name) {
  httplib::Headers headers;
  headers.emplace("Accept", "application/json");
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  headers.emplace("Mcp-Method", method);
  if (!name.empty()) {
    headers.emplace("Mcp-Name", name);
  }
  return headers;
}

Json toolCallBody(std::string name, Json arguments, std::int64_t id,
                  const std::string& progress_token) {
  Json params;
  params["name"] = std::move(name);
  params["arguments"] = std::move(arguments);
  Json meta = Json{{"io.modelcontextprotocol/protocolVersion",
                    "2026-07-28"},
                   {"io.modelcontextprotocol/clientCapabilities",
                    Json::object()}};
  if (!progress_token.empty()) {
    meta["progressToken"] = progress_token;
  }
  params["_meta"] = std::move(meta);
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", std::move(params)}};
}

std::vector<Json> parseSse(const std::string& body) {
  std::vector<Json> frames;
  std::size_t offset = 0;
  while (offset < body.size()) {
    const std::size_t end = body.find("\n\n", offset);
    const std::string frame =
        body.substr(offset, end == std::string::npos ? std::string::npos
                                                     : end - offset);
    const std::size_t data_pos = frame.find("data: ");
    if (data_pos != std::string::npos) {
      const std::size_t line_end = frame.find('\n', data_pos);
      const std::string data =
          frame.substr(data_pos + 6,
                       line_end == std::string::npos
                           ? std::string::npos
                           : line_end - (data_pos + 6));
      if (!data.empty()) {
        frames.push_back(Json::parse(data));
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 2;
  }
  return frames;
}

class AlwaysAccept final : public IAdmissionHook {
public:
  AdmissionDecision admit(const RequestContext&, const Json&) override {
    return AdmissionDecision::Accept;
  }
};

class AlwaysReject final : public IAdmissionHook {
public:
  AdmissionDecision admit(const RequestContext&, const Json&) override {
    return AdmissionDecision::Reject;
  }
};

class AlwaysEmergency final : public IAdmissionHook {
public:
  AdmissionDecision admit(const RequestContext&, const Json&) override {
    return AdmissionDecision::Emergency;
  }
};

class RecordingExecutor final : public mcp::v1::IExecutor {
public:
  mcp::v1::ExecutorSubmitResult post(std::function<void()> task,
                                     mcp::v1::Priority priority) noexcept override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      priorities_.push_back(priority);
    }
    task();
    return mcp::v1::ExecutorSubmitResult{};
  }

  void drain() noexcept override {}
  void stop() noexcept override {}
  std::size_t workerCount() const noexcept override { return 0; }

  bool saw(mcp::v1::Priority priority) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto value : priorities_) {
      if (value == priority) {
        return true;
      }
    }
    return false;
  }

private:
  mutable std::mutex mutex_;
  std::vector<mcp::v1::Priority> priorities_;
};

} // namespace

TEST_CASE("SSE tool call streams progress and a final response") {
  StreamableHttpServerOptions http_options;
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  RunningServer running(http_options, core_options);
  addTool(running.server(), "streamer",
          [](const Json&, RequestContext&, IProgressSink& progress) {
            progress.report(0.25, "one");
            progress.report(0.75, "two");
          });
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = toolCallBody("streamer", Json::object(), 42, "tok-1");
  auto response = client.Post("/mcp", sseHeaders("tools/call", "streamer"),
                              body.dump(), "application/json");
  INFO("client error: ", response.error());
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(response->get_header_value("Content-Type").find("text/event-stream") !=
        std::string::npos);

  const std::vector<Json> frames = parseSse(response->body);
  int progress_frames = 0;
  bool saw_final = false;
  for (const auto& frame : frames) {
    if (frame.value("method", std::string{}) ==
        "notifications/progress") {
      ++progress_frames;
      CHECK(frame.at("params").at("progressToken") == "tok-1");
    }
    if (frame.contains("id") && frame.value("id", std::int64_t{}) == 42 &&
        frame.contains("result")) {
      saw_final = true;
    }
  }
  CHECK(progress_frames == 2);
  CHECK(saw_final);

  client.stop();
  running.stop();
}

TEST_CASE("JSON request drops progress notifications") {
  StreamableHttpServerOptions http_options;
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  RunningServer running(http_options, core_options);
  addTool(running.server(), "streamer",
          [](const Json&, RequestContext&, IProgressSink& progress) {
            progress.report(0.5, "ignored");
          });
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = toolCallBody("streamer", Json::object(), 43, "tok-2");
  auto response = client.Post("/mcp", jsonHeaders("tools/call", "streamer"),
                              body.dump(), "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(response->body.find("notifications/progress") == std::string::npos);
  const Json parsed = Json::parse(response->body);
  CHECK(parsed.at("id") == 43);
  CHECK(parsed.at("result").at("content").at(0).at("text") == "ok");

  client.stop();
  running.stop();
}

TEST_CASE("admission rejection returns HTTP 429") {
  StreamableHttpServerOptions http_options;
  http_options.admission = std::make_shared<AlwaysReject>();
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  RunningServer running(http_options, core_options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body =
      Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", Json{{"_meta", Json{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", Json::object()}}}}}};
  auto response =
      client.Post("/mcp", jsonHeaders("tools/list", ""), body.dump(), "application/json");
  REQUIRE(response);
  CHECK(response->status == 429);

  client.stop();
  running.stop();
}

TEST_CASE("admission acceptance lets the request through") {
  StreamableHttpServerOptions http_options;
  http_options.admission = std::make_shared<AlwaysAccept>();
  ServerOptions core_options;
  RunningServer running(http_options, core_options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body =
      Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", Json{{"_meta", Json{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", Json::object()}}}}}};
  auto response =
      client.Post("/mcp", jsonHeaders("tools/list", ""), body.dump(), "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(Json::parse(response->body).at("id") == 2);

  client.stop();
  running.stop();
}

TEST_CASE("maxConcurrentStreams rejects a second stream") {
  StreamableHttpServerOptions http_options;
  http_options.maxConcurrentStreams = 1;
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  RunningServer running(http_options, core_options);

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  addTool(running.server(), "block",
          [&entered, &release_future](const Json&, RequestContext&,
                                      IProgressSink& progress) {
            progress.report(0.1, "started");
            entered.set_value();
            release_future.wait();
          });
  addTool(running.server(), "quick",
          [](const Json&, RequestContext&, IProgressSink&) {});
  running.start();

  int first_status = 0;
  bool first_final = false;
  std::thread first([&] {
    httplib::Client client(baseUrlFor(running.port()));
    const Json body = toolCallBody("block", Json::object(), 51, "tok-b");
    auto response = client.Post("/mcp", sseHeaders("tools/call", "block"),
                                body.dump(), "application/json");
    first_status = response ? response->status : -1;
    if (response) {
      for (const auto& frame : parseSse(response->body)) {
        if (frame.contains("id") && frame.value("id", std::int64_t{}) == 51 &&
            frame.contains("result")) {
          first_final = true;
        }
      }
    }
    client.stop();
  });

  entered_future.wait();

  httplib::Client second_client(baseUrlFor(running.port()));
  const Json second_body =
      toolCallBody("quick", Json::object(), 52, "tok-q");
  auto second = second_client.Post("/mcp",
                                   sseHeaders("tools/call", "quick"),
                                   second_body.dump(), "application/json");
  REQUIRE(second);
  CHECK(second->status == 429);
  second_client.stop();

  release.set_value();
  first.join();
  CHECK(first_status == 200);
  CHECK(first_final);

  running.stop();
}

TEST_CASE("same JSON-RPC id on concurrent connections is isolated by route") {
  StreamableHttpServerOptions http_options;
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  RunningServer running(http_options, core_options);

  std::promise<void> both_entered;
  auto both_entered_future = both_entered.get_future();
  std::atomic<int> invocations{0};
  addTool(running.server(), "echo",
          [&](const Json& arguments, RequestContext&, IProgressSink&) {
            if (invocations.fetch_add(1) + 1 == 2) {
              both_entered.set_value();
            }
            both_entered_future.wait();
            (void)arguments;
          });
  running.start();

  struct PostResult {
    int status = -1;
    Json body;
  };
  auto post = [&running](const std::string& tag) {
    httplib::Client client(baseUrlFor(running.port()));
    const Json body = toolCallBody("echo", Json{{"tag", tag}}, 60, "");
    auto response = client.Post("/mcp", jsonHeaders("tools/call", "echo"),
                                body.dump(), "application/json");
    PostResult result;
    if (response) {
      result.status = response->status;
      result.body = Json::parse(response->body);
    }
    client.stop();
    return result;
  };

  PostResult first;
  PostResult second;
  std::thread first_thread([&] { first = post("first"); });
  std::thread second_thread([&] { second = post("second"); });
  first_thread.join();
  second_thread.join();

  REQUIRE(first.status == 200);
  REQUIRE(second.status == 200);
  CHECK(first.body.at("id") == 60);
  CHECK(second.body.at("id") == 60);
  CHECK(invocations.load() == 2);

  running.stop();
}

TEST_CASE("perStreamQueue overflow ends with a stream_overflow error frame") {
  StreamableHttpServerOptions http_options;
  http_options.perStreamQueue = 1;
  ServerOptions core_options;
  core_options.allowUnvalidatedTools = true;
  // The overflow path is deterministic only when the handler enqueues all
  // progress frames before the SSE provider starts draining.
  core_options.useInlineExecutor = true;
  RunningServer running(http_options, core_options);
  addTool(running.server(), "flood",
          [](const Json&, RequestContext&, IProgressSink& progress) {
            progress.report(0.1, "one");
            progress.report(0.2, "two");
            progress.report(0.3, "three");
          });
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = toolCallBody("flood", Json::object(), 70, "tok-f");
  auto response = client.Post("/mcp", sseHeaders("tools/call", "flood"),
                              body.dump(), "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);

  bool saw_error = false;
  const std::vector<Json> frames = parseSse(response->body);
  for (const auto& frame : frames) {
    if (frame.contains("id") && frame.value("id", std::int64_t{}) == 70) {
      REQUIRE(frame.contains("error"));
      CHECK(frame.at("error").at("data").at("reason") ==
            "stream_overflow");
      saw_error = true;
    }
  }
  CHECK(saw_error);

  const std::optional<mcp::v1::McpError> error = running.server().lastError();
  REQUIRE(error.has_value());
  CHECK(error->data.at("reason") == "stream_overflow");

  client.stop();
  running.stop();
}
TEST_CASE("2026 _meta.protocolVersion must match the header") {
  StreamableHttpServerOptions http_options;
  ServerOptions core_options;
  RunningServer running(http_options, core_options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"id", 91},
           {"method", "ping"},
           {"params",
            Json{{"_meta",
                  Json{{"io.modelcontextprotocol/protocolVersion",
                        "2025-11-25"},
                       {"io.modelcontextprotocol/clientCapabilities",
                        Json::object()}}}}}};
  auto response =
      client.Post("/mcp", jsonHeaders("tools/list", ""), body.dump(),
                  "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);
  const Json error = Json::parse(response->body);
  CHECK(error.at("id") == 91);
  CHECK(error.at("error").at("code") == -32020);

  client.stop();
  running.stop();
}
TEST_CASE("emergency admission posts with Emergency priority") {
  StreamableHttpServerOptions http_options;
  http_options.admission = std::make_shared<AlwaysEmergency>();
  auto executor = std::make_shared<RecordingExecutor>();
  ServerOptions core_options;
  core_options.executor = executor;
  RunningServer running(http_options, core_options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 95},
                         {"method", "tools/list"},
                         {"params",
                          Json{{"_meta",
                                Json{{"io.modelcontextprotocol/protocolVersion",
                                      "2026-07-28"},
                                     {"io.modelcontextprotocol/"
                                      "clientCapabilities",
                                      Json::object()}}}}}};
  auto response =
      client.Post("/mcp", jsonHeaders("tools/list", ""), body.dump(),
                  "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(executor->saw(mcp::v1::Priority::Emergency));

  client.stop();
  running.stop();
}
TEST_CASE("2026 missing _meta returns 400 InvalidParams with the id") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 8},
                         {"method", "tools/list"},
                         {"params", Json::object()}};
  auto response = client.Post("/mcp", jsonHeaders("tools/list", ""),
                              body.dump(), "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);
  const Json error = Json::parse(response->body);
  CHECK(error.at("id") == 8);
  CHECK(error.at("error").at("code") == -32602);

  client.stop();
  running.stop();
}

TEST_CASE("2026 removed methods return 404 MethodNotFound with the id") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const std::vector<std::string> methods = {
      "initialize", "ping", "logging/setLevel", "resources/subscribe",
      "resources/unsubscribe", "unknown/method"};
  for (std::size_t index = 0; index < methods.size(); ++index) {
    const Json body =
        Json{{"jsonrpc", "2.0"},
             {"id", 100 + static_cast<int>(index)},
             {"method", methods[index]},
             {"params",
              Json{{"_meta",
                    Json{{"io.modelcontextprotocol/protocolVersion",
                          "2026-07-28"},
                         {"io.modelcontextprotocol/clientCapabilities",
                          Json::object()}}}}}};
    auto response = client.Post("/mcp",
                                jsonHeaders(methods[index], ""),
                                body.dump(), "application/json");
    REQUIRE(response);
    CHECK(response->status == 404);
    const Json error = Json::parse(response->body);
    CHECK(error.at("id") == 100 + static_cast<int>(index));
    CHECK(error.at("error").at("code") == -32601);
  }

  client.stop();
  running.stop();
}

TEST_CASE("unsupported version reports requested and supported") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "1999-01-01");
  headers.emplace("Mcp-Method", "tools/list");
  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"id", 9},
           {"method", "tools/list"},
           {"params",
            Json{{"_meta",
                  Json{{"io.modelcontextprotocol/protocolVersion",
                        "1999-01-01"},
                       {"io.modelcontextprotocol/clientCapabilities",
                        Json::object()}}}}}};
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);
  const Json error = Json::parse(response->body);
  CHECK(error.at("id") == 9);
  CHECK(error.at("error").at("code") == -32022);
  CHECK(error.at("error").at("data").at("requested") == "1999-01-01");
  CHECK(error.at("error").at("data").at("supported").is_array());

  client.stop();
  running.stop();
}

TEST_CASE("Host and Origin rebinding attempts return 403") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"id", 10},
           {"method", "tools/list"},
           {"params",
            Json{{"_meta",
                  Json{{"io.modelcontextprotocol/protocolVersion",
                        "2026-07-28"},
                       {"io.modelcontextprotocol/clientCapabilities",
                        Json::object()}}}}}};
  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers = jsonHeaders("tools/list", "");
  headers.emplace("Host", "evil.example.com");
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 403);

  headers = jsonHeaders("tools/list", "");
  headers.emplace("Origin", "null");
  auto origin_response = client.Post("/mcp", headers, body.dump(),
                                     "application/json");
  REQUIRE(origin_response);
  CHECK(origin_response->status == 403);

  client.stop();
  running.stop();
}

TEST_CASE("2025 unknown methods still return HTTP 200") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();
  const std::string session_id = initializeSession(running.port());

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers = sessionHeaders(session_id);
  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 11},
                         {"method", "unknown/method"},
                         {"params", Json::object()}};
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  const Json error = Json::parse(response->body);
  CHECK(error.at("error").at("code") == -32601);

  client.stop();
  running.stop();
}
TEST_CASE("unsupported version reports the transport supported list") {
  ServerOptions core_options;
  core_options.supportedVersions = {"2025-11-25"};
  RunningServer running(StreamableHttpServerOptions{}, core_options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "1999-01-01");
  headers.emplace("Mcp-Method", "tools/list");
  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"id", 12},
           {"method", "tools/list"},
           {"params",
            Json{{"_meta",
                  Json{{"io.modelcontextprotocol/protocolVersion",
                        "1999-01-01"},
                       {"io.modelcontextprotocol/clientCapabilities",
                        Json::object()}}}}}};
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);
  const Json error = Json::parse(response->body);
  CHECK(error.at("id") == 12);
  CHECK(error.at("error").at("code") == -32022);
  CHECK(error.at("error").at("data").at("requested") == "1999-01-01");
  CHECK(error.at("error").at("data").at("supported") ==
        Json::array({"2025-11-25"}));

  client.stop();
  running.stop();
}

TEST_CASE("2026 notification with bad _meta gets an empty 400 body") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body =
      Json{{"jsonrpc", "2.0"},
           {"method", "notifications/message"},
           {"params", Json::object()}};
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  headers.emplace("Mcp-Method", "notifications/message");
  auto response = client.Post("/mcp", headers, body.dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 400);
  CHECK(response->body.empty());

  client.stop();
  running.stop();
}
