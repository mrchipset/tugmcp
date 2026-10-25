// Second translation unit for the HTTP single-header ODR test.
//
// It intentionally does NOT define TUGMCP_IMPLEMENTATION. The build compiles
// this TU with only the generated header and nlohmann/json on the include path
// (no cpp-httplib include path), proving the declaration section is
// httplib-free.

#ifndef TUGMCP_HTTP_DECL_HEADER
#define TUGMCP_HTTP_DECL_HEADER "tugmcp_http.hpp"
#endif
#include TUGMCP_HTTP_DECL_HEADER

#include <cstdint>
#include <string>

int consumerHttpSecondUnit() {
  mcp::v1::RequestId id;
  id.value = std::int64_t{11};
  if (!id.toJson().is_number_integer()) {
    return 1;
  }
  if (mcp::v1::sha256Hex("http-odr").size() != 64) {
    return 2;
  }

  // Touch HTTP-only declarations without ever including httplib.
  mcp::v1::StreamableHttpServerOptions server_options;
  server_options.port = 0;
  server_options.tls.enabled = false;
  if (server_options.bind != "127.0.0.1") {
    return 3;
  }
  mcp::v1::StreamableHttpClientOptions client_options;
  client_options.protocolVersion = "2025-11-25";
  if (client_options.baseUrl != "") {
    return 4;
  }
  return 0;
}
