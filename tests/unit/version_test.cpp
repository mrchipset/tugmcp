#include <doctest/doctest.h>

#include <tugmcp/version.hpp>

#include <optional>
#include <string>

using mcp::v1::ProtocolEra;
using mcp::v1::ProtocolVersion;
using mcp::v1::eraFromWire;
using mcp::v1::toWire;

TEST_CASE("wire version strings map to eras and back") {
  CHECK(toWire(ProtocolEra::V2025) == "2025-11-25");
  CHECK(toWire(ProtocolEra::V2026) == "2026-07-28");
  CHECK(toWire(ProtocolEra::Legacy2024) == "2024-11-05");

  CHECK(eraFromWire("2025-11-25") == std::optional<ProtocolEra>(ProtocolEra::V2025));
  CHECK(eraFromWire("2026-07-28") == std::optional<ProtocolEra>(ProtocolEra::V2026));
  CHECK(eraFromWire("2024-11-05") == std::optional<ProtocolEra>(ProtocolEra::Legacy2024));
}

TEST_CASE("unknown wire versions yield nullopt") {
  CHECK_FALSE(eraFromWire("").has_value());
  CHECK_FALSE(eraFromWire("2025-06-18").has_value());
  CHECK_FALSE(eraFromWire("2025-11-25 ").has_value());
}

TEST_CASE("ProtocolVersion parses canonical dates") {
  const auto parsed = ProtocolVersion::parse("2025-11-25");
  REQUIRE(parsed.has_value());
  CHECK(parsed->year == 2025);
  CHECK(parsed->month == 11);
  CHECK(parsed->day == 25);
  CHECK(parsed->toString() == "2025-11-25");
}

TEST_CASE("ProtocolVersion rejects malformed or impossible dates") {
  CHECK_FALSE(ProtocolVersion::parse("").has_value());
  CHECK_FALSE(ProtocolVersion::parse("2025-1-25").has_value());
  CHECK_FALSE(ProtocolVersion::parse("2025-13-01").has_value());
  CHECK_FALSE(ProtocolVersion::parse("2025-00-10").has_value());
  CHECK_FALSE(ProtocolVersion::parse("2025-02-30").has_value());
  CHECK_FALSE(ProtocolVersion::parse("2023-02-29").has_value());  // not a leap year
  CHECK(ProtocolVersion::parse("2024-02-29").has_value());        // leap year
  CHECK_FALSE(ProtocolVersion::parse("2025-11-25x").has_value());
}

TEST_CASE("ProtocolVersion equality is structural") {
  const auto a = ProtocolVersion::parse("2026-07-28");
  const auto b = ProtocolVersion::parse("2026-07-28");
  const auto c = ProtocolVersion::parse("2025-11-25");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(c.has_value());
  CHECK(*a == *b);
  CHECK_FALSE(*a != *b);
  CHECK(*a != *c);
  CHECK_FALSE(*a == *c);
}
