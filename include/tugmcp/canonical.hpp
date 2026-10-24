#pragma once

#include <string>
#include <string_view>

#include <tugmcp/tool.hpp>

namespace mcp {
namespace v1 {

// Lowercase hexadecimal SHA-256 over the UTF-8 bytes of `data`. Exposed so
// callers and tests can anchor hashes independently of canonicalization.
std::string sha256Hex(std::string_view data);

// Canonicalizes `value` using the documented RFC 8785 subset:
//   * object members are sorted by the UTF-8 byte order of their keys;
//   * arrays keep their original order;
//   * strings use standard JSON escaping and emit non-ASCII UTF-8 bytes as-is;
//   * integers are rendered in decimal (int64/uint64 precision preserved);
//   * floating point values use the shortest round-tripping representation and
//     negative zero is normalized to "0";
//   * NaN and infinity are rejected;
//   * invalid UTF-8 is rejected.
//
// This is intentionally a subset of JCS, not a claim of full compliance: the
// exact ECMAScript number-to-string formatting is not reproduced, only the
// shortest round-trip form. The algorithm is stable for a given input and
// toolchain, which is what tool_schema_hash relies on.
Result<std::string> canonicalize(const Json& value);

// sha256 hex of "mcp-tool-v1\n" + canonical({name, inputSchema, outputSchema,
// annotations}). Description/title/idempotency are intentionally excluded.
Result<std::string> toolSchemaHash(const ToolDescriptor& tool);

} // namespace v1
} // namespace mcp
