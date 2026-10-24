#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>

using mcp::v1::decode;
using mcp::v1::encode;
using mcp::v1::ErrorCode;
using mcp::v1::Json;
using mcp::v1::Message;
using mcp::v1::MessageKind;
using mcp::v1::parseStrict;

namespace {

Message decodeOk(std::string_view raw) {
  auto result = decode(raw);
  REQUIRE(result.hasValue());
  return result.value();
}

std::string encodeOk(const Message& message) {
  auto result = encode(message);
  REQUIRE(result.hasValue());
  return result.value();
}

void expectDecodeError(std::string_view raw, ErrorCode expected) {
  auto result = decode(raw);
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) == static_cast<int>(expected));
}

Json reparse(const std::string& text) { return Json::parse(text); }

std::string utf8Payload(const std::string& bytes) {
  return std::string(
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"a\",\"params\":\"") +
         bytes + "\"}";
}

} // namespace

TEST_CASE("decode and encode round-trip a request") {
  const std::string raw =
      R"({"jsonrpc":"2.0","id":"req-1","method":"tools/call","params":{"name":"echo"}})";
  const Message message = decodeOk(raw);
  CHECK(message.kind == MessageKind::Request);
  CHECK(std::get<std::string>(message.id.value) == "req-1");
  CHECK(message.method == "tools/call");
  CHECK(message.params == Json{{"name", "echo"}});
  CHECK(reparse(encodeOk(message)) == reparse(raw));
}

TEST_CASE("decode and encode round-trip a notification") {
  const std::string raw =
      R"({"jsonrpc":"2.0","method":"notifications/initialized","params":[1,2]})";
  const Message message = decodeOk(raw);
  CHECK(message.kind == MessageKind::Notification);
  CHECK(message.method == "notifications/initialized");
  CHECK(message.params == Json::array({1, 2}));
  CHECK(reparse(encodeOk(message)) == reparse(raw));
}

TEST_CASE("decode and encode round-trip a result response") {
  const std::string raw =
      R"({"jsonrpc":"2.0","id":7,"result":{"content":[{"type":"text","text":"hi"}]}})";
  const Message message = decodeOk(raw);
  CHECK(message.kind == MessageKind::Response);
  REQUIRE(std::holds_alternative<std::int64_t>(message.id.value));
  CHECK(std::get<std::int64_t>(message.id.value) == 7);
  CHECK_FALSE(message.error.has_value());
  CHECK(reparse(encodeOk(message)) == reparse(raw));
}

TEST_CASE("decode and encode round-trip an error response") {
  const std::string raw =
      R"({"jsonrpc":"2.0","id":"e1","error":{"code":-32601,"message":"missing","data":{"method":"x"}}})";
  const Message message = decodeOk(raw);
  CHECK(message.kind == MessageKind::Response);
  REQUIRE(message.error.has_value());
  CHECK(static_cast<int>(message.error->code) ==
        static_cast<int>(ErrorCode::MethodNotFound));
  CHECK(message.error->message == "missing");
  CHECK(message.error->data == Json{{"method", "x"}});
  CHECK(reparse(encodeOk(message)) == reparse(raw));
}

TEST_CASE("batch arrays parse as JSON but decode rejects them") {
  const std::string raw = R"([{"jsonrpc":"2.0","id":1,"method":"a"}])";
  auto parsed = parseStrict(raw);
  REQUIRE(parsed.hasValue());
  CHECK(parsed.value().is_array());

  auto result = decode(raw);
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InvalidRequest));
  CHECK(result.error().message == "batch is not supported");
}

TEST_CASE("duplicate keys are rejected with ParseError") {
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"method":"a","method":"b"})",
                    ErrorCode::ParseError);
}

TEST_CASE("escaped duplicate keys are rejected") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":{"a":1,"\u0061":2}})",
      ErrorCode::ParseError);
}

TEST_CASE("duplicate keys inside nested arrays are rejected") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":[{"x":1,"x":2}]})",
      ErrorCode::ParseError);
}

TEST_CASE("duplicate keys in separate array elements are allowed") {
  auto parsed = parseStrict(R"([{"a":1},{"a":2}])");
  REQUIRE(parsed.hasValue());
  REQUIRE(parsed.value().is_array());
  CHECK(parsed.value().size() == 2);
}

