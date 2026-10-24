#include "tugmcp/canonical.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_MSC_VER) || (defined(__GNUC__) && __GNUC__ >= 11) || \
    defined(__clang__)
#include <charconv>
#define TUGMCP_CANONICAL_HAS_FLOAT_TO_CHARS 1
#endif

namespace mcp {
namespace v1 {
namespace {

constexpr int kMaxDepth = 512;

bool isValidUtf8(const std::string& text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
      ++index;
      continue;
    }

    int extra = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      codepoint = lead & 0x1F;
      if (lead < 0xC2) { // overlong
        return false;
      }
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      codepoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      codepoint = lead & 0x07;
      if (lead > 0xF4) {
        return false;
      }
    } else {
      return false;
    }

    if (index + static_cast<std::size_t>(extra) >= text.size()) {
      return false;
    }
    for (int offset = 1; offset <= extra; ++offset) {
      const unsigned char continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }

    if (extra == 2 && codepoint < 0x800) {
      return false;
    }
    if (extra == 3 && codepoint < 0x10000) {
      return false;
    }
    if (codepoint > 0x10FFFF) {
      return false;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      return false;
    }

    index += static_cast<std::size_t>(extra) + 1;
  }
  return true;
}

void appendEscapedString(const std::string& text, std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const unsigned char character : text) {
    switch (character) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (character < 0x20) {
        out += "\\u00";
        out.push_back(kHex[(character >> 4) & 0x0F]);
        out.push_back(kHex[character & 0x0F]);
      } else {
        out.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  out.push_back('"');
}

bool appendNumber(const Json& value, std::string& out, std::string& error) {
  if (value.is_number_unsigned()) {
    out += std::to_string(value.get<std::uint64_t>());
    return true;
  }
  if (value.is_number_integer()) {
    out += std::to_string(value.get<std::int64_t>());
    return true;
  }
  if (value.is_number_float()) {
    const double number = value.get<double>();
    if (!std::isfinite(number)) {
      error = "canonical JSON cannot encode NaN or infinity";
      return false;
    }
    if (number == 0.0) {
      out.push_back('0');
      return true;
    }
#ifdef TUGMCP_CANONICAL_HAS_FLOAT_TO_CHARS
    std::array<char, 64> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    if (result.ec != std::errc()) {
      error = "canonical JSON could not format a floating point value";
      return false;
    }
    out.append(buffer.data(), result.ptr);
#else
    // Older standard libraries have no floating point std::to_chars; nlohmann
    // itself emits a round-tripping form, so reuse it rather than inventing one.
    out += value.dump();
#endif
    return true;
  }
  error = "canonical JSON encountered an unsupported number";
  return false;
}

bool encode(const Json& value, std::string& out, int depth, std::string& error) {
  if (depth > kMaxDepth) {
    error = "canonical JSON nesting exceeds the limit";
    return false;
  }

  if (value.is_null()) {
    out += "null";
    return true;
  }
  if (value.is_boolean()) {
    out += value.get<bool>() ? "true" : "false";
    return true;
  }
  if (value.is_number()) {
    return appendNumber(value, out, error);
  }
  if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    if (!isValidUtf8(text)) {
      error = "canonical JSON cannot encode invalid UTF-8";
      return false;
    }
    appendEscapedString(text, out);
    return true;
  }
  if (value.is_array()) {
    out.push_back('[');
    bool first = true;
    for (const auto& item : value) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      if (!encode(item, out, depth + 1, error)) {
        return false;
      }
    }
    out.push_back(']');
    return true;
  }
  if (value.is_object()) {
    std::vector<std::pair<std::string, const Json*>> members;
    members.reserve(value.size());
    for (auto it = value.begin(); it != value.end(); ++it) {
      members.emplace_back(it.key(), &it.value());
    }
    std::sort(members.begin(), members.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });

    out.push_back('{');
    bool first = true;
    for (const auto& member : members) {
      if (!isValidUtf8(member.first)) {
        error = "canonical JSON cannot encode an invalid UTF-8 object key";
        return false;
      }
      if (!first) {
        out.push_back(',');
      }
      first = false;
      appendEscapedString(member.first, out);
      out.push_back(':');
      if (!encode(*member.second, out, depth + 1, error)) {
        return false;
      }
    }
    out.push_back('}');
    return true;
  }

  error = "canonical JSON encountered an unsupported value";
  return false;
}

// --- sha256 -------------------------------------------------------------

inline std::uint32_t rotr(std::uint32_t value, int shift) {
  return (value >> shift) | (value << (32 - shift));
}

std::array<std::uint8_t, 32> sha256(const std::string& input) {
  static constexpr std::array<std::uint32_t, 64> kConstants = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

  std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                        0x1f83d9abu, 0x5be0cd19u};

  std::string message = input;
  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(message.size()) * 8u;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) {
    message.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<char>((bit_length >> shift) & 0xFFu));
  }

  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::array<std::uint32_t, 64> schedule{};
    for (int index = 0; index < 16; ++index) {
      const std::size_t base = offset + static_cast<std::size_t>(index) * 4;
      schedule[static_cast<std::size_t>(index)] =
          (static_cast<std::uint32_t>(
               static_cast<unsigned char>(message[base]))
           << 24) |
          (static_cast<std::uint32_t>(
               static_cast<unsigned char>(message[base + 1]))
           << 16) |
          (static_cast<std::uint32_t>(
               static_cast<unsigned char>(message[base + 2]))
           << 8) |
          static_cast<std::uint32_t>(
              static_cast<unsigned char>(message[base + 3]));
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^
                               rotr(schedule[index - 15], 18) ^
                               (schedule[index - 15] >> 3);
      const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^
                               rotr(schedule[index - 2], 19) ^
                               (schedule[index - 2] >> 10);
      schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t index = 0; index < 64; ++index) {
      const std::uint32_t sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t temp1 =
          h + sum1 + choice + kConstants[index] + schedule[index];
      const std::uint32_t sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = sum0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state[index] >> 24) & 0xFFu);
    digest[index * 4 + 1] =
        static_cast<std::uint8_t>((state[index] >> 16) & 0xFFu);
    digest[index * 4 + 2] =
        static_cast<std::uint8_t>((state[index] >> 8) & 0xFFu);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state[index] & 0xFFu);
  }
  return digest;
}

std::string toHex(const std::array<std::uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    hex.push_back(kHex[(byte >> 4) & 0x0F]);
    hex.push_back(kHex[byte & 0x0F]);
  }
  return hex;
}

Result<std::string> failure(std::string message) {
  return Result<std::string>::failure(
      McpError::make(ErrorCode::InvalidParams, std::move(message)));
}

} // namespace

Result<std::string> canonicalize(const Json& value) {
  std::string output;
  std::string error;
  if (!encode(value, output, 0, error)) {
    return failure(std::move(error));
  }
  return output;
}

std::string sha256Hex(std::string_view data) {
  return toHex(sha256(std::string(data)));
}

Result<std::string> toolSchemaHash(const ToolDescriptor& tool) {
  Json projection;
  projection["name"] = tool.name;
  projection["inputSchema"] = tool.inputSchema;
  projection["outputSchema"] = tool.outputSchema;
  projection["annotations"] = tool.annotations;

  auto canonical = canonicalize(projection);
  if (!canonical.hasValue()) {
    return canonical;
  }

  const std::string payload = "mcp-tool-v1\n" + canonical.value();
  return toHex(sha256(payload));
}

} // namespace v1
} // namespace mcp
