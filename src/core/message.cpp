#include "tugmcp/message.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace mcp {
namespace v1 {

Result<RequestId> RequestId::fromJson(const Json& json) {
  if (json.is_string()) {
    RequestId id;
    id.value = json.get<std::string>();
    return id;
  }

  if (json.is_number_unsigned()) {
    const auto value = json.get<std::uint64_t>();
    RequestId id;
    if (value <= static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
      id.value = static_cast<std::int64_t>(value);
    } else {
      id.value = value;
    }
    return id;
  }

  if (json.is_number_integer()) {
    RequestId id;
    id.value = json.get<std::int64_t>();
    return id;
  }

  return Result<RequestId>::failure(
      McpError::make(ErrorCode::InvalidRequest,
                     "request id must be a string or an integer"));
}

Json RequestId::toJson() const {
  return std::visit([](const auto& value) -> Json { return Json(value); }, value);
}

} // namespace v1
} // namespace mcp
