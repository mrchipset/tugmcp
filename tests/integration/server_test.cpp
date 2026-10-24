#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
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
