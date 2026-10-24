// Security hook integration tests (auth / authorization / approval / audit).

#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"

#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/security.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

#include <chrono>
#include <functional>
#include <future>
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
using mcp::v1::IApprovalHook;
using mcp::v1::IAuditHook;
using mcp::v1::IAuthenticator;
using mcp::v1::IAuthorizer;
using mcp::v1::IPromptProvider;
using mcp::v1::IResourceProvider;
using mcp::v1::IToolHandler;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::Page;
using mcp::v1::PromptDescriptor;
using mcp::v1::PromptOutcome;
using mcp::v1::PromptResult;
using mcp::v1::RequestContext;
using mcp::v1::ResourceContent;
using mcp::v1::ResourceDescriptor;
using mcp::v1::ResourceOutcome;
using mcp::v1::ResourceTemplate;
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
  void setSupportedVersions(std::vector<std::string> versions) override {
    inner_->setSupportedVersions(std::move(versions));
  }
  void setAuthenticator(std::shared_ptr<IAuthenticator> auth) override {
    inner_->setAuthenticator(std::move(auth));
  }
  void setAuditHook(std::shared_ptr<IAuditHook> audit) override {
    inner_->setAuditHook(std::move(audit));
  }
  bool supportsStreaming() const override {
    return inner_->supportsStreaming();
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
  RunningServer(ServerOptions options,
                StreamableHttpServerOptions http_options = {}) {
    http_options.bind = "127.0.0.1";
    http_options.port = 0;
    auto created = makeStreamableHttpServerTransport(http_options);
    REQUIRE(created.hasValue());
    concrete_ = std::dynamic_pointer_cast<mcp::v1::HttpServerTransport>(
        created.value());
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
  std::shared_ptr<mcp::v1::HttpServerTransport> concrete_;
  std::shared_ptr<ReadyTransport> transport_;
  std::unique_ptr<McpServer> server_;
  std::thread thread_;
};

class EchoTool final : public IToolHandler {
public:
  Result<ToolOutcome> invoke(const Json&, RequestContext&,
                             mcp::v1::IProgressSink&) override {
    ToolResult result;
    result.content.push_back(ContentBlock{ContentType::Text, "ok"});
    return ToolOutcome(std::move(result));
  }
};

void add_tool(McpServer& server, const std::string& name) {
  ToolDescriptor descriptor;
  descriptor.name = name;
  descriptor.inputSchema = Json{{"type", "object"}};
  server.tools().add(std::move(descriptor), std::make_shared<EchoTool>());
}

class GoodAuthenticator final : public IAuthenticator {
public:
  Result<AuthContext> authenticate(const Headers& headers) override {
    const auto it = headers.find("authorization");
    if (it == headers.end() || it->second != "Bearer good") {
      return Result<AuthContext>::failure(mcp::v1::McpError::make(
          mcp::v1::ErrorCode::InternalError, "unauthorized"));
    }
    AuthContext auth;
    auth.authenticated = true;
    auth.principal = "tester";
    return auth;
  }
};

class DenyAuthorizer final : public IAuthorizer {
public:
  Status authorize(const AuthContext&, std::string_view name) override {
    if (name == "denied" || name == "test://denied" ||
        name == "denied-prompt") {
      return Status::failure(mcp::v1::McpError::make(
          mcp::v1::ErrorCode::InternalError, "forbidden"));
    }
    return Status::success();
  }
};

class FakeApproval final : public IApprovalHook {
public:
  enum class Mode { Approve, Deny, Timeout };
  explicit FakeApproval(Mode mode) : mode_(mode) {}

  std::future<mcp::v1::ApprovalResult> approve(
      const mcp::v1::ApprovalRequest&) override {
    if (mode_ == Mode::Timeout) {
      return promise_.get_future();
    }
    std::promise<mcp::v1::ApprovalResult> promise;
    mcp::v1::ApprovalResult result;
    result.approved = mode_ == Mode::Approve;
    result.reason = result.approved ? "ok" : "denied";
    promise.set_value(result);
    return promise.get_future();
  }

private:
  Mode mode_;
  std::promise<mcp::v1::ApprovalResult> promise_;
};

class RecordingAudit final : public IAuditHook {
public:
  explicit RecordingAudit(bool should_throw = false)
      : should_throw_(should_throw) {}

  void onEvent(const AuthContext&, std::string_view event,
               const Json&) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events_.push_back(std::string(event));
    }
    if (should_throw_) {
      throw std::runtime_error("audit failure");
    }
  }

  bool has(const std::string& event) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& value : events_) {
      if (value == event) {
        return true;
      }
    }
    return false;
  }

