// Standalone single-header consumer for the core + stdio artifact.
//
// This TU owns the implementation. It must not include anything from the split
// library (no "src/" includes, no static library).
#define TUGMCP_IMPLEMENTATION
#ifndef TUGMCP_AMALGAMATION_HEADER_NAME
#error "TUGMCP_AMALGAMATION_HEADER_NAME must name the generated header"
#endif
#include TUGMCP_AMALGAMATION_HEADER_NAME

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

// Defined in consumer_core_tu2.cpp, which includes the header without the
// implementation macro. Calling it proves the two TUs link without duplicate
// symbols.
int consumerSecondUnit();

namespace {

class EchoHandler final : public mcp::v1::IToolHandler {
public:
  mcp::v1::Result<mcp::v1::ToolOutcome> invoke(
      const mcp::v1::Json& arguments, mcp::v1::RequestContext&,
      mcp::v1::IProgressSink&) override {
    mcp::v1::ToolResult result;
    mcp::v1::ContentBlock block;
    block.type = mcp::v1::ContentType::Text;
    block.text = arguments.dump();
    result.content.push_back(std::move(block));
    return mcp::v1::ToolOutcome(std::move(result));
  }
};

class FakeTransport final : public mcp::v1::ITransport {
public:
  mcp::v1::Status start() override {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    cv_.notify_all();
    return mcp::v1::Status::success();
  }

  mcp::v1::Status send(const mcp::v1::Json& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sent_.push_back(message);
    cv_.notify_all();
    return mcp::v1::Status::success();
  }

  mcp::v1::Status sendTo(std::string_view,
                         const mcp::v1::Json& message) override {
    return send(message);
  }

  void setOnMessage(std::function<void(mcp::v1::Json)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(mcp::v1::McpError)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_close_ = std::move(handler);
  }

  void close() override {
    std::function<void()> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      handler = on_close_;
    }
    cv_.notify_all();
    if (handler) {
      handler();
    }
  }

  bool waitStarted(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return started_; });
  }

  bool push(mcp::v1::Json message) {
    std::function<void(mcp::v1::Json)> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || !on_message_) {
        return false;
      }
      handler = on_message_;
    }
    handler(std::move(message));
    return true;
  }

  std::optional<mcp::v1::Json> waitForResponse(
      const mcp::v1::Json& id, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool found = cv_.wait_for(lock, timeout, [this, &id] {
      return findResponseLocked(id).has_value();
    });
    if (!found) {
      return std::nullopt;
    }
    return findResponseLocked(id);
  }

private:
  std::optional<mcp::v1::Json> findResponseLocked(const mcp::v1::Json& id) const {
    for (const auto& message : sent_) {
      if (message.is_object() && message.contains("id") &&
          message.at("id") == id) {
        return message;
      }
    }
    return std::nullopt;
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool started_ = false;
  bool closed_ = false;
  std::vector<mcp::v1::Json> sent_;
  std::function<void(mcp::v1::Json)> on_message_;
  std::function<void(mcp::v1::McpError)> on_error_;
  std::function<void()> on_close_;
};

class ScriptedClientTransport final : public mcp::v1::ITransport {
public:
  mcp::v1::Status start() override { return mcp::v1::Status::success(); }

  mcp::v1::Status send(const mcp::v1::Json& message) override {
    std::function<void(mcp::v1::Json)> handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sent_.push_back(message);
      handler = on_message_;
    }
    if (!handler || !message.is_object()) {
      return mcp::v1::Status::success();
    }

