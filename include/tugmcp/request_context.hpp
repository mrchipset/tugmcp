#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "message.hpp"
#include "security.hpp"

namespace mcp {
namespace v1 {

// Cooperative cancellation flag. Handlers poll cancelled(); the server cancels
// when the peer withdraws the request or the transport shuts down.
class CancellationToken {
public:
  void cancel() noexcept {
    cancelled_.store(true, std::memory_order_relaxed);
  }

  bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<bool> cancelled_{false};
};

// Per-request state handed to handlers. The dispatcher owns the cancellation
// token for the request lifetime; future cancellation routing will share this
// token with the pending-request registry. inputResponses and requestState are
// the verbatim MRTR fields supplied by the client (both absent for a normal
// request).
struct RequestContext {
  RequestId id;
  std::string method;
  std::shared_ptr<CancellationToken> cancellation;
  Json inputResponses;
  std::optional<std::string> requestState;
  // Client-declared and unverified. Never use this as an authorization
  // input (D10); the host remains responsible for authorization.
  Json clientCapabilities;
  std::optional<RequestId> subscriptionId;
  std::string sessionId;
  AuthContext auth;
};

} // namespace v1
} // namespace mcp