private:
  bool should_throw_;
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

class SimpleResources final : public IResourceProvider {
public:
  Result<Page<ResourceDescriptor>> list(const mcp::v1::ListParams&,
                                        RequestContext&) override {
    return Page<ResourceDescriptor>{};
  }
  Result<Page<ResourceTemplate>> templates(const mcp::v1::ListParams&,
                                           RequestContext&) override {
    return Page<ResourceTemplate>{};
  }
  Result<ResourceOutcome> read(std::string_view uri,
                               RequestContext&) override {
    ResourceContent content;
    content.uri = std::string(uri);
    content.mimeType = "text/plain";
    content.text = "resource";
    return ResourceOutcome(std::move(content));
  }
};

class SimplePrompts final : public IPromptProvider {
public:
  Result<Page<PromptDescriptor>> list(const mcp::v1::ListParams&,
                                      RequestContext&) override {
    return Page<PromptDescriptor>{};
  }
  Result<PromptOutcome> get(std::string_view name, const Json&,
                            RequestContext&) override {
    PromptResult prompt;
    prompt.messages = Json::array(
        {Json{{"role", "user"},
              {"content", Json{{"type", "text"},
                               {"text", std::string(name)}}}}});
    return PromptOutcome(std::move(prompt));
  }
};

Json meta() {
  return Json{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
              {"io.modelcontextprotocol/clientCapabilities",
               Json::object()}};
}

httplib::Headers headers(const char* method = nullptr, bool auth = true,
                       const char* name = nullptr) {
  httplib::Headers value;
  value.emplace("Accept", "application/json");
  value.emplace("MCP-Protocol-Version", "2026-07-28");
  if (method != nullptr) {
    value.emplace("Mcp-Method", method);
  }
  if (name != nullptr) {
    value.emplace("Mcp-Name", name);
  }
  if (auth) {
    value.emplace("Authorization", "Bearer good");
  }
  return value;
}

Json tool_body(const std::string& name, int id = 1) {
  Json params;
  params["name"] = name;
  params["arguments"] = Json::object();
  params["_meta"] = meta();
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", std::move(params)}};
}

} // namespace

TEST_CASE("authentication failure is 401 and success reaches the tool") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  options.auth = std::make_shared<GoodAuthenticator>();
  RunningServer running(options);
  add_tool(running.server(), "echo");
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  auto denied = client.Post("/mcp", headers("tools/call", false, "echo"),
                            tool_body("echo").dump(), "application/json");
  REQUIRE(denied);
  CHECK(denied->status == 401);
  CHECK(Json::parse(denied->body).at("id") == 1);
  CHECK(Json::parse(denied->body).at("error").at("data").at("reason") ==
        "unauthorized");

  auto allowed = client.Post("/mcp", headers("tools/call", true, "echo"),
                             tool_body("echo", 2).dump(),
                             "application/json");
  REQUIRE(allowed);
  CHECK(allowed->status == 200);
  CHECK(Json::parse(allowed->body).at("result").at("content").at(0).at(
            "text") == "ok");

  client.stop();
  running.stop();
}

