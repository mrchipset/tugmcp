#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "error.hpp"
#include "message.hpp"

namespace mcp {
namespace v1 {

// Transport-supplied metadata for an inbound message. An empty
// protocolVersion means the transport did not observe an explicit version (for
// example stdio or a legacy client). An empty route means the transport has a
// single logical route (sendTo falls back to send).
struct MessageMetadata {
  std::string protocolVersion;
  std::string route;
  std::string sessionId;
  bool emergency = false;
};

using MessageHandlerEx = std::function<void(Json, const MessageMetadata&)>;

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

  // Send a message on a transport-defined route (the route is the opaque token
  // supplied with MessageMetadata). Single-route transports ignore the route.
  virtual Status sendTo(std::string_view route, const Json& message) {
    (void)route;
    return send(message);
  }

  // Send to a server-managed session stream (2025 GET stream).
  virtual Status sendToSession(std::string_view sessionId, const Json& message) {
    (void)sessionId;
    (void)message;
    return Status::failure(McpError::make(
        ErrorCode::InternalError, "session not supported",
        Json{{"reason", "session_not_supported"}}));
  }

  // Best-effort abort of transport work for the given request. This is the
  // CloseStream cancellation path: it must not block and may be a no-op for
  // transports that cannot interrupt an in-flight send.
  virtual void cancelPending(const RequestId&) {}

  // Whether this transport can open per-request server streams (SSE).
  virtual bool supportsStreaming() const { return false; }

  // Optional notification that a transport route is gone. Single-route
  // transports may leave this unset.
  virtual void setOnRouteClosed(std::function<void(const std::string&)>) {}

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

  // Metadata-aware receive callback. Transports that can observe per-message
  // protocol information override this; by default it adapts to setOnMessage
  // and reports no metadata.
  virtual void setOnMessageEx(MessageHandlerEx handler) {
    setOnMessage([handler = std::move(handler)](Json message) mutable {
      handler(std::move(message), MessageMetadata{});
    });
  }

  // Transport/framing error callback (not protocol dispatch errors).
  virtual void setOnError(std::function<void(McpError)> handler) = 0;

  // Called once on EOF or close().
  virtual void setOnClose(std::function<void()> handler) = 0;

  // Stop the transport. Idempotent; no onMessage/onError follows.
  virtual void close() = 0;
};

} // namespace v1
} // namespace mcp
