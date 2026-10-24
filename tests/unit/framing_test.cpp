#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <tugmcp/message.hpp>
#include <tugmcp/transport/stdio.hpp>

#include "fake_transport.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::chrono::seconds kWaitTimeout(2);

#if defined(_WIN32)

constexpr unsigned int kPipeBufferBytes = 64u * 1024u;

int createPipe(int descriptors[2]) {
  return _pipe(descriptors, kPipeBufferBytes, _O_BINARY);
}

void closeDescriptor(int descriptor) { _close(descriptor); }

long writeDescriptor(int descriptor, const char* data, std::size_t size) {
  return static_cast<long>(
      _write(descriptor, data, static_cast<unsigned int>(size)));
}

std::FILE* openReadStream(int descriptor) { return _fdopen(descriptor, "rb"); }

std::FILE* openWriteStream(int descriptor) { return _fdopen(descriptor, "wb"); }

#else

int createPipe(int descriptors[2]) { return ::pipe(descriptors); }

void closeDescriptor(int descriptor) { ::close(descriptor); }

long writeDescriptor(int descriptor, const char* data, std::size_t size) {
  return static_cast<long>(::write(descriptor, data, size));
}

std::FILE* openReadStream(int descriptor) { return ::fdopen(descriptor, "rb"); }

std::FILE* openWriteStream(int descriptor) { return ::fdopen(descriptor, "wb"); }

#endif

// Read side of a pipe handed to StdioTransport; the test owns the write end.
class InputPipe {
public:
  InputPipe() {
    int descriptors[2] = {-1, -1};
    REQUIRE(createPipe(descriptors) == 0);
    write_descriptor_ = descriptors[1];
    read_file_ = openReadStream(descriptors[0]);
    if (read_file_ == nullptr) {
      closeDescriptor(descriptors[0]);
      closeDescriptor(write_descriptor_);
      write_descriptor_ = -1;
    }
    REQUIRE(read_file_ != nullptr);
  }

  ~InputPipe() {
    closeWrite();
    if (read_file_ != nullptr) {
      std::fclose(read_file_);
    }
  }

  InputPipe(const InputPipe&) = delete;
  InputPipe& operator=(const InputPipe&) = delete;

  std::FILE* readFile() const { return read_file_; }

  void write(std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const long written =
          writeDescriptor(write_descriptor_, data.data() + offset,
                          data.size() - offset);
      REQUIRE(written > 0);
      offset += static_cast<std::size_t>(written);
    }
  }

  // Closing the write end delivers EOF to the reader thread.
  void closeWrite() {
    if (write_descriptor_ >= 0) {
      closeDescriptor(write_descriptor_);
      write_descriptor_ = -1;
    }
  }

private:
  int write_descriptor_ = -1;
  std::FILE* read_file_ = nullptr;
};

// Write side handed to StdioTransport; the test reads the produced lines.
class OutputPipe {
public:
  OutputPipe() {
    int descriptors[2] = {-1, -1};
    REQUIRE(createPipe(descriptors) == 0);
    read_file_ = openReadStream(descriptors[0]);
    write_file_ = openWriteStream(descriptors[1]);
    if (read_file_ == nullptr || write_file_ == nullptr) {
      closeDescriptor(descriptors[0]);
      closeDescriptor(descriptors[1]);
      read_file_ = nullptr;
      write_file_ = nullptr;
    }
    REQUIRE(read_file_ != nullptr);
    REQUIRE(write_file_ != nullptr);
  }

  ~OutputPipe() {
    // Flush/close the transport-facing stream before closing the reader.
    if (write_file_ != nullptr) {
      std::fclose(write_file_);
    }
    if (read_file_ != nullptr) {
      std::fclose(read_file_);
    }
  }

  OutputPipe(const OutputPipe&) = delete;
  OutputPipe& operator=(const OutputPipe&) = delete;

  std::FILE* writeFile() const { return write_file_; }

  // Blocking read of one newline-terminated line (send() flushes it).
  std::string readLine() {
    std::string buffer(4096, '\0');
    char* got = std::fgets(&buffer[0], static_cast<int>(buffer.size()),
                           read_file_);
    REQUIRE(got != nullptr);
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
  }

private:
  std::FILE* read_file_ = nullptr;
  std::FILE* write_file_ = nullptr;
};

