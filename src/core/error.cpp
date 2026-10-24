#include "tugmcp/error.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace mcp {
namespace v1 {
namespace {

bool readErrorCode(const Json& value, int& out) {
  if (!value.is_number_integer()) {
    return false;
  }
  if (value.is_number_unsigned()) {
    const auto code = value.get<std::uint64_t>();
    if (code > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    out = static_cast<int>(code);
    return true;
  }
  const auto code = value.get<std::int64_t>();
  if (code < std::numeric_limits<int>::min() ||
      code > std::numeric_limits<int>::max()) {
    return false;
  }
  out = static_cast<int>(code);
  return true;
}

Status invalidError(const char* message) {
  return Status::failure(
      McpError::make(ErrorCode::InvalidRequest, message));
}

} // namespace

std::string toString(ErrorCode code) {
  switch (code) {
  case ErrorCode::ParseError:
    return "ParseError";
  case ErrorCode::InvalidRequest:
    return "InvalidRequest";
  case ErrorCode::MethodNotFound:
    return "MethodNotFound";
  case ErrorCode::InvalidParams:
    return "InvalidParams";
  case ErrorCode::InternalError:
    return "InternalError";
  case ErrorCode::HeaderMismatch:
    return "HeaderMismatch";
  case ErrorCode::MissingRequiredClientCapability:
    return "MissingRequiredClientCapability";
  case ErrorCode::UnsupportedVersion:
    return "UnsupportedVersion";
  }
  return "UnknownError";
}

McpError McpError::make(ErrorCode code, std::string message, Json data,
                        bool retryable) {
  McpError error;
  error.code = code;
  error.message = std::move(message);
  error.data = std::move(data);
  error.retryable = retryable;
  return error;
}

Json errorToJson(const McpError& error) {
  Json json;
  json["code"] = static_cast<int>(error.code);
  json["message"] = error.message;
  if (!error.data.is_null()) {
    json["data"] = error.data;
  }
  // NOTE: `retryable` is a C++-side hint only; JSON-RPC error objects have no
  // such member, so it must not be serialized onto the wire.
  return json;
}

Status errorFromJson(const Json& json, McpError& out) {
  if (!json.is_object()) {
    return invalidError("error must be an object");
  }
  if (!json.contains("code") || !json.contains("message")) {
    return invalidError("error must contain code and message");
  }
  if (!json.at("message").is_string()) {
    return invalidError("error message must be a string");
  }

  int code = 0;
  if (!readErrorCode(json.at("code"), code)) {
    return invalidError("error code must be an integer");
  }

  McpError error;
  error.code = static_cast<ErrorCode>(code);
  error.message = json.at("message").get<std::string>();
  if (json.contains("data")) {
    error.data = json.at("data");
  }
  // Unknown members (e.g. a non-standard "retryable") are tolerated and ignored:
  // retry hints are local policy, not part of the wire contract.
  out = std::move(error);
  return Status::success();
}

Status Status::success() noexcept { return Status{}; }

Status Status::failure(McpError error) {
  Status status;
  status.ok = false;
  status.error = std::move(error);
  return status;
}

} // namespace v1
} // namespace mcp
