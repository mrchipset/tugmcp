#include "tugmcp/message.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {
namespace v1 {
namespace {

constexpr int kCodecMaxDepth = 256;

// Strict UTF-8 validator: rejects overlong encodings, surrogates and code
// points above U+10FFFF. The JSON parser itself does not validate UTF-8.
bool codecIsValidUtf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t index = 0;

  while (index < size) {
    const unsigned char lead = bytes[index];
    if (lead <= 0x7F) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t code_point = 0;

    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x1F);
      if (code_point < 2) {
        return false; // overlong
      }
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x0F);
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      code_point = static_cast<std::uint32_t>(lead & 0x07);
    } else {
      return false;
    }

    if (index + length > size) {
      return false;
    }

    for (std::size_t offset = 1; offset < length; ++offset) {
      const unsigned char continuation = bytes[index + offset];
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      code_point =
          (code_point << 6) | static_cast<std::uint32_t>(continuation & 0x3F);
    }

    if (length == 3 && code_point < 0x800) {
      return false;
    }
    if (length == 4 && code_point < 0x10000) {
      return false;
    }
    if (code_point > 0x10FFFF) {
      return false;
    }
    if (code_point >= 0xD800 && code_point <= 0xDFFF) {
      return false;
    }

    index += length;
  }

  return true;
}

// SAX handler that rejects duplicate object keys at any depth and aborts on
// nesting deeper than kCodecMaxDepth before the DOM parser can recurse.
class StrictSax : public nlohmann::json_sax<Json> {
public:
  using number_integer_t = nlohmann::json_sax<Json>::number_integer_t;
  using number_unsigned_t = nlohmann::json_sax<Json>::number_unsigned_t;
  using number_float_t = nlohmann::json_sax<Json>::number_float_t;
  using string_t = nlohmann::json_sax<Json>::string_t;
  using binary_t = nlohmann::json_sax<Json>::binary_t;

  bool null() override { return true; }

  bool boolean(bool) override { return true; }

  bool number_integer(number_integer_t) override { return true; }

  bool number_unsigned(number_unsigned_t) override { return true; }

  bool number_float(number_float_t value, const string_t&) override {
    if (!std::isfinite(value)) {
      non_finite_ = true;
      return false;
    }
    return true;
  }

  bool string(string_t&) override { return true; }

  bool binary(binary_t&) override { return true; }

  bool start_object(std::size_t) override {
    if (++depth_ > kCodecMaxDepth) {
      too_deep_ = true;
      return false;
    }
    objects_.emplace_back();
    return true;
  }

  bool key(string_t& value) override {
    if (objects_.empty()) {
      syntax_error_ = true;
      return false;
    }
    if (!objects_.back().insert(value).second) {
      duplicate_key_ = true;
      return false;
    }
    return true;
  }

  bool end_object() override {
    if (objects_.empty()) {
      syntax_error_ = true;
      return false;
    }
    objects_.pop_back();
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (++depth_ > kCodecMaxDepth) {
      too_deep_ = true;
      return false;
    }
    return true;
  }

  bool end_array() override {
    --depth_;
    return true;
  }

  bool parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) override {
    syntax_error_ = true;
    return false;
  }

  bool duplicate_key() const noexcept { return duplicate_key_; }
  bool too_deep() const noexcept { return too_deep_; }
  bool syntax_error() const noexcept { return syntax_error_; }
  bool non_finite() const noexcept { return non_finite_; }

private:
  int depth_ = 0;
  bool duplicate_key_ = false;
  bool too_deep_ = false;
  bool syntax_error_ = false;
  bool non_finite_ = false;
  std::vector<std::set<std::string>> objects_;
};

bool hasOnlyFiniteNumbers(const Json& value) {
  if (value.is_number_float()) {
    return std::isfinite(value.get<double>());
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!hasOnlyFiniteNumbers(it.value())) {
        return false;
      }
    }
  } else if (value.is_array()) {
    for (const auto& element : value) {
      if (!hasOnlyFiniteNumbers(element)) {
        return false;
      }
    }
  }
  return true;
}

bool hasOnlyValidUtf8Strings(const Json& value) {
  if (value.is_string()) {
    return codecIsValidUtf8(value.get_ref<const std::string&>());
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!codecIsValidUtf8(it.key()) || !hasOnlyValidUtf8Strings(it.value())) {
        return false;
      }
    }
  } else if (value.is_array()) {
    for (const auto& element : value) {
      if (!hasOnlyValidUtf8Strings(element)) {
        return false;
      }
    }
  }
  return true;
}

Result<Message> invalidRequest(std::string message) {
  return Result<Message>::failure(
      McpError::make(ErrorCode::InvalidRequest, std::move(message)));
}

Result<std::string> invalidEncodeRequest(std::string message) {
  return Result<std::string>::failure(
      McpError::make(ErrorCode::InvalidRequest, std::move(message)));
}

Result<std::string> internalEncodeError(std::string message) {
  return Result<std::string>::failure(
      McpError::make(ErrorCode::InternalError, std::move(message)));
}

} // namespace

