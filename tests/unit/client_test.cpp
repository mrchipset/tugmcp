#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <doctest/doctest.h>
#include <tugmcp/client.hpp>

#include "../helpers/fake_transport.hpp"

using mcp::v1::ClientOptions;
using mcp::v1::ErrorCode;
using mcp::v1::Json;
using mcp::v1::McpClient;
using mcp::v1::Result;
using mcp::v1::Status;
using mcp::v1::test::FakeTransport;

namespace {

Json resultResponse(const Json& request, Json result) {
  return Json{{"jsonrpc", "2.0"},
              {"id", request.at("id")},
              {"result", std::move(result)}};
}

Json errorResponse(const Json& request, int code, std::string message) {
  return Json{{"jsonrpc", "2.0"},
              {"id", request.at("id")},
              {"error",
               Json{{"code", code}, {"message", std::move(message)}}}};
}

std::string methodOf(const Json& request) {
  return request.value("method", std::string{});
}

} // namespace

TEST_CASE("connect sends initialize and notifications/initialized") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    if (methodOf(request) == "initialize") {
      transport->deliver(resultResponse(
          request, Json{{"protocolVersion", "2025-11-25"}}));
    }
  });

  ClientOptions options;
  options.name = "test-client";
  options.version = "1.2.3";
  options.timeoutMs = 1000;
  options.protocolVersion = "2025-11-25";
  McpClient client(options, transport);

  const Status status = client.connect();
  REQUIRE(status.ok);

  const auto sent = transport->sent();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].at("method") == "initialize");
  CHECK(sent[0].at("params").at("protocolVersion") == "2025-11-25");
  CHECK(sent[0].at("params").at("capabilities").is_object());
  CHECK(sent[0].at("params").at("clientInfo").at("name") == "test-client");
  CHECK(sent[0].at("params").at("clientInfo").at("version") == "1.2.3");

  CHECK(sent[1].at("method") == "notifications/initialized");
  CHECK_FALSE(sent[1].contains("id"));
}

TEST_CASE("connect fails when initialize returns an error") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    if (methodOf(request) == "initialize") {
      transport->deliver(errorResponse(request, -32603, "init failed"));
    }
  });

  ClientOptions options;
  options.timeoutMs = 1000;
  options.protocolVersion = "2025-11-25";
  McpClient client(options, transport);

  const Status status = client.connect();
  REQUIRE_FALSE(status.ok);
  CHECK(static_cast<int>(status.error.code) ==
        static_cast<int>(ErrorCode::InternalError));
  CHECK(status.error.message == "init failed");
}

TEST_CASE("call returns a successful result") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    if (methodOf(request) == "ping") {
      transport->deliver(resultResponse(request, Json{{"pong", true}}));
    }
  });

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  auto result = client.call("ping");
  REQUIRE(result.hasValue());
  CHECK(result.value() == Json{{"pong", true}});

  const auto sent = transport->sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].at("method") == "ping");
  CHECK(sent[0].at("params").is_object());
}

TEST_CASE("call propagates error responses") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    transport->deliver(errorResponse(request, -32601, "no such method"));
  });

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  auto result = client.call("missing");
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::MethodNotFound));
  CHECK(result.error().message == "no such method");
}

TEST_CASE("listTools and callTool use the expected request shapes") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    const std::string method = methodOf(request);
    if (method == "tools/list") {
      Json tools = Json::array();
      tools.push_back(Json{{"name", "echo"}});
      transport->deliver(resultResponse(request, Json{{"tools", tools}}));
    } else if (method == "tools/call") {
      transport->deliver(resultResponse(
          request, Json{{"content", Json::array()},
                        {"structuredContent", Json{{"text", "hi"}}}}));
    }
  });

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  auto listed = client.listTools();
  REQUIRE(listed.hasValue());
  CHECK(listed.value().at("tools").at(0).at("name") == "echo");

  auto called = client.callTool("echo", Json{{"text", "hi"}});
  REQUIRE(called.hasValue());
  CHECK(called.value().at("structuredContent").at("text") == "hi");

  const auto sent = transport->sent();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].at("method") == "tools/list");
  CHECK(sent[0].at("params").is_object());

  CHECK(sent[1].at("method") == "tools/call");
  CHECK(sent[1].at("params").at("name") == "echo");
  CHECK(sent[1].at("params").at("arguments") == Json{{"text", "hi"}});
}

TEST_CASE("call times out with reason timeout") {
  auto transport = std::make_shared<FakeTransport>();

  ClientOptions options;
  options.timeoutMs = 1;
  McpClient client(options, transport);

  auto result = client.call("ping");
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InternalError));
  CHECK(result.error().data.at("reason") == "timeout");
}

TEST_CASE("responses with unknown ids are ignored") {
  auto transport = std::make_shared<FakeTransport>();
  transport->setAutoResponder([&transport](const Json& request) {
    const auto id = request.at("id").get<std::int64_t>();
    transport->deliver(Json{{"jsonrpc", "2.0"},
                            {"id", id + 1000},
                            {"result", Json{{"value", "unknown"}}}});
    transport->deliver(Json{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", Json{{"value", "correct"}}}});
  });

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  auto result = client.call("ping");
  REQUIRE(result.hasValue());
  CHECK(result.value().at("value") == "correct");
}

TEST_CASE("close is idempotent and subsequent calls fail as transport_closed") {
  auto transport = std::make_shared<FakeTransport>();

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  client.close();
  client.close();
  CHECK(transport->closed());

  auto result = client.call("ping");
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InternalError));
  CHECK(result.error().data.at("reason") == "transport_closed");
}

TEST_CASE("send failures map to reason send_failed") {
  auto transport = std::make_shared<FakeTransport>();
  transport->close();

  ClientOptions options;
  options.timeoutMs = 1000;
  McpClient client(options, transport);

  auto result = client.call("ping");
  REQUIRE_FALSE(result.hasValue());
  CHECK(result.error().data.at("reason") == "send_failed");
}

TEST_CASE("call fails with transport_closed when close is called in flight") {
  auto transport = std::make_shared<FakeTransport>();
  std::promise<void> sent;
  auto sent_future = sent.get_future();
  transport->setAutoResponder([&sent](const Json&) { sent.set_value(); });

  ClientOptions options;
  options.timeoutMs = 5000;
  McpClient client(options, transport);

  std::optional<Result<Json>> outcome;
  std::thread caller([&client, &outcome] { outcome = client.call("ping"); });

  sent_future.wait();
  client.close();
  caller.join();

  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  CHECK(outcome->error().data.contains("reason"));
  CHECK(outcome->error().data.at("reason") == "transport_closed");
}

TEST_CASE("call fails with transport_closed when the transport closes in flight") {
  auto transport = std::make_shared<FakeTransport>();
  std::promise<void> sent;
  auto sent_future = sent.get_future();
  transport->setAutoResponder([&sent](const Json&) { sent.set_value(); });

  ClientOptions options;
  options.timeoutMs = 5000;
  McpClient client(options, transport);

  std::optional<Result<Json>> outcome;
  std::thread caller([&client, &outcome] { outcome = client.call("ping"); });

  sent_future.wait();
  transport->emitClose();
  caller.join();

  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->hasValue());
  CHECK(outcome->error().data.contains("reason"));
  CHECK(outcome->error().data.at("reason") == "transport_closed");
}
