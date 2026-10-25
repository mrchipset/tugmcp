// Standalone TLS single-header compile/link/ODR smoke test.
//
// It does not start a TLS server. It proves the TLS variant defines the
// OpenSSL/cpp-httplib macros before <httplib.h>, exposes the expected
// declarations, and links the implementation with OpenSSL.

#define TUGMCP_IMPLEMENTATION
#ifndef TUGMCP_AMALGAMATION_HTTP_TLS_HEADER_NAME
#define TUGMCP_AMALGAMATION_HTTP_TLS_HEADER_NAME "tugmcp_http_tls.hpp"
#endif
#include TUGMCP_AMALGAMATION_HTTP_TLS_HEADER_NAME

#include <iostream>
#include <string>

int consumerHttpSecondUnit();

int main() {
  mcp::v1::StreamableHttpServerOptions options;
  options.tls.enabled = true;

  const auto created = mcp::v1::makeStreamableHttpServerTransport(options);
  if (created.hasValue()) {
    std::cerr << "TLS ODR smoke: expected tls_not_configured\n";
    return 1;
  }
  const std::string reason =
      created.error().data.value("reason", std::string{});
  if (reason != "tls_not_configured") {
    std::cerr << "TLS ODR smoke: unexpected reason " << reason << '\n';
    return 2;
  }
  if (mcp::v1::sha256Hex("tls-odr").size() != 64) {
    std::cerr << "TLS ODR smoke: sha256 mismatch\n";
    return 3;
  }
  if (consumerHttpSecondUnit() != 0) {
    std::cerr << "TLS ODR smoke: second TU failed\n";
    return 4;
  }
  std::cout << "amalgamation TLS ODR: all checks passed\n";
  return 0;
}
