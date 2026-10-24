#include "tugmcp/version.hpp"

#include <cstdio>

namespace mcp {
namespace v1 {
namespace {

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
  switch (month) {
  case 1:
  case 3:
  case 5:
  case 7:
  case 8:
  case 10:
  case 12:
    return 31;
  case 4:
  case 6:
  case 9:
  case 11:
    return 30;
  case 2:
    return isLeapYear(year) ? 29 : 28;
  default:
    return 0;
  }
}

bool isDigit(char value) { return value >= '0' && value <= '9'; }

int parseTwoDigits(std::string_view text, std::size_t offset) {
  return (text[offset] - '0') * 10 + (text[offset + 1] - '0');
}

} // namespace

std::string_view toWire(Era era) noexcept {
  switch (era) {
  case Era::V2025:
    return kWire2025;
  case Era::V2026:
    return kWire2026;
  case Era::Legacy2024:
    return kWireLegacy2024;
  }
  return kWire2026;
}

std::optional<Era> eraFromWire(std::string_view wire) noexcept {
  if (wire == kWire2025) {
    return Era::V2025;
  }
  if (wire == kWire2026) {
    return Era::V2026;
  }
  if (wire == kWireLegacy2024) {
    return Era::Legacy2024;
  }
  return std::nullopt;
}

std::string ProtocolVersion::toString() const {
  char buffer[32];
  const int written =
      std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  if (written <= 0) {
    return {};
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

std::optional<ProtocolVersion> ProtocolVersion::parse(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index == 4 || index == 7) {
      continue;
    }
    if (!isDigit(text[index])) {
      return std::nullopt;
    }
  }

  ProtocolVersion version;
  version.year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                 (text[2] - '0') * 10 + (text[3] - '0');
  version.month = parseTwoDigits(text, 5);
  version.day = parseTwoDigits(text, 8);

  if (version.month < 1 || version.month > 12) {
    return std::nullopt;
  }
  if (version.day < 1 || version.day > daysInMonth(version.year, version.month)) {
    return std::nullopt;
  }

  return version;
}

bool ProtocolVersion::operator==(const ProtocolVersion& other) const {
  return year == other.year && month == other.month && day == other.day;
}

bool ProtocolVersion::operator!=(const ProtocolVersion& other) const {
  return !(*this == other);
}

} // namespace v1
} // namespace mcp
