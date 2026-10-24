#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mcp {
namespace v1 {
namespace detail {

inline std::string asciiLower(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

inline std::string hostFromAuthority(const std::string& authority) {
  if (authority.empty()) {
    return {};
  }
  if (authority.front() == '[') {
    const std::size_t end = authority.find(']');
    if (end == std::string::npos) {
      return asciiLower(authority);
    }
    return asciiLower(authority.substr(1, end - 1));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(':') == colon) {
    return asciiLower(authority.substr(0, colon));
  }
  return asciiLower(authority);
}

inline bool isLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

inline bool listMatches(const std::vector<std::string>& list,
                        const std::string& value) {
  const std::string candidate = asciiLower(value);
  for (const auto& item : list) {
    if (item == "*" || asciiLower(item) == candidate) {
      return true;
    }
  }
  return false;
}

inline bool hostAllowed(const std::string& host_header,
                        const std::vector<std::string>& allowed_hosts) {
  if (host_header.empty()) {
    return false;
  }
  const std::string host = hostFromAuthority(host_header);
  if (isLoopbackHost(host)) {
    return true;
  }
  return listMatches(allowed_hosts, host);
}

inline bool originAllowed(const std::string& origin,
                          const std::vector<std::string>& allowed_origins) {
  if (origin.empty()) {
    return true;
  }
  if (origin == "null") {
    return false;
  }
  const std::size_t scheme_end = origin.find("://");
  const std::string authority =
      scheme_end == std::string::npos ? origin : origin.substr(scheme_end + 3);
  const std::size_t path = authority.find('/');
  const std::string host = hostFromAuthority(
      path == std::string::npos ? authority : authority.substr(0, path));
  if (isLoopbackHost(host)) {
    return true;
  }
  return listMatches(allowed_origins, origin);
}

} // namespace detail
} // namespace v1
} // namespace mcp