TEST_CASE("authorizer denial maps to 403 for tools resources and prompts") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  options.authorizer = std::make_shared<DenyAuthorizer>();
  options.resources = std::make_shared<SimpleResources>();
  options.prompts = std::make_shared<SimplePrompts>();
  RunningServer running(options);
  add_tool(running.server(), "denied");
  add_tool(running.server(), "allowed");
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  auto tool = client.Post("/mcp", headers("tools/call", true, "denied"),
                          tool_body("denied").dump(), "application/json");
  REQUIRE(tool);
  CHECK(tool->status == 403);
  CHECK(Json::parse(tool->body).at("error").at("data").at("reason") ==
        "forbidden");
  auto allowed = client.Post("/mcp", headers("tools/call", true, "allowed"),
                             tool_body("allowed", 2).dump(),
                             "application/json");
  REQUIRE(allowed);
  CHECK(allowed->status == 200);

  Json read_params;
  read_params["uri"] = "test://denied";
  read_params["_meta"] = meta();
  const Json read_body = Json{{"jsonrpc", "2.0"},
                              {"id", 3},
                              {"method", "resources/read"},
                              {"params", read_params}};
  auto resource = client.Post("/mcp", headers("resources/read"),
                              read_body.dump(), "application/json");
  REQUIRE(resource);
  CHECK(resource->status == 403);

  Json prompt_params;
  prompt_params["name"] = "denied-prompt";
  prompt_params["_meta"] = meta();
  const Json prompt_body = Json{{"jsonrpc", "2.0"},
                                {"id", 4},
                                {"method", "prompts/get"},
                                {"params", prompt_params}};
  auto prompt = client.Post("/mcp", headers("prompts/get"),
                            prompt_body.dump(), "application/json");
  REQUIRE(prompt);
  CHECK(prompt->status == 403);

  client.stop();
  running.stop();
}

TEST_CASE("approval denial and timeout map to 403") {
  for (const auto mode : {FakeApproval::Mode::Deny,
                          FakeApproval::Mode::Timeout}) {
    ServerOptions options;
    options.allowUnvalidatedTools = true;
    options.approval = std::make_shared<FakeApproval>(mode);
    options.approvalTimeout = std::chrono::milliseconds(100);
    RunningServer running(options);
    add_tool(running.server(), "echo");
    running.start();

    httplib::Client client(baseUrlFor(running.port()));
    auto response = client.Post("/mcp", headers("tools/call", true, "echo"),
                                tool_body("echo").dump(),
                                "application/json");
    REQUIRE(response);
    CHECK(response->status == 403);
    const std::string expected =
        mode == FakeApproval::Mode::Deny ? "approval_denied"
                                         : "approval_timeout";
    CHECK(Json::parse(response->body).at("error").at("data").at("reason") ==
          expected);

    client.stop();
    running.stop();
  }
}

TEST_CASE("approved call proceeds and audit receives events") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  options.approval =
      std::make_shared<FakeApproval>(FakeApproval::Mode::Approve);
  auto audit = std::make_shared<RecordingAudit>();
  options.audit = audit;
  RunningServer running(options);
  add_tool(running.server(), "echo");
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  auto response = client.Post("/mcp", headers("tools/call", true, "echo"),
                              tool_body("echo").dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(audit->has("tool_call"));

  client.stop();
  running.stop();
}

