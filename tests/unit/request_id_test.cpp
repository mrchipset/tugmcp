#include <cstdint>
#include <functional>
#include <string>
#include <variant>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>

using mcp::v1::decode;
using mcp::v1::encode;
using mcp::v1::ErrorCode;
using mcp::v1::Json;
using mcp::v1::Message;
using mcp::v1::MessageKind;
using mcp::v1::RequestId;

TEST_CASE("RequestId normalizes non-negative integers to int64") {
  const Json raw = Json::parse("42");
  auto id = RequestId::fromJson(raw);
  REQUIRE(id.hasValue());
  REQUIRE(std::holds_alternative<std::int64_t>(id.value().value));
  CHECK(std::get<std::int64_t>(id.value().value) == 42);
  CHECK(id.value().toJson() == raw);
}

TEST_CASE("RequestId uses uint64 only above INT64_MAX") {
  const Json raw = Json::parse("9223372036854775808");
  auto id = RequestId::fromJson(raw);
  REQUIRE(id.hasValue());
  REQUIRE(std::holds_alternative<std::uint64_t>(id.value().value));
  CHECK(std::get<std::uint64_t>(id.value().value) ==
        UINT64_C(9223372036854775808));
  CHECK(id.value().toJson() == raw);
}

TEST_CASE("RequestId accepts negative and signed integers") {
  auto negative = RequestId::fromJson(Json::parse("-7"));
  REQUIRE(negative.hasValue());
  REQUIRE(std::holds_alternative<std::int64_t>(negative.value().value));
  CHECK(std::get<std::int64_t>(negative.value().value) == -7);

  auto signed_value = RequestId::fromJson(Json(11));
  REQUIRE(signed_value.hasValue());
  REQUIRE(std::holds_alternative<std::int64_t>(signed_value.value().value));
  CHECK(std::get<std::int64_t>(signed_value.value().value) == 11);
}

TEST_CASE("RequestId accepts strings") {
  const Json raw = "abc-123";
  auto id = RequestId::fromJson(raw);
  REQUIRE(id.hasValue());
  CHECK(std::holds_alternative<std::string>(id.value().value));
  CHECK(std::get<std::string>(id.value().value) == "abc-123");
  CHECK(id.value().toJson() == raw);
}

TEST_CASE("RequestId rejects floats, booleans, null, arrays and objects") {
  auto float_id = RequestId::fromJson(Json(1.5));
  CHECK_FALSE(float_id.hasValue());
  CHECK(static_cast<int>(float_id.error().code) ==
        static_cast<int>(ErrorCode::InvalidRequest));

  auto bool_id = RequestId::fromJson(Json(true));
  CHECK_FALSE(bool_id.hasValue());

  auto null_id = RequestId::fromJson(Json(nullptr));
  CHECK_FALSE(null_id.hasValue());

  auto array_id = RequestId::fromJson(Json::array({1, 2}));
  CHECK_FALSE(array_id.hasValue());

  auto object_id = RequestId::fromJson(Json::object({{"id", 1}}));
  CHECK_FALSE(object_id.hasValue());
}

TEST_CASE("RequestId equality is numeric across alternatives") {
  RequestId signed_id;
  signed_id.value = std::int64_t{7};
  RequestId unsigned_id;
  unsigned_id.value = std::uint64_t{7};

  CHECK(signed_id == unsigned_id);
  CHECK(unsigned_id == signed_id);

  RequestId text_id;
  text_id.value = std::string("7");
  CHECK_FALSE(signed_id == text_id);
  CHECK_FALSE(text_id == signed_id);
}

TEST_CASE("RequestId hash is consistent for equal values") {
  RequestId signed_id;
  signed_id.value = std::int64_t{7};
  RequestId unsigned_id;
  unsigned_id.value = std::uint64_t{7};
  REQUIRE(signed_id == unsigned_id);
  CHECK(std::hash<RequestId>{}(signed_id) ==
        std::hash<RequestId>{}(unsigned_id));

  RequestId first_text;
  first_text.value = std::string("abc");
  RequestId second_text;
  second_text.value = std::string("abc");
  REQUIRE(first_text == second_text);
  CHECK(std::hash<RequestId>{}(first_text) ==
        std::hash<RequestId>{}(second_text));
}

TEST_CASE("decode(encode(message)).id equals the original id") {
  Message message;
  message.kind = MessageKind::Request;
  message.id.value = std::uint64_t{9};
  message.method = "ping";

  auto encoded = encode(message);
  REQUIRE(encoded.hasValue());

  auto decoded = decode(encoded.value());
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().id == message.id);
}
