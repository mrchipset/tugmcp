#include "tugmcp/transport/stdio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "tugmcp/message.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // keep windows.h from shadowing std::min/std::max
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#endif

namespace mcp {
namespace v1 {
namespace {

constexpr std::size_t kReadChunkBytes = 4096;
constexpr std::size_t kInitialLineReserve = 64u * 1024u;

Status internalFailure(const char* message) {
  return Status::failure(McpError::make(ErrorCode::InternalError, message));
}

} // namespace

class StdioTransport::Impl {
public:
  explicit Impl(StdioOptions options) : options_(options) {}

  ~Impl() {
    close();
    // A close() from inside the reader thread cannot join itself; finish the
    // hand-off here instead of letting ~std::thread terminate the process.
    if (reader_.joinable()) {
      if (reader_.get_id() == std::this_thread::get_id()) {
        reader_.detach();
      } else {
        reader_.join();
      }
    }
  }

  Status start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
      return internalFailure("transport is closed");
    }
    if (started_) {
      return Status::success();
    }
    if (options_.input == nullptr || options_.output == nullptr) {
      return internalFailure("stdio stream is null");
    }

    if (options_.setBinaryMode) {
      enableBinaryMode();
    }

    try {
      reader_ = std::thread([this] {
        try {
          readLoop();
        } catch (...) {
          // Never let an exception escape the reader thread.
          closed_.store(true, std::memory_order_release);
          notifyClose();
        }
      });
    } catch (...) {
      return internalFailure("failed to start stdio reader thread");
    }
    started_ = true;
    return Status::success();
  }

  Status send(const Json& message) {
    if (closed_.load(std::memory_order_acquire)) {
      return internalFailure("transport is closed");
    }

    if (options_.output == nullptr) {
      return internalFailure("stdio output stream is null");
    }

    std::string payload;
    try {
      payload =
          message.dump(-1, ' ', false, Json::error_handler_t::strict);
    } catch (...) {
      return internalFailure("failed to encode message");
    }

    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
      return internalFailure("transport is closed");
    }
    if (std::fwrite(payload.data(), 1, payload.size(), options_.output) !=
        payload.size()) {
      return internalFailure("failed to write to stdio output");
    }
    if (std::fputc('\n', options_.output) == EOF) {
      return internalFailure("failed to write to stdio output");
    }
    if (std::fflush(options_.output) != 0) {
      return internalFailure("failed to flush stdio output");
    }
    return Status::success();
  }

  Status sendRaw(std::string_view raw) {
    if (closed_.load(std::memory_order_acquire)) {
      return internalFailure("transport is closed");
    }
    if (options_.output == nullptr) {
      return internalFailure("stdio output stream is null");
    }

    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
      return internalFailure("transport is closed");
    }
    if (!raw.empty() &&
        std::fwrite(raw.data(), 1, raw.size(), options_.output) !=
            raw.size()) {
      return internalFailure("failed to write to stdio output");
    }
    if (std::fputc('\n', options_.output) == EOF) {
      return internalFailure("failed to write to stdio output");
    }
    if (std::fflush(options_.output) != 0) {
      return internalFailure("failed to flush stdio output");
    }
    return Status::success();
  }

  void setOnMessage(std::function<void(Json)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(McpError)> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_close_ = std::move(handler);
  }

  void close() {
    // Publish the closed state first so in-flight callbacks are suppressed.
    closed_.store(true, std::memory_order_release);
    notifyClose();

    std::thread pending;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!reader_.joinable()) {
        return;
      }
      if (reader_.get_id() == std::this_thread::get_id()) {
        // Called from a callback on the reader thread: the reader exits after
        // the callback returns, and a later close()/destructor joins it.
        return;
      }
#if defined(_WIN32)
      // A reader parked inside fgets() is blocked in a synchronous read; cancel
      // it so join() cannot hang. Failure is ignored: the reader may have just
      // left the read (it will observe closed_ on its own).
      ::CancelSynchronousIo(reader_.native_handle());
#endif
      pending = std::move(reader_);
    }
#if !defined(_WIN32)
    // POSIX: there is no portable way to interrupt fgets(); this join relies on
    // the peer closing the read stream (EOF).
#endif
    pending.join();
  }