Result<Json> parseStrict(std::string_view raw) {
  if (!codecIsValidUtf8(raw)) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "invalid UTF-8"));
  }

  StrictSax sax;
  bool parsed = false;
  try {
    parsed = Json::sax_parse(raw.begin(), raw.end(), &sax);
  } catch (const std::exception&) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "malformed JSON"));
  } catch (...) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "malformed JSON"));
  }

  if (sax.duplicate_key()) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "duplicate object key"));
  }
  if (sax.too_deep()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::ParseError, "maximum nesting depth exceeded"));
  }
  if (sax.non_finite()) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::ParseError, "non-finite number is not allowed"));
  }
  if (!parsed || sax.syntax_error()) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "malformed JSON"));
  }

  Json document;
  try {
    document = Json::parse(raw.begin(), raw.end());
  } catch (const std::exception&) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "malformed JSON"));
  } catch (...) {
    return Result<Json>::failure(
        McpError::make(ErrorCode::ParseError, "malformed JSON"));
  }

  if (!hasOnlyFiniteNumbers(document)) {
    return Result<Json>::failure(McpError::make(
        ErrorCode::ParseError, "non-finite number is not allowed"));
  }

  return document;
}

Result<Message> decode(std::string_view raw) {
  auto parsed = parseStrict(raw);
  if (!parsed.hasValue()) {
    return Result<Message>::failure(parsed.error());
  }
  return decode(parsed.value());
}

Result<Message> decode(const std::string& raw) {
  return decode(std::string_view(raw));
}

Result<Message> decode(const char* raw) {
  return decode(raw != nullptr ? std::string_view(raw) : std::string_view());
}

Result<Message> decode(const Json& document) {
  if (document.is_array()) {
    return invalidRequest("batch is not supported");
  }
  if (!document.is_object()) {
    return invalidRequest("message must be a JSON object");
  }

  try {
    if (!document.contains("jsonrpc") || !document.at("jsonrpc").is_string() ||
        document.at("jsonrpc").get<std::string>() != "2.0") {
      return invalidRequest("jsonrpc must be \"2.0\"");
    }

    const bool has_method = document.contains("method");
    const bool has_id = document.contains("id");

    if (has_method) {
      if (document.contains("result") || document.contains("error")) {
        return invalidRequest(
            "request must not contain result or error");
      }

      const Json& method = document.at("method");
      if (!method.is_string() || method.get<std::string>().empty()) {
        return invalidRequest("method must be a non-empty string");
      }

      Json params = nullptr;
      if (document.contains("params") && !document.at("params").is_null()) {
        const Json& raw_params = document.at("params");
        if (!raw_params.is_object() && !raw_params.is_array()) {
          return invalidRequest("params must be an object, array or null");
        }
        params = raw_params;
      }

      Message message;
      message.jsonrpc = "2.0";
      message.method = method.get<std::string>();
      message.params = std::move(params);

      if (has_id) {
        auto id = RequestId::fromJson(document.at("id"));
        if (!id.hasValue()) {
          return Result<Message>::failure(id.error());
        }
        message.kind = MessageKind::Request;
        message.id = id.value();
      } else {
        message.kind = MessageKind::Notification;
      }

      return message;
    }

    if (has_id) {
      if (document.contains("params")) {
        return invalidRequest("response must not contain params");
      }

      auto id = RequestId::fromJson(document.at("id"));
      if (!id.hasValue()) {
        return Result<Message>::failure(id.error());
      }

      const bool has_result = document.contains("result");
      const bool has_error = document.contains("error");
      if (has_result == has_error) {
        return invalidRequest(
            "response must contain exactly one of result or error");
      }

      Message message;
      message.jsonrpc = "2.0";
      message.kind = MessageKind::Response;
      message.id = id.value();

      if (has_result) {
        message.result = document.at("result");
        return message;
      }

      McpError error;
      const Status error_status = errorFromJson(document.at("error"), error);
      if (!error_status) {
        return Result<Message>::failure(error_status.error);
      }
      message.error = std::move(error);
      return message;
    }

    return invalidRequest("message must contain method or id");
  } catch (const std::exception&) {
    return invalidRequest("malformed message");
  } catch (...) {
    return invalidRequest("malformed message");
  }
}

Result<std::string> encode(const Message& message) {
  Json json;
  json["jsonrpc"] = "2.0";

  switch (message.kind) {
  case MessageKind::Request:
    if (message.method.empty()) {
      return invalidEncodeRequest("method must be non-empty");
    }
    json["id"] = message.id.toJson();
    json["method"] = message.method;
    if (!message.params.is_null()) {
      json["params"] = message.params;
    }
    break;

  case MessageKind::Notification:
    if (message.method.empty()) {
      return invalidEncodeRequest("method must be non-empty");
    }
    json["method"] = message.method;
    if (!message.params.is_null()) {
      json["params"] = message.params;
    }
    break;

  case MessageKind::Response: {
    const bool has_error = message.error.has_value();
    const bool has_result = !message.result.is_null();
    if (has_error && has_result) {
      return invalidEncodeRequest(
          "response must contain exactly one of result or error");
    }

    json["id"] = message.id.toJson();
    if (has_error) {
      json["error"] = errorToJson(*message.error);
    } else {
      json["result"] = message.result;
    }
    break;
  }
  }

  if (!hasOnlyFiniteNumbers(json)) {
    return internalEncodeError("non-finite number is not allowed");
  }
  if (!hasOnlyValidUtf8Strings(json)) {
    return internalEncodeError("invalid UTF-8 in message");
  }

  try {
    return json.dump(-1, ' ', false, Json::error_handler_t::strict);
  } catch (...) {
    return internalEncodeError("failed to encode message");
  }
}

} // namespace v1
} // namespace mcp
