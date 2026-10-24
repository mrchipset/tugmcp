#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tugmcp/message.hpp"
#include "tugmcp/transport.hpp"

namespace mcp {
namespace v1 {
namespace test {

// In-memory ITransport used by unit/integration tests. All members are safe to
// call from multiple threads. Callbacks are copied under the lock and invoked
// without holding it, so handlers may call back into the fake.
class FakeTransport final : public ITransport {
public:
  Status start() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return Status::failure(McpError::make(
          ErrorCode::InternalError, "transport is closed"));
    }
    if (!started_) {
      started_ = true;
      start_count.fetch_add(1, std::memory_order_relaxed);
    }
    return Status::success();
  }

  Status send(const Json& message) override {
    std::function<void(const Json&)> responder;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return Status::failure(McpError::make(
            ErrorCode::InternalError, "transport is closed"));
      }
      sent_.push_back(message);
      responder = auto_responder_;
    }
    // Invoke outside the lock so responders may safely call deliver(), which
    // copies and invokes the onMessage callback.
    if (responder) {
      responder(message);
    }
    return Status::success();
  }

  // Optional deterministic responder invoked synchronously from send().
  // Passing nullptr clears it.
  void setAutoResponder(std::function<void(const Json&)> responder) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_responder_ = std::move(responder);
  }

  void setOnMessage(std::function<void(Json)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(McpError)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_close_ = std::move(handler);
  }

  // Marks the fake closed. Does not synthesize onClose: tests drive framing
  // events explicitly with emitClose().
  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }

  // ---- test controls -------------------------------------------------------

  void deliver(Json message) {
    std::function<void(Json)> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      handler = on_message_;
    }
    if (handler) {
      handler(std::move(message));
    }
  }

  // Parses raw JSON text before delivering it. Invalid text is surfaced through
  // the onError handler as a ParseError.
  void deliverRaw(const std::string& raw) {
    auto parsed = parseStrict(raw);
    if (!parsed.hasValue()) {
      emitError(parsed.error());
      return;
    }
    deliver(std::move(parsed.value()));
  }

  void emitError(McpError error) {
    std::function<void(McpError)> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      handler = on_error_;
    }
    if (handler) {
      handler(std::move(error));
    }
  }

  void emitClose() {
    std::function<void()> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      handler = on_close_;
    }
    if (handler) {
      handler();
    }
  }

  // ---- observations --------------------------------------------------------

  std::vector<Json> sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
  }

  std::size_t sentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_.size();
  }

  void clearSent() {
    std::lock_guard<std::mutex> lock(mutex_);
    sent_.clear();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  bool started() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_;
  }

  // Number of successful start() transitions; repeated start() calls are
  // idempotent and do not increment it.
  int startCount() const {
    return start_count.load(std::memory_order_relaxed);
  }

  // Public for convenient assertions; only start() writes it.
  std::atomic<int> start_count{0};

private:
  mutable std::mutex mutex_;
  std::vector<Json> sent_;
  std::function<void(Json)> on_message_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
  std::function<void(const Json&)> auto_responder_;
  bool started_ = false;
  bool closed_ = false;
};

} // namespace test
} // namespace v1
} // namespace mcp
