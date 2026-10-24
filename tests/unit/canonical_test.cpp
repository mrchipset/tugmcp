#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <tugmcp/canonical.hpp>
#include <tugmcp/tool.hpp>

using mcp::v1::canonicalize;
using mcp::v1::Json;
using mcp::v1::sha256Hex;
using mcp::v1::ToolDescriptor;
using mcp::v1::toolSchemaHash;

namespace {

std::string canonical(const Json& value) {
  auto result = canonicalize(value);
  CHECK(result.hasValue());
  return result.hasValue() ? result.value() : std::string{};
}

ToolDescriptor sampleTool() {
  ToolDescriptor tool;
  tool.name = "echo";
  tool.title = "Echo";
  tool.description = "Echo text";
  tool.inputSchema = Json{{"type", "object"},
                          {"properties", Json{{"text", Json{{"type", "string"}}}}}};
  tool.outputSchema = Json{{"type", "object"}};
  tool.annotations = Json{{"readOnlyHint", true}};
  return tool;
}

} // namespace

TEST_CASE("canonical sorts object keys recursively") {
  const Json value =
      Json::parse(R"({"b":{"z":1,"a":2},"a":[3,2,1],"nested":{"y":null,"x":1}})");
  CHECK(canonical(value) ==
        R"({"a":[3,2,1],"b":{"a":2,"z":1},"nested":{"x":1,"y":null}})");
}

TEST_CASE("canonical sorts top-level keys by UTF-8 byte order") {
  const Json value = Json{{"z", 1}, {"a", 2}, {"m", 3}};
  CHECK(canonical(value) == R"({"a":2,"m":3,"z":1})");
}

TEST_CASE("canonical preserves array order") {
  const Json value = Json::array({3, 1, 2, 0});
  CHECK(canonical(value) == "[3,1,2,0]");
}

TEST_CASE("canonical renders integers without formatting loss") {
  CHECK(canonical(Json(0)) == "0");
  CHECK(canonical(Json(-42)) == "-42");
  CHECK(canonical(Json(std::numeric_limits<std::int64_t>::max())) ==
        "9223372036854775807");
  CHECK(canonical(Json(std::numeric_limits<std::uint64_t>::max())) ==
        "18446744073709551615");
}

TEST_CASE("canonical renders floats with shortest round-trip form") {
  CHECK(canonical(Json(1.5)) == "1.5");
  CHECK(canonical(Json(1.0)) == "1");
  CHECK(canonical(Json(-2.25)) == "-2.25");
}

TEST_CASE("canonical normalizes negative zero") {
  CHECK(canonical(Json(-0.0)) == "0");
  CHECK(canonical(Json(0.0)) == "0");
}

TEST_CASE("canonical emits UTF-8 without escaping non-ASCII") {
  const Json value = Json{{"greeting", "\xE4\xB8\xAD\xE6\x96\x87"}};
  CHECK(canonical(value) == "{\"greeting\":\"\xE4\xB8\xAD\xE6\x96\x87\"}");
}

TEST_CASE("canonical escapes control characters and quotes") {
  const std::string text = "quote\" slash\\ newline\n tab\t ctrl\x01";
  const Json value = Json{{"k", text}};
  CHECK(canonical(value) ==
        "{\"k\":\"quote\\\" slash\\\\ newline\\n tab\\t ctrl\\u0001\"}");
}

TEST_CASE("canonical rejects NaN") {
  Json value = std::numeric_limits<double>::quiet_NaN();
  auto result = canonicalize(value);
  REQUIRE_FALSE(result.hasValue());
}

TEST_CASE("canonical rejects positive and negative infinity") {
  auto positive = canonicalize(Json(std::numeric_limits<double>::infinity()));
  auto negative = canonicalize(Json(-std::numeric_limits<double>::infinity()));
  REQUIRE_FALSE(positive.hasValue());
  REQUIRE_FALSE(negative.hasValue());
}

TEST_CASE("canonical rejects invalid UTF-8") {
  Json value = std::string("\xC3\x28", 2);
  auto result = canonicalize(value);
  REQUIRE_FALSE(result.hasValue());
}

TEST_CASE("sha256Hex matches known vectors") {
  CHECK(sha256Hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("toolSchemaHash matches a fixed external anchor") {
  ToolDescriptor tool;
  tool.name = "echo";
  tool.inputSchema = Json{{"type", "object"}};

  auto hash = toolSchemaHash(tool);
  REQUIRE(hash.hasValue());
  CHECK(hash.value() ==
        "3ae8d79785dfee98e691b99b5b65bac79e0baefbb3bad7ca5c0ec38d741f1299");
}

TEST_CASE("toolSchemaHash is stable and hex encoded") {
  const ToolDescriptor tool = sampleTool();
  auto first = toolSchemaHash(tool);
  auto second = toolSchemaHash(tool);
  REQUIRE(first.hasValue());
  REQUIRE(second.hasValue());
  CHECK(first.value() == second.value());
  CHECK(first.value().size() == 64);
  CHECK(first.value().find_first_not_of("0123456789abcdef") ==
        std::string::npos);
}

TEST_CASE("toolSchemaHash ignores description and title") {
  ToolDescriptor changed = sampleTool();
  changed.title = "Different title";
  changed.description = "Different description";
  CHECK(toolSchemaHash(changed).value() == toolSchemaHash(sampleTool()).value());
}

TEST_CASE("toolSchemaHash reacts to each hashed field") {
  const std::string baseline = toolSchemaHash(sampleTool()).value();

  ToolDescriptor renamed = sampleTool();
  renamed.name = "echo2";
  CHECK(toolSchemaHash(renamed).value() != baseline);

  ToolDescriptor inputs = sampleTool();
  inputs.inputSchema = Json{{"type", "object"}, {"required", Json::array({"x"})}};
  CHECK(toolSchemaHash(inputs).value() != baseline);

  ToolDescriptor outputs = sampleTool();
  outputs.outputSchema = Json{{"type", "string"}};
  CHECK(toolSchemaHash(outputs).value() != baseline);

  ToolDescriptor annotations = sampleTool();
  annotations.annotations = Json{{"readOnlyHint", false}};
  CHECK(toolSchemaHash(annotations).value() != baseline);
}
