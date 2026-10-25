// Second translation unit for the single-header ODR/link test.
//
// It intentionally does NOT define TUGMCP_IMPLEMENTATION. The inline/public
// declarations are used here; implementation symbols resolve to TU1.
#ifndef TUGMCP_AMALGAMATION_HEADER_NAME
#error "TUGMCP_AMALGAMATION_HEADER_NAME must name the generated header"
#endif
#include TUGMCP_AMALGAMATION_HEADER_NAME

#include <cstdint>
#include <string>

int consumerSecondUnit() {
  mcp::v1::RequestId id;
  id.value = std::int64_t{7};
  const mcp::v1::Json encoded_id = id.toJson();
  const std::string hash = mcp::v1::sha256Hex("abc");
  if (!encoded_id.is_number_integer() ||
      encoded_id.get<std::int64_t>() != 7) {
    return 1;
  }
  if (hash.size() != 64) {
    return 2;
  }
  return 0;
}
