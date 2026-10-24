#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

// JSON-RPC and MCP error codes used by the library.
enum class ErrorCode : int {
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,
  HeaderMismatch = -32020,
  MissingRequiredClientCapability = -32021,
  MissingCapability = MissingRequiredClientCapability,
  UnsupportedVersion = -32022
};

// Backwards-compatible unqualified spelling for ErrorCode::MissingCapability.
inline constexpr ErrorCode MissingCapability =
    ErrorCode::MissingRequiredClientCapability;

// Human-readable name for an error code (diagnostics only). Returns
// std::string rather than string_view so doctest's ADL stringification keeps
// working for enum comparisons.
std::string toString(ErrorCode code);

struct McpError {
  ErrorCode code = ErrorCode::InternalError;
  std::string message;
  Json data = nullptr;
  bool retryable = false;

  static McpError make(ErrorCode code, std::string message,
                       Json data = Json(), bool retryable = false);
};

// Convert an error to/from its JSON-RPC error-object representation.
Json errorToJson(const McpError& error);

// Error-as-value container. Accessing the wrong alternative is a programming
// error and throws std::logic_error.
template <class T>
class Result {
  // Result<McpError> would degrade to std::variant<McpError, McpError> and make
  // failure() indistinguishable from success; use Status with an out-parameter.
  static_assert(!std::is_same<T, McpError>::value,
                "Result<McpError> is ambiguous; use Status instead");

public:
  Result(T value) : storage_(std::move(value)) {}

  static Result failure(McpError error) {
    return Result(std::move(error), FailureTag{});
  }

  bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }

  explicit operator bool() const noexcept { return hasValue(); }

  T& value() {
    if (!hasValue()) {
      throw std::logic_error("Result::value() called without a value");
    }
    return std::get<T>(storage_);
  }

  const T& value() const {
    if (!hasValue()) {
      throw std::logic_error("Result::value() called without a value");
    }
    return std::get<T>(storage_);
  }

  McpError& error() {
    if (hasValue()) {
      throw std::logic_error("Result::error() called on a successful result");
    }
    return std::get<McpError>(storage_);
  }

  const McpError& error() const {
    if (hasValue()) {
      throw std::logic_error("Result::error() called on a successful result");
    }
    return std::get<McpError>(storage_);
  }

private:
  struct FailureTag {};

  Result(McpError error, FailureTag) : storage_(std::move(error)) {}

  std::variant<T, McpError> storage_;
};

struct Status {
  bool ok = true;
  McpError error{};

  static Status success() noexcept;

  static Status failure(McpError error);

  explicit operator bool() const noexcept { return ok; }
};

// Parses a JSON-RPC error object. Returns a failed Status (InvalidRequest) when
// the object is malformed; on success writes the parsed error to `out`.
Status errorFromJson(const Json& json, McpError& out);

} // namespace v1
} // namespace mcp
