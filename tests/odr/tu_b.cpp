#include <doctest/doctest.h>

#include <tugmcp/canonical.hpp>
#include <tugmcp/client.hpp>
#include <tugmcp/core.hpp>
#include <tugmcp/request_context.hpp>
#include <tugmcp/schema.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport.hpp>
#include <tugmcp/transport/http.hpp>
#include <tugmcp/transport/stdio.hpp>

TEST_CASE("odr tu b round-trips a string request id") {
  using namespace mcp::v1;

  auto id = RequestId::fromJson(Json("odr-id"));
  REQUIRE(id.hasValue());

  Message message;
  message.kind = MessageKind::Notification;
  message.method = "notifications/initialized";

  auto encoded = encode(message);
  REQUIRE(encoded.hasValue());

  auto decoded = decode(encoded.value());
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().method == message.method);

  CHECK(toWire(Era::V2026) == kWire2026);
  CHECK(makeSystemTimeSource() != nullptr);
}