TEST_CASE("overlong UTF-8 encodings are rejected") {
  expectDecodeError(utf8Payload("\xC0\xAF"), ErrorCode::ParseError);
  expectDecodeError(utf8Payload("\xE0\x80\xAF"), ErrorCode::ParseError);
  expectDecodeError(utf8Payload("\xF0\x80\x80\xAF"), ErrorCode::ParseError);
}

TEST_CASE("lone surrogates and out-of-range code points are rejected") {
  expectDecodeError(utf8Payload("\xED\xA0\x80"), ErrorCode::ParseError);
  expectDecodeError(utf8Payload("\xF4\x90\x80\x80"), ErrorCode::ParseError);
}

TEST_CASE("truncated UTF-8 sequences are rejected") {
  expectDecodeError(utf8Payload("\xE4\xB8"), ErrorCode::ParseError);
  expectDecodeError(utf8Payload("\xC3\x28"), ErrorCode::ParseError);
}

TEST_CASE("NaN and Infinity literals are rejected with ParseError") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":NaN})",
      ErrorCode::ParseError);
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":Infinity})",
      ErrorCode::ParseError);
}

TEST_CASE("overflowing floating point literals are rejected") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":1e999})",
      ErrorCode::ParseError);
}

TEST_CASE("200 nesting levels are accepted") {
  constexpr int kDepth = 200;
  std::string deep(kDepth, '[');
  deep += '0';
  deep.append(kDepth, ']');

  auto parsed = parseStrict(deep);
  REQUIRE(parsed.hasValue());

  // Walk down to the innermost value: the top-level value is the array itself.
  const Json* node = &parsed.value();
  for (int i = 0; i < kDepth; ++i) {
    REQUIRE(node->is_array());
    REQUIRE(node->size() == 1);
    node = &node->at(0);
  }
  CHECK(node->is_number_integer());
  CHECK(*node == 0);
}

TEST_CASE("non-object JSON is rejected by decode but accepted by parseStrict") {
  auto parsed = parseStrict("123");
  REQUIRE(parsed.hasValue());
  CHECK(parsed.value() == 123);
  expectDecodeError("123", ErrorCode::InvalidRequest);
}

TEST_CASE("non-integer and non-string ids are rejected") {
  expectDecodeError(R"({"jsonrpc":"2.0","id":1.5,"method":"a"})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":true,"method":"a"})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":null,"method":"a"})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":[1],"method":"a"})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("integer id boundaries are preserved") {
  const Message max_int =
      decodeOk(R"({"jsonrpc":"2.0","id":9223372036854775807,"method":"a"})");
  REQUIRE(std::holds_alternative<std::int64_t>(max_int.id.value));
  CHECK(std::get<std::int64_t>(max_int.id.value) ==
        INT64_C(9223372036854775807));

  const Message min_int =
      decodeOk(R"({"jsonrpc":"2.0","id":-9223372036854775808,"method":"a"})");
  REQUIRE(std::holds_alternative<std::int64_t>(min_int.id.value));
  CHECK(std::get<std::int64_t>(min_int.id.value) ==
        INT64_C(-9223372036854775807) - 1);

  const Message max_uint =
      decodeOk(R"({"jsonrpc":"2.0","id":18446744073709551615,"method":"a"})");
  REQUIRE(std::holds_alternative<std::uint64_t>(max_uint.id.value));
  CHECK(std::get<std::uint64_t>(max_uint.id.value) ==
        UINT64_C(18446744073709551615));
}

TEST_CASE("jsonrpc field is required and must be 2.0") {
  expectDecodeError(R"({"id":1,"method":"a"})", ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"1.0","id":1,"method":"a"})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":2.0,"id":1,"method":"a"})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("messages need either method or id") {
  expectDecodeError(R"({"jsonrpc":"2.0"})", ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","method":""})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("requests must not contain result or error") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","result":{}})",
      ErrorCode::InvalidRequest);
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"method":"a","error":{"code":-32603,"message":"x"}})",
      ErrorCode::InvalidRequest);
}

TEST_CASE("responses must not contain params") {
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"result":{},"params":{}})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("responses must contain exactly one of result or error") {
  expectDecodeError(
      R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":-32603,"message":"x"}})",
      ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":1})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("malformed and empty input are rejected with ParseError") {
  expectDecodeError(R"({"a":)", ErrorCode::ParseError);
  expectDecodeError("", ErrorCode::ParseError);
}

TEST_CASE("params must be an object, array or null") {
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"method":"a","params":"text"})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"method":"a","params":5})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("null params is accepted") {
  const Message message =
      decodeOk(R"({"jsonrpc":"2.0","id":1,"method":"a","params":null})");
  CHECK(message.kind == MessageKind::Request);
  CHECK(message.params.is_null());
}