// Thread-safe callback collector; condition variables replace sleeping.
class Collector {
public:
  void onMessage(mcp::v1::Json message) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
    condition_.notify_all();
  }

  void onError(mcp::v1::McpError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    errors_.push_back(std::move(error));
    condition_.notify_all();
  }

  void onClose() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    ++close_count_;
    condition_.notify_all();
  }

  bool waitForMessages(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout,
                               [&] { return messages_.size() >= count; });
  }

  bool waitForErrors(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout,
                               [&] { return errors_.size() >= count; });
  }

  bool waitForClose() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout, [&] { return closed_; });
  }

  std::vector<mcp::v1::Json> takeMessages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
  }

  std::vector<mcp::v1::McpError> takeErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
  }

  int closeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return close_count_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<mcp::v1::Json> messages_;
  std::vector<mcp::v1::McpError> errors_;
  bool closed_ = false;
  int close_count_ = 0;
};

// Owns the pipes and the transport and tears them down in the right order:
// close the write end first (EOF), then join the reader thread.
struct Harness {
  explicit Harness(std::size_t maxMessageBytes = 4u * 1024u * 1024u) {
    mcp::v1::StdioOptions options;
    options.input = input.readFile();
    options.output = output.writeFile();
    options.maxMessageBytes = maxMessageBytes;
    // The pipes are opened in binary mode already; do not touch stdin/stdout.
    options.setBinaryMode = false;

    transport = mcp::v1::makeStdioTransport(options);
    transport->setOnMessage(
        [this](mcp::v1::Json message) { collector.onMessage(std::move(message)); });
    transport->setOnError(
        [this](mcp::v1::McpError error) { collector.onError(std::move(error)); });
    transport->setOnClose([this] { collector.onClose(); });
  }

  ~Harness() {
    input.closeWrite();
    if (transport) {
      transport->close();
    }
  }

  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  mcp::v1::Status start() { return transport->start(); }

  InputPipe input;
  OutputPipe output;
  Collector collector;
  std::shared_ptr<mcp::v1::ITransport> transport;
};

constexpr const char* kPingLine = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

} // namespace

TEST_CASE("stdio reads one newline-delimited message") {
  Harness harness;
  REQUIRE(harness.start().ok);

  harness.input.write(std::string(kPingLine) + "\n");
  REQUIRE(harness.collector.waitForMessages(1));

  const auto messages = harness.collector.takeMessages();
  REQUIRE(messages.size() == 1);
  CHECK(messages.at(0).at("method") == "ping");
  CHECK(messages.at(0).at("id") == 1);
  CHECK(harness.collector.takeErrors().empty());

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio splits two messages written at once and tolerates CRLF") {
  Harness harness;
  REQUIRE(harness.start().ok);

  harness.input.write(
      std::string(R"({"jsonrpc":"2.0","id":1,"method":"first"})") + "\r\n" +
      std::string(R"({"jsonrpc":"2.0","id":2,"method":"second"})") + "\n");
  REQUIRE(harness.collector.waitForMessages(2));

  const auto messages = harness.collector.takeMessages();
  REQUIRE(messages.size() == 2);
  CHECK(messages.at(0).at("method") == "first");
  CHECK(messages.at(1).at("method") == "second");
  CHECK(harness.collector.takeErrors().empty());

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio ignores empty lines") {
  Harness harness;
  REQUIRE(harness.start().ok);

  // LF, CRLF and LF: all must be skipped without producing errors.
  harness.input.write(std::string("\n\r\n\n") + kPingLine + "\n");
  REQUIRE(harness.collector.waitForMessages(1));

  const auto messages = harness.collector.takeMessages();
  REQUIRE(messages.size() == 1);
  CHECK(messages.at(0).at("method") == "ping");
  CHECK(harness.collector.takeErrors().empty());

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio reports malformed JSON and keeps reading") {
  Harness harness;
  REQUIRE(harness.start().ok);

  harness.input.write(
      std::string(R"({"jsonrpc":"2.0","id":1,"method":})") + "\n" +
      kPingLine + "\n");
  REQUIRE(harness.collector.waitForErrors(1));
  REQUIRE(harness.collector.waitForMessages(1));

  const auto errors = harness.collector.takeErrors();
  REQUIRE(errors.size() == 1);
  CHECK(static_cast<int>(errors.at(0).code) ==
        static_cast<int>(mcp::v1::ErrorCode::ParseError));

  const auto messages = harness.collector.takeMessages();
  REQUIRE(messages.size() == 1);
  CHECK(messages.at(0).at("method") == "ping");

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio rejects an overlong line and recovers on the next line") {
  Harness harness(64);
  REQUIRE(harness.start().ok);

  // Spans several read chunks so the discard-until-newline path is exercised.
  harness.input.write(std::string(9000, 'a') + "\n" + kPingLine + "\n");
  REQUIRE(harness.collector.waitForErrors(1));
  REQUIRE(harness.collector.waitForMessages(1));

  const auto errors = harness.collector.takeErrors();
  REQUIRE(errors.size() == 1);
  CHECK(static_cast<int>(errors.at(0).code) ==
        static_cast<int>(mcp::v1::ErrorCode::InvalidParams));
  CHECK(errors.at(0).message == "message too large");

  const auto messages = harness.collector.takeMessages();
  REQUIRE(messages.size() == 1);
  CHECK(messages.at(0).at("method") == "ping");

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio send emits a newline-terminated message that decodes") {
  Harness harness;
  REQUIRE(harness.start().ok);

  const mcp::v1::Json response = {
      {"jsonrpc", "2.0"}, {"id", 7}, {"result", {{"ok", true}}}};
  const mcp::v1::Status status = harness.transport->send(response);
  REQUIRE(status.ok);

  std::string line = harness.output.readLine();
  REQUIRE_FALSE(line.empty());
  CHECK(line.back() == '\n');

  line.pop_back();
  CHECK(line.find('\r') == std::string::npos);

  const auto decoded = mcp::v1::decode(line);
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().kind == mcp::v1::MessageKind::Response);
  CHECK(decoded.value().result.at("ok") == true);

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
}

