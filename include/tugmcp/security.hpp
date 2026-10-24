#pragma once

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <tugmcp/error.hpp>
#include <tugmcp/message.hpp>


namespace mcp {
namespace v1 {

using Json = nlohmann::json;
// Header names are lowercased before reaching IAuthenticator.
using Headers = std::map<std::string, std::string>;

class CancellationToken;

struct AuthContext {
  bool authenticated = false;
  std::string principal;
  std::vector<std::string> roles;
  Json attributes;
};

class IAuthenticator {
public:
  virtual ~IAuthenticator() = default;
  virtual Result<AuthContext> authenticate(const Headers& headers) = 0;
};

// Authorizes individual tool/resource/prompt operations. It does not filter
// list results; lists remain explicit public enumeration (D20).
class IAuthorizer {
public:
  virtual ~IAuthorizer() = default;
  virtual Status authorize(const AuthContext& auth,
                           std::string_view toolOrResource) = 0;
};

struct ApprovalRequest {
  AuthContext auth;
  std::string tool;
  Json arguments;
  RequestId id;
  std::chrono::steady_clock::time_point deadline{};
  std::shared_ptr<CancellationToken> cancellation;
};

struct ApprovalResult {
  bool approved = false;
  std::string reason;
};

class IApprovalHook {
public:
  virtual ~IApprovalHook() = default;
  virtual std::future<ApprovalResult> approve(const ApprovalRequest&) = 0;
};

// Observes pre-execution authorization/approval events only; audit errors
// never change authorization or execution outcomes.
class IAuditHook {
public:
  virtual ~IAuditHook() = default;
  virtual void onEvent(const AuthContext& auth, std::string_view event,
                       const Json& detail) = 0;
};

} // namespace v1
} // namespace mcp
