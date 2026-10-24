#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

// Protocol generations supported by the library.
enum class ProtocolEra { Legacy2024, V2025, V2026 };

using Era = ProtocolEra;

// Wire version strings as defined by the MCP specification.
inline constexpr std::string_view kWire2025 = "2025-11-25";
inline constexpr std::string_view kWire2026 = "2026-07-28";
inline constexpr std::string_view kWireLegacy2024 = "2024-11-05";

// Map a protocol era to its wire version string.
std::string_view toWire(Era era) noexcept;

// Parse a wire version string into an era. Unknown values return std::nullopt.
std::optional<Era> eraFromWire(std::string_view wire) noexcept;

// Structured protocol version (YYYY-MM-DD).
struct ProtocolVersion {
  int year = 0;
  int month = 0;
  int day = 0;

  std::string toString() const;
  static std::optional<ProtocolVersion> parse(std::string_view text);

  // Hand-written because C++17 has no defaulted comparison.
  bool operator==(const ProtocolVersion& other) const;
  bool operator!=(const ProtocolVersion& other) const;
};

} // namespace v1
} // namespace mcp
