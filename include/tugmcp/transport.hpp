#pragma once

#include <functional>
#include <string_view>

#include "error.hpp"
#include "message.hpp"

namespace mcp {
namespace v1 {

// Byte/message transport contract. A transport only moves JSON-RPC envelopes;
// it does not know MCP method semantics. Handlers are set before start() and
// are never invoked after close() returns (except a single onClose).
class ITransport {
public:
  virtual ~ITransport() = default;

  // Start the receive side. Idempotent: a second call without an intervening
  // close() is a no-op that succeeds.
  virtual Status start() = 0;

  // Serialize and send one message. Thread-safe. Fails once closed.
  virtual Status send(const Json& message) = 0;

  // Strict JSON text convenience path. The default validates the text and
  // forwards to send(); transports may override it to preserve raw bytes.
  virtual Status sendRaw(std::string_view raw) {
    auto parsed = parseStrict(raw);
    if (!parsed.hasValue()) {
      return Status::failure(parsed.error());
    }
    return send(parsed.value());
  }

  // Receive callback. Set before start().
  virtual void setOnMessage(std::function<void(Json)> handler) = 0;

  // Transport/framing error callback (not protocol dispatch errors).
  virtual void setOnError(std::function<void(McpError)> handler) = 0;

  // Called once on EOF or close().
  virtual void setOnClose(std::function<void()> handler) = 0;

  // Stop the transport. Idempotent; no onMessage/onError follows.
  virtual void close() = 0;
};

} // namespace v1
} // namespace mcp