    const std::string method = message.value("method", std::string{});
    if (method == "initialize" && message.contains("id")) {
      mcp::v1::Json response;
      response["jsonrpc"] = "2.0";
      response["id"] = message.at("id");
      response["result"] =
          mcp::v1::Json{{"protocolVersion", "2025-11-25"},
                        {"capabilities", mcp::v1::Json::object()},
                        {"serverInfo",
                         mcp::v1::Json{{"name", "scripted"},
                                       {"version", "1"}}}};
      handler(std::move(response));
    } else if (method == "tools/list" && message.contains("id")) {
      mcp::v1::Json response;
      response["jsonrpc"] = "2.0";
      response["id"] = message.at("id");
      response["result"] = mcp::v1::Json{{"tools", mcp::v1::Json::array()}};
      handler(std::move(response));
    }
    return mcp::v1::Status::success();
  }

  mcp::v1::Status sendTo(std::string_view,
                         const mcp::v1::Json& message) override {
    return send(message);
  }

  void setOnMessage(std::function<void(mcp::v1::Json)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(handler);
  }

  void setOnError(std::function<void(mcp::v1::McpError)> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(handler);
  }

  void setOnClose(std::function<void()> handler) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_close_ = std::move(handler);
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }

  bool sentMethod(std::string_view method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& message : sent_) {
      if (message.is_object() &&
          message.value("method", std::string{}) == method) {
        return true;
      }
    }
    return false;
  }

private:
  mutable std::mutex mutex_;
  std::vector<mcp::v1::Json> sent_;
  std::function<void(mcp::v1::Json)> on_message_;
  std::function<void(mcp::v1::McpError)> on_error_;
  std::function<void()> on_close_;
  bool closed_ = false;
};

#if defined(_WIN32)
bool makePipe(int fds[2]) { return _pipe(fds, 8192, _O_BINARY) == 0; }
FILE* openPipe(int fd, const char* mode) { return _fdopen(fd, mode); }
void closeDescriptor(int fd) { _close(fd); }
#else
bool makePipe(int fds[2]) { return ::pipe(fds) == 0; }
FILE* openPipe(int fd, const char* mode) { return ::fdopen(fd, mode); }
void closeDescriptor(int fd) { ::close(fd); }
#endif

bool writeAll(int fd, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
#if defined(_WIN32)
    const int written =
        _write(fd, bytes.data() + offset,
               static_cast<unsigned int>(bytes.size() - offset));
#else
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
#endif
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

int readSome(int fd, char* data, std::size_t size) {
#if defined(_WIN32)
  return _read(fd, data, static_cast<unsigned int>(size));
#else
  return static_cast<int>(::read(fd, data, size));
#endif
}

bool readLine(int fd, std::string& line) {
  line.clear();
  for (;;) {
    char byte = 0;
    if (readSome(fd, &byte, 1) <= 0) {
      return false;
    }
    line.push_back(byte);
    if (byte == '\n') {
      return true;
    }
  }
}

void runStdioRoundtrip(
    const std::function<void(bool, const char*)>& check) {
  using namespace std::chrono_literals;

  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  if (!makePipe(input_pipe) || !makePipe(output_pipe)) {
    check(false, "stdio pipes created");
    if (input_pipe[0] >= 0) {
      closeDescriptor(input_pipe[0]);
    }
    if (input_pipe[1] >= 0) {
      closeDescriptor(input_pipe[1]);
    }
    if (output_pipe[0] >= 0) {
      closeDescriptor(output_pipe[0]);
    }
    if (output_pipe[1] >= 0) {
      closeDescriptor(output_pipe[1]);
    }
    return;
  }

  std::FILE* input = openPipe(input_pipe[0], "rb");
  std::FILE* output = openPipe(output_pipe[1], "wb");
  check(input != nullptr && output != nullptr, "stdio FILE wrappers");
  if (input == nullptr || output == nullptr) {
    if (input != nullptr) {
      std::fclose(input);
    } else {
      closeDescriptor(input_pipe[0]);
    }
    if (output != nullptr) {
      std::fclose(output);
    } else {
      closeDescriptor(output_pipe[1]);
    }
    closeDescriptor(input_pipe[1]);
    closeDescriptor(output_pipe[0]);
    return;
  }

  mcp::v1::StdioOptions stdio_options;
  stdio_options.input = input;
  stdio_options.output = output;
  mcp::v1::StdioTransport stdio(stdio_options);

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<mcp::v1::Json> received;
  std::optional<mcp::v1::McpError> stdio_error;
  bool closed = false;
  stdio.setOnMessage([&](mcp::v1::Json message) {
    std::lock_guard<std::mutex> lock(mutex);
    received.push_back(std::move(message));
    cv.notify_all();
  });
  stdio.setOnError([&](mcp::v1::McpError error) {
    std::lock_guard<std::mutex> lock(mutex);
    stdio_error = std::move(error);
    cv.notify_all();
  });
  stdio.setOnClose([&] {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    cv.notify_all();
  });

  check(stdio.start().ok, "StdioTransport::start");
  const std::string request =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
  check(writeAll(input_pipe[1], request), "write framed stdio request");

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, 5s, [&] {
      return !received.empty() || stdio_error.has_value();
    });
  }
  check(!stdio_error.has_value(), "StdioTransport reported no error");
  check(received.size() == 1, "StdioTransport delivered one frame");
  if (!received.empty()) {
    check(received.front().value("method", std::string{}) == "ping",
          "StdioTransport decoded request method");
  }

  mcp::v1::Json response;
  response["jsonrpc"] = "2.0";
  response["id"] = 1;
  response["result"] = mcp::v1::Json{{"ok", true}};
  check(stdio.send(response).ok, "StdioTransport::send response");

  std::string line;
  check(readLine(output_pipe[0], line), "read framed stdio response");
  const auto decoded = mcp::v1::decode(line);
  check(decoded.hasValue(), "decode stdio response");
  if (decoded.hasValue()) {
    check(decoded.value().kind == mcp::v1::MessageKind::Response,
          "stdio response kind");
    check(decoded.value().result.value("ok", false),
          "stdio response result");
  }

  closeDescriptor(input_pipe[1]);
  input_pipe[1] = -1;
  stdio.close();
  {
    std::lock_guard<std::mutex> lock(mutex);
    check(closed, "StdioTransport close notified");
  }
  std::fclose(input);
  std::fclose(output);
  closeDescriptor(output_pipe[0]);
  input_pipe[0] = -1;
  output_pipe[0] = -1;
  output_pipe[1] = -1;
}

