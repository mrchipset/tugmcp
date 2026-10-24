#include <stdexcept>
#include <string>

#include <doctest/doctest.h>
#include <tugmcp/error.hpp>

using mcp::v1::ErrorCode;
using mcp::v1::errorFromJson;
using mcp::v1::errorToJson;
using mcp::v1::Json;
using mcp::v1::McpError;
using mcp::v1::MissingCapability;
using mcp::v1::Result;
using mcp::v1::Status;

TEST_CASE("ErrorCode values match the JSON-RPC and MCP ranges") {
  CHECK(static_cast<int>(ErrorCode::ParseError) == -32700);
  CHECK(static_cast<int>(ErrorCode::InvalidRequest) == -32600);
  CHECK(static_cast<int>(ErrorCode::MethodNotFound) == -32601);
  CHECK(static_cast<int>(ErrorCode::InvalidParams) == -32602);
  CHECK(static_cast<int>(ErrorCode::InternalError) == -32603);
  CHECK(static_cast<int>(ErrorCode::HeaderMismatch) == -32020);
  CHECK(static_cast<int>(ErrorCode::MissingRequiredClientCapability) == -32021);
  CHECK(static_cast<int>(ErrorCode::MissingCapability) == -32021);
  CHECK(static_cast<int>(MissingCapability) == -32021);
  CHECK(static_cast<int>(ErrorCode::UnsupportedVersion) == -32022);
  CHECK(mcp::v1::toString(ErrorCode::MissingCapability) ==
        "MissingRequiredClientCapability");
}

TEST_CASE("Result holds a value on success") {
  Result<int> result(21);
  REQUIRE(result.hasValue());
  CHECK(static_cast<bool>(result));
  CHECK(result.value() == 21);
  CHECK_THROWS_AS(result.error(), std::logic_error);
}

TEST_CASE("Result holds an error on failure") {
  auto result = Result<int>::failure(
      McpError::make(ErrorCode::InvalidParams, "bad params"));
  REQUIRE_FALSE(result.hasValue());
  CHECK_FALSE(static_cast<bool>(result));
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(result.error().message == "bad params");
  CHECK_THROWS_AS(result.value(), std::logic_error);
}

TEST_CASE("Result error data and retryability are preserved") {
  auto result = Result<int>::failure(McpError::make(
      ErrorCode::InternalError, "boom", Json{{"reason", "test"}}, true));
  REQUIRE_FALSE(result.hasValue());
  CHECK(result.error().data == Json{{"reason", "test"}});
  CHECK(result.error().retryable);
}

TEST_CASE("Status reports success and failure") {
  const Status ok = Status::success();
  CHECK(ok.ok);
  CHECK(static_cast<bool>(ok));

  const Status failed =
      Status::failure(McpError::make(ErrorCode::ParseError, "bad json"));
  CHECK_FALSE(failed.ok);
  CHECK_FALSE(static_cast<bool>(failed));
  CHECK(static_cast<int>(failed.error.code) ==
        static_cast<int>(ErrorCode::ParseError));
  CHECK(failed.error.message == "bad json");
}

TEST_CASE("McpError::make defaults data to null and retryable to false") {
  const auto error = McpError::make(ErrorCode::MethodNotFound, "missing");
  CHECK(static_cast<int>(error.code) ==
        static_cast<int>(ErrorCode::MethodNotFound));
  CHECK(error.message == "missing");
  CHECK(error.data.is_null());
  CHECK_FALSE(error.retryable);
}

TEST_CASE("errorToJson and errorFromJson round-trip") {
  const auto original = McpError::make(
      ErrorCode::MissingRequiredClientCapability, "capability missing",
      Json{{"capability", "roots"}}, true);

  const Json wire = errorToJson(original);
  CHECK(wire.at("code") == -32021);
  CHECK(wire.at("message") == "capability missing");
  CHECK(wire.at("data") == Json{{"capability", "roots"}});
  // retryable is a C++-side hint and must not leak onto the wire.
  CHECK_FALSE(wire.contains("retryable"));

  McpError parsed;
  REQUIRE(errorFromJson(wire, parsed));
  CHECK(static_cast<int>(parsed.code) ==
        static_cast<int>(ErrorCode::MissingRequiredClientCapability));
  CHECK(parsed.message == "capability missing");
  CHECK(parsed.data == Json{{"capability", "roots"}});
  CHECK_FALSE(parsed.retryable);
}

TEST_CASE("errorFromJson rejects malformed error objects") {
  McpError sink;
  CHECK_FALSE(errorFromJson(Json("boom"), sink));
  CHECK_FALSE(errorFromJson(Json{{"code", -32603}}, sink));
  CHECK_FALSE(errorFromJson(Json{{"code", "oops"}, {"message", "x"}}, sink));
  // Unknown members are tolerated (local retry policy is not a wire contract).
  CHECK(errorFromJson(
      Json{{"code", -32603}, {"message", "x"}, {"retryable", 1}}, sink));
}
