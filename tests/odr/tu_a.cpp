#include <doctest/doctest.h>

#include <tugmcp/canonical.hpp>
#include <tugmcp/client.hpp>
#include <tugmcp/core.hpp>
#include <tugmcp/operation.hpp>
#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/request_context.hpp>
#include <tugmcp/schema.hpp>
#include <tugmcp/server.hpp>
#include <tugmcp/time_source.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport.hpp>
#include <tugmcp/transport/http.hpp>
#include <tugmcp/transport/stdio.hpp>

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

  auto canonical = canonicalize(Json{{"b", 1}, {"a", 2}});
  REQUIRE(canonical.hasValue());
  CHECK(canonical.value() == R"({"a":2,"b":1})");
}
