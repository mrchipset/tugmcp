#include <doctest/doctest.h>
#include <tugmcp/core.hpp>

TEST_CASE("odr tu a round-trips a request id") {
  using namespace mcp::v1;

  auto id = RequestId::fromJson(Json::parse("11"));
  REQUIRE(id.hasValue());

  Message message;
  message.kind = MessageKind::Request;
  message.id = id.value();
  message.method = "ping";

  auto encoded = encode(message);
  REQUIRE(encoded.hasValue());

  auto decoded = decode(encoded.value());
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().id == message.id);
}