TEST_CASE("response errors require integer code and string message") {
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":"m"}})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32603}})",
                    ErrorCode::InvalidRequest);
  expectDecodeError(R"({"jsonrpc":"2.0","id":1,"error":"boom"})",
                    ErrorCode::InvalidRequest);
}

TEST_CASE("excessively deep nesting is rejected instead of crashing") {
  std::string raw = R"({"jsonrpc":"2.0","id":1,"method":"a","params":)";
  for (int i = 0; i < 300; ++i) {
    raw += '[';
  }
  for (int i = 0; i < 300; ++i) {
    raw += ']';
  }
  raw += '}';
  expectDecodeError(raw, ErrorCode::ParseError);
}

TEST_CASE("encode rejects non-finite numbers with InternalError") {
  Message message;
  message.kind = MessageKind::Notification;
  message.method = "notify";
  message.params = Json::object();
  message.params["value"] = std::numeric_limits<double>::quiet_NaN();

  auto nan_result = encode(message);
  REQUIRE_FALSE(nan_result.hasValue());
  CHECK(static_cast<int>(nan_result.error().code) ==
        static_cast<int>(ErrorCode::InternalError));

  message.params["value"] = std::numeric_limits<double>::infinity();
  auto inf_result = encode(message);
  REQUIRE_FALSE(inf_result.hasValue());
  CHECK(static_cast<int>(inf_result.error().code) ==
        static_cast<int>(ErrorCode::InternalError));
}

TEST_CASE("encode rejects an empty method with InvalidRequest") {
  Message message;
  message.kind = MessageKind::Request;
  message.method.clear();

  auto result = encode(message);
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InvalidRequest));
}

TEST_CASE("encode rejects a response with both result and error") {
  Message message;
  message.kind = MessageKind::Response;
  message.id.value = std::string("r1");
  message.result = Json::object();
  message.error = mcp::v1::McpError::make(ErrorCode::InternalError, "boom");

  auto result = encode(message);
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InvalidRequest));
}

TEST_CASE("encode rejects invalid UTF-8 strings") {
  Message message;
  message.kind = MessageKind::Notification;
  message.method = "\xC3\x28";

  auto result = encode(message);
  REQUIRE_FALSE(result.hasValue());
  CHECK(static_cast<int>(result.error().code) ==
        static_cast<int>(ErrorCode::InternalError));
}

TEST_CASE("encode output is compact and contains no newline") {
  const Message message = decodeOk(
      R"({"jsonrpc":"2.0","id":1,"method":"a","params":{"x":1}})");
  const std::string text = encodeOk(message);
  CHECK(text.find('\n') == std::string::npos);
  CHECK(text.find(' ') == std::string::npos);
}

TEST_CASE("decode(Json) matches decode(string)") {
  const std::string request_raw =
      R"({"jsonrpc":"2.0","id":"req-9","method":"tools/call","params":{"name":"echo"}})";
  auto request_text = decode(request_raw);
  auto request_json_doc = parseStrict(request_raw);
  REQUIRE(request_text.hasValue());
  REQUIRE(request_json_doc.hasValue());

  auto request_json = decode(request_json_doc.value());
  REQUIRE(request_json.hasValue());
  CHECK(request_text.value().kind == request_json.value().kind);
  CHECK(request_text.value().id == request_json.value().id);
  CHECK(request_text.value().method == request_json.value().method);
  CHECK(request_text.value().params == request_json.value().params);

  const std::string error_raw =
      R"({"jsonrpc":"2.0","id":12,"error":{"code":-32601,"message":"missing","data":{"x":1}}})";
  auto error_text = decode(error_raw);
  auto error_json_doc = parseStrict(error_raw);
  REQUIRE(error_text.hasValue());
  REQUIRE(error_json_doc.hasValue());

  auto error_json = decode(error_json_doc.value());
  REQUIRE(error_json.hasValue());
  CHECK(error_text.value().kind == error_json.value().kind);
  CHECK(error_text.value().id == error_json.value().id);
  REQUIRE(error_text.value().error.has_value());
  REQUIRE(error_json.value().error.has_value());
  CHECK(static_cast<int>(error_text.value().error->code) ==
        static_cast<int>(error_json.value().error->code));
  CHECK(error_text.value().error->message ==
        error_json.value().error->message);
  CHECK(error_text.value().error->data == error_json.value().error->data);
}