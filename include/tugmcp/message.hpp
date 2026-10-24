#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "error.hpp"

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

// JSON-RPC request identifier. Strings and integer values are accepted. The
// unsigned alternative is only used for values above INT64_MAX.
struct RequestId {
  using Value = std::variant<std::string, std::int64_t, std::uint64_t>;

  Value value;

  static Result<RequestId> fromJson(const Json& json);
  Json toJson() const;

  // Cross-alternative numeric comparison: int64 and uint64 compare by value.
  friend bool operator==(const RequestId& lhs, const RequestId& rhs) {
    if (const auto* left_int = std::get_if<std::int64_t>(&lhs.value)) {
      if (const auto* right_int = std::get_if<std::int64_t>(&rhs.value)) {
        return *left_int == *right_int;
      }
      if (const auto* right_uint = std::get_if<std::uint64_t>(&rhs.value)) {
        return *left_int >= 0 &&
               static_cast<std::uint64_t>(*left_int) == *right_uint;
      }
      return false;
    }

    if (const auto* left_uint = std::get_if<std::uint64_t>(&lhs.value)) {
      if (const auto* right_uint = std::get_if<std::uint64_t>(&rhs.value)) {
        return *left_uint == *right_uint;
      }
      if (const auto* right_int = std::get_if<std::int64_t>(&rhs.value)) {
        return *right_int >= 0 &&
               static_cast<std::uint64_t>(*right_int) == *left_uint;
      }
      return false;
    }

    const auto* left_text = std::get_if<std::string>(&lhs.value);
    const auto* right_text = std::get_if<std::string>(&rhs.value);
    return left_text != nullptr && right_text != nullptr &&
           *left_text == *right_text;
  }

  friend bool operator!=(const RequestId& lhs, const RequestId& rhs) {
    return !(lhs == rhs);
  }
};

enum class MessageKind { Request, Notification, Response };

struct Message {
  MessageKind kind = MessageKind::Request;
  Json jsonrpc = "2.0";
  RequestId id{};                // Request / Response
  std::string method;            // Request / Notification
  Json params = nullptr;         // Request / Notification: object|array|null
  Json result = nullptr;         // Response
  std::optional<McpError> error; // Response
};

// Strict JSON: UTF-8 validation, duplicate-key rejection, NaN/Inf rejection,
// integer preservation. Any JSON top-level value is accepted.
Result<Json> parseStrict(std::string_view raw);

// Decode a single JSON-RPC message from a raw JSON text.
Result<Message> decode(std::string_view raw);

// Encode a message as compact JSON-RPC. Returns InternalError when the
// message cannot be represented as valid strict JSON.
Result<std::string> encode(const Message& message);

} // namespace v1
} // namespace mcp

namespace std {

template <>
struct hash<mcp::v1::RequestId> {
  std::size_t operator()(const mcp::v1::RequestId& id) const noexcept {
    if (const auto* text = std::get_if<std::string>(&id.value)) {
      return std::hash<std::string>{}(*text);
    }
    if (const auto* number = std::get_if<std::int64_t>(&id.value)) {
      if (*number >= 0) {
        return std::hash<std::uint64_t>{}(
            static_cast<std::uint64_t>(*number));
      }
      return std::hash<std::int64_t>{}(*number);
    }
    return std::hash<std::uint64_t>{}(
        std::get<std::uint64_t>(id.value));
  }
};

} // namespace std
