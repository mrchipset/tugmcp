#pragma once

#include <atomic>
#include <string>

#include "message.hpp"

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

// Per-request state handed to handlers. The token pointer is non-owning and is
// only valid for the duration of the call.
struct RequestContext {
  RequestId id;
  std::string method;
  CancellationToken* cancellation = nullptr;
};

} // namespace v1
} // namespace mcp