private:
  void enableBinaryMode() {
#if defined(_WIN32)
    // C stdio on Windows translates CRLF unless the underlying descriptor is
    // binary. Touch only the process streams we actually own per the options.
    if (options_.input == stdin) {
      _setmode(_fileno(stdin), _O_BINARY);
    }
    if (options_.output == stdout) {
      _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
  }

  void readLoop() {
    std::string line;
    line.reserve(std::min(options_.maxMessageBytes, kInitialLineReserve));

    std::array<char, kReadChunkBytes> chunk{};
    bool overflow = false;

    while (!closed_.load(std::memory_order_acquire)) {
      char* got = std::fgets(chunk.data(), static_cast<int>(chunk.size()),
                             options_.input);
      if (got == nullptr) {
        if (closed_.load(std::memory_order_acquire)) {
          // close() cancelled the blocking read; onClose already fired.
          break;
        }
        // EOF (or read error). A final line without a trailing newline is
        // still a complete message.
        if (!overflow && !line.empty()) {
          handleLine(line);
        }
        break;
      }

      const std::size_t length = std::strlen(chunk.data());
      const bool ends_with_newline =
          length > 0 && chunk.data()[length - 1] == '\n';
      const std::size_t payload_length = length - (ends_with_newline ? 1 : 0);

      if (!overflow) {
        if (line.size() + payload_length > options_.maxMessageBytes) {
          // Abandon the overlong line and keep consuming until its newline so
          // that framing stays aligned for the next message.
          overflow = true;
          line.clear();
          emitError(McpError::make(ErrorCode::InvalidParams,
                                   "message too large"));
        } else {
          line.append(chunk.data(), payload_length);
        }
      }

      if (ends_with_newline) {
        if (!overflow && !line.empty()) {
          handleLine(line);
        }
        line.clear();
        overflow = false;
      }
    }

    closed_.store(true, std::memory_order_release);
    notifyClose();
  }

  void handleLine(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return; // Empty lines are ignored, not errors.
    }
    if (closed_.load(std::memory_order_acquire)) {
      return;
    }

    // decode() validates the JSON-RPC envelope (strict JSON, batch rejection,
    // field combinations). ITransport hands JSON documents to the protocol
    // layer, so the same text is parsed once more into a Json value.
    auto decoded = decode(line);
    if (!decoded.hasValue()) {
      emitError(decoded.error());
      return;
    }

    auto parsed = parseStrict(line);
    if (!parsed.hasValue()) {
      emitError(parsed.error());
      return;
    }
    emitMessage(std::move(parsed.value()));
  }

  void emitMessage(Json message) {
    if (closed_.load(std::memory_order_acquire)) {
      return;
    }
    std::function<void(Json)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_message_;
    }
    if (!handler) {
      return;
    }
    try {
      handler(std::move(message));
    } catch (...) {
      // A throwing user callback must not unwind through the reader thread.
      emitError(McpError::make(ErrorCode::InternalError,
                               "onMessage handler threw"));
    }
  }

  void emitError(McpError error) {
    if (closed_.load(std::memory_order_acquire)) {
      return;
    }
    std::function<void(McpError)> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_error_;
    }
    if (!handler) {
      return;
    }
    try {
      handler(std::move(error));
    } catch (...) {
      // Diagnostics must not take down the reader thread either.
    }
  }

  void notifyClose() {
    if (close_notified_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::function<void()> handler;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      handler = on_close_;
    }
    if (!handler) {
      return;
    }
    try {
      handler();
    } catch (...) {
      // Swallow: close notification is best-effort.
    }
  }

  StdioOptions options_;

  std::mutex lifecycle_mutex_; // guards started_ and reader_
  std::mutex send_mutex_;      // serializes writes
  std::mutex callback_mutex_;  // guards the handler slots

  std::atomic<bool> closed_{false};
  std::atomic<bool> close_notified_{false};
  bool started_ = false;
  std::thread reader_;

  std::function<void(Json)> on_message_;
  std::function<void(McpError)> on_error_;
  std::function<void()> on_close_;
};

StdioTransport::StdioTransport(StdioOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

StdioTransport::~StdioTransport() = default;

Status StdioTransport::start() { return impl_->start(); }

Status StdioTransport::send(const Json& message) { return impl_->send(message); }

Status StdioTransport::sendRaw(std::string_view raw) {
  return impl_->sendRaw(raw);
}

void StdioTransport::setOnMessage(std::function<void(Json)> handler) {
  impl_->setOnMessage(std::move(handler));
}

void StdioTransport::setOnError(std::function<void(McpError)> handler) {
  impl_->setOnError(std::move(handler));
}

void StdioTransport::setOnClose(std::function<void()> handler) {
  impl_->setOnClose(std::move(handler));
}

void StdioTransport::close() { impl_->close(); }

std::shared_ptr<ITransport> makeStdioTransport(StdioOptions options) {
  return std::make_shared<StdioTransport>(std::move(options));
}

} // namespace v1
} // namespace mcp
