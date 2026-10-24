#include <tugmcp/operation.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace mcp {
namespace v1 {

std::string_view toString(ResultType type) noexcept {
  return type == ResultType::InputRequired ? "input_required" : "complete";
}

Result<ProgressToken> progressTokenFromJson(const Json& json) {
  if (json.is_string()) {
    return ProgressToken{json.get<std::string>()};
  }

  if (json.is_number_unsigned()) {
    const std::uint64_t value = json.get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
      return Result<ProgressToken>::failure(McpError::make(
          ErrorCode::InvalidParams, "invalid progressToken"));
    }
    return ProgressToken{static_cast<std::int64_t>(value)};
  }

  if (json.is_number_integer()) {
    return ProgressToken{json.get<std::int64_t>()};
  }

  return Result<ProgressToken>::failure(
      McpError::make(ErrorCode::InvalidParams, "invalid progressToken"));
}

Json progressTokenToJson(const ProgressToken& token) {
  if (const auto* text = std::get_if<std::string>(&token)) {
    return Json(*text);
  }
  return Json(std::get<std::int64_t>(token));
}

} // namespace v1
} // namespace mcp