bool hasTool(const mcp::v1::Json& response, const std::string& name) {
  if (!response.is_object() || !response.contains("result")) {
    return false;
  }
  const auto& result = response.at("result");
  if (!result.is_object() || !result.contains("tools") ||
      !result.at("tools").is_array()) {
    return false;
  }
  for (const auto& tool : result.at("tools")) {
    if (tool.is_object() && tool.value("name", std::string{}) == name) {
      return true;
    }
  }
  return false;
}

} // namespace

int main() {
  using namespace std::chrono_literals;
  int failures = 0;
  const auto check = [&failures](bool condition, const char* what) {
    if (!condition) {
      std::cerr << "FAIL: " << what << '\n';
      ++failures;
    }
  };

  // Codec roundtrip.
  mcp::v1::Message request;
  request.kind = mcp::v1::MessageKind::Request;
  request.id.value = std::int64_t{42};
  request.method = "tools/list";
  request.params = mcp::v1::Json::object();
  const auto encoded = mcp::v1::encode(request);
  check(encoded.hasValue(), "encode(request)");
  if (encoded.hasValue()) {
    const auto decoded = mcp::v1::decode(encoded.value());
    check(decoded.hasValue(), "decode(encoded request)");
    if (decoded.hasValue()) {
      check(decoded.value().kind == mcp::v1::MessageKind::Request,
            "decoded message kind");
      check(decoded.value().method == "tools/list", "decoded method");
      check(decoded.value().id == request.id, "decoded request id");
    }
  }

  // ToolRegistry register/find.
  mcp::v1::ToolRegistry registry;
  mcp::v1::ToolDescriptor descriptor;
  descriptor.name = "echo";
  descriptor.description = "Echoes its arguments";
  const auto added = registry.add(
      descriptor, std::make_shared<EchoHandler>());
  check(added.ok, "ToolRegistry::add");
  const auto found = registry.find("echo");
  check(found.has_value() && found->name == "echo", "ToolRegistry::find");
  check(registry.size() == 1, "ToolRegistry::size");

  // Thread pool post/drain.
  const auto executor = mcp::v1::makeThreadPoolExecutor(2, 32);
  std::atomic<int> ran{0};
  for (int index = 0; index < 16; ++index) {
    const auto submitted = executor->post([&ran] { ran.fetch_add(1); });
    check(static_cast<bool>(submitted), "executor post");
  }
  executor->drain();
  check(ran.load() == 16, "executor drained all tasks");
  const auto rejected = executor->post([] {});
  check(!rejected, "executor rejects after drain");

  // Minimal McpServer + in-test transport (initialize + tools/list).
  auto transport = std::make_shared<FakeTransport>();
  mcp::v1::ServerOptions options;
  options.name = "amalgamation-consumer";
  options.useInlineExecutor = true;
  options.allowUnvalidatedTools = true;
  mcp::v1::McpServer server(options, transport);
  check(server.tools().add(descriptor, std::make_shared<EchoHandler>()).ok,
        "server tool registration");

  std::thread server_thread([&server] { (void)server.run(); });
  check(transport->waitStarted(5s), "server transport started");

  const mcp::v1::Json init_id = 1;
  mcp::v1::Json init_request;
  init_request["jsonrpc"] = "2.0";
  init_request["id"] = init_id;
  init_request["method"] = "initialize";
  init_request["params"] = mcp::v1::Json{
      {"protocolVersion", "2025-11-25"},
      {"capabilities", mcp::v1::Json::object()},
      {"clientInfo", mcp::v1::Json{{"name", "consumer"}, {"version", "1"}}}};
  check(transport->push(std::move(init_request)), "push initialize");
  const auto init_response = transport->waitForResponse(init_id, 5s);
  check(init_response.has_value(), "initialize response");
  if (init_response.has_value()) {
    check(!init_response->contains("error"), "initialize has no error");
    check(init_response->value("result", mcp::v1::Json::object())
              .value("protocolVersion", std::string{}) == "2025-11-25",
          "initialize protocol version");
  }

  const mcp::v1::Json list_id = 2;
  mcp::v1::Json list_request;
  list_request["jsonrpc"] = "2.0";
  list_request["id"] = list_id;
  list_request["method"] = "tools/list";
  list_request["params"] = mcp::v1::Json::object();
  check(transport->push(std::move(list_request)), "push tools/list");
  const auto list_response = transport->waitForResponse(list_id, 5s);
  check(list_response.has_value(), "tools/list response");
  if (list_response.has_value()) {
    check(!list_response->contains("error"), "tools/list has no error");
    check(hasTool(*list_response, "echo"), "tools/list contains echo");
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  // StdioTransport framing roundtrip over real OS pipes.
  runStdioRoundtrip(check);

  // McpClient public API over a scripted in-test transport.
  auto client_transport = std::make_shared<ScriptedClientTransport>();
  mcp::v1::ClientOptions client_options;
  client_options.protocolVersion = "2025-11-25";
  client_options.autoOpenStream = false;
  mcp::v1::McpClient client(client_options, client_transport);
  const auto connected = client.connect();
  check(connected.ok, "McpClient::connect");
  if (connected.ok) {
    const auto tools = client.listTools();
    check(tools.hasValue(), "McpClient::listTools");
    if (tools.hasValue()) {
      check(tools.value().contains("tools") &&
                tools.value().at("tools").is_array(),
            "McpClient::listTools result shape");
    }
  }
  client.close();
  check(client_transport->sentMethod("initialize"),
        "McpClient sent initialize");
  check(client_transport->sentMethod("tools/list"),
        "McpClient sent tools/list");

  check(consumerSecondUnit() == 0, "second TU links and runs");

  if (failures != 0) {
    std::cerr << failures << " amalgamation consumer check(s) failed\n";
    return 1;
  }
  std::cout << "amalgamation consumer: all checks passed\n";
  return 0;
}