TEST_CASE("stdio start and close are idempotent; send fails after close") {
  Harness harness;
  REQUIRE(harness.start().ok);
  CHECK(harness.transport->start().ok); // second start is a no-op

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());

  harness.transport->close();
  harness.transport->close(); // second close is a no-op

  const mcp::v1::Status status = harness.transport->send(
      mcp::v1::Json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", mcp::v1::Json::object()}});
  REQUIRE_FALSE(status.ok);
  CHECK(static_cast<int>(status.error.code) ==
        static_cast<int>(mcp::v1::ErrorCode::InternalError));
}

TEST_CASE("stdio close returns while the reader is blocked") {
  Harness harness;
  REQUIRE(harness.start().ok);

  // The write end stays open, so the reader is parked in fgets() on an empty
  // pipe. close() must still return promptly via CancelSynchronousIo.
  auto transport = harness.transport;
  auto closer = std::async(std::launch::async, [transport] {
    transport->close();
    return true;
  });

  REQUIRE(closer.wait_for(kWaitTimeout) == std::future_status::ready);
  CHECK(closer.get());
  CHECK(harness.collector.closeCount() == 1);

  // Closing again is safe and must not notify onClose a second time.
  harness.transport->close();
  CHECK(harness.collector.closeCount() == 1);
}

TEST_CASE("stdio sendRaw preserves bytes and fails after close") {
  Harness harness;
  REQUIRE(harness.start().ok);

  const std::string raw =
      R"({ "jsonrpc" : "2.0" , "id" : 1 , "method" : "ping" })";
  REQUIRE(harness.transport->sendRaw(raw).ok);

  std::string line = harness.output.readLine();
  REQUIRE(!line.empty());
  CHECK(line.back() == '\n');
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  CHECK(line == raw);

  const auto decoded = mcp::v1::decode(line);
  REQUIRE(decoded.hasValue());
  CHECK(decoded.value().method == "ping");

  harness.input.closeWrite();
  REQUIRE(harness.collector.waitForClose());
  harness.transport->close();

  const mcp::v1::Status after = harness.transport->sendRaw(kPingLine);
  CHECK_FALSE(after.ok);
}

TEST_CASE("FakeTransport does not notify after close") {
  mcp::v1::test::FakeTransport transport;
  int closes = 0;
  int messages = 0;
  int errors = 0;
  transport.setOnClose([&closes] { ++closes; });
  transport.setOnMessage([&messages](mcp::v1::Json) { ++messages; });
  transport.setOnError([&errors](mcp::v1::McpError) { ++errors; });

  transport.close();
  transport.emitClose();
  transport.deliver(mcp::v1::Json::object());
  transport.emitError(mcp::v1::McpError::make(
      mcp::v1::ErrorCode::InternalError, "late"));

  CHECK(closes == 0);
  CHECK(messages == 0);
  CHECK(errors == 0);
}