TEST_CASE("throwing audit hook reports audit_failed and does not break the call") {
  ServerOptions options;
  options.allowUnvalidatedTools = true;
  options.audit = std::make_shared<RecordingAudit>(true);
  RunningServer running(options);
  add_tool(running.server(), "echo");
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  auto response = client.Post("/mcp", headers("tools/call", true, "echo"),
                              tool_body("echo").dump(),
                              "application/json");
  REQUIRE(response);
  CHECK(response->status == 200);
  const auto error = running.server().lastError();
  REQUIRE(error.has_value());
  CHECK(error->data.at("reason") == "audit_failed");

  client.stop();
  running.stop();
}
TEST_CASE("2025 authentication failure and success") {
  ServerOptions options;
  auto audit = std::make_shared<RecordingAudit>();
  options.auth = std::make_shared<GoodAuthenticator>();
  options.audit = audit;
  RunningServer running(options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers missing;
  missing.emplace("Accept", "application/json");
  missing.emplace("MCP-Protocol-Version", "2025-11-25");
  const Json init_body =
      Json{{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "initialize"},
           {"params", Json{{"protocolVersion", "2025-11-25"}}}};
  auto denied = client.Post("/mcp", missing, init_body.dump(),
                            "application/json");
  REQUIRE(denied);
  CHECK(denied->status == 401);
  CHECK(Json::parse(denied->body).at("error").at("data").at("reason") ==
        "unauthorized");
  CHECK(audit->has("auth_failed"));

  httplib::Headers allowed;
  allowed.emplace("Accept", "application/json");
  allowed.emplace("MCP-Protocol-Version", "2025-11-25");
  allowed.emplace("Authorization", "Bearer good");
  auto initialized = client.Post("/mcp", allowed, init_body.dump(),
                                 "application/json");
  REQUIRE(initialized);
  REQUIRE(initialized->status == 200);
  const std::string session_id =
      initialized->get_header_value("MCP-Session-Id");
  REQUIRE_FALSE(session_id.empty());

  httplib::Headers list_headers = allowed;
  list_headers.emplace("MCP-Session-Id", session_id);
  const Json list_body = Json{{"jsonrpc", "2.0"},
                              {"id", 2},
                              {"method", "tools/list"},
                              {"params", Json::object()}};
  auto listed = client.Post("/mcp", list_headers, list_body.dump(),
                            "application/json");
  REQUIRE(listed);
  CHECK(listed->status == 200);

  client.stop();
  running.stop();
}
TEST_CASE("2025 resource subscribe authorization allow and deny") {
  ServerOptions options;
  options.authorizer = std::make_shared<DenyAuthorizer>();
  RunningServer running(options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers init_headers;
  init_headers.emplace("Accept", "application/json");
  init_headers.emplace("MCP-Protocol-Version", "2025-11-25");
  const Json init_body =
      Json{{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "initialize"},
           {"params", Json{{"protocolVersion", "2025-11-25"}}}};
  auto initialized = client.Post("/mcp", init_headers, init_body.dump(),
                                 "application/json");
  REQUIRE(initialized);
  REQUIRE(initialized->status == 200);
  const std::string session_id =
      initialized->get_header_value("MCP-Session-Id");
  REQUIRE_FALSE(session_id.empty());

  httplib::Headers headers;
  headers.emplace("Accept", "application/json");
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", session_id);

  const Json denied_body =
      Json{{"jsonrpc", "2.0"},
           {"id", 2},
           {"method", "resources/subscribe"},
           {"params", Json{{"uri", "test://denied"}}}};
  auto denied = client.Post("/mcp", headers, denied_body.dump(),
                            "application/json");
  REQUIRE(denied);
  CHECK(denied->status == 403);
  CHECK(Json::parse(denied->body).at("error").at("data").at("reason") ==
        "forbidden");

  const Json allowed_body =
      Json{{"jsonrpc", "2.0"},
           {"id", 3},
           {"method", "resources/subscribe"},
           {"params", Json{{"uri", "test://allowed"}}}};
  auto allowed = client.Post("/mcp", headers, allowed_body.dump(),
                             "application/json");
  REQUIRE(allowed);
  CHECK(allowed->status == 200);

  client.stop();
  running.stop();
}

TEST_CASE("GET and DELETE authentication failures return 401") {
  ServerOptions options;
  options.auth = std::make_shared<GoodAuthenticator>();
  RunningServer running(options);
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  auto get = client.Get("/mcp", headers);
  REQUIRE(get);
  CHECK(get->status == 401);

  auto remove = client.Delete("/mcp", headers);
  REQUIRE(remove);
  CHECK(remove->status == 401);

  client.stop();
  running.stop();
}
