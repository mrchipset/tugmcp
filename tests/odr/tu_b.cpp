#include <doctest/doctest.h>
#include <tugmcp/core.hpp>

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
}
