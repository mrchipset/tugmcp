// Standalone single-header consumer for the core + stdio artifact.
//
// This TU owns the implementation. It must not include anything from the split
// library (no "src/" includes, no static library).
#define TUGMCP_IMPLEMENTATION
#include "tugmcp.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

  check(consumerSecondUnit() == 0, "second TU links and runs");

  if (failures != 0) {
    std::cerr << failures << " amalgamation consumer check(s) failed\n";
    return 1;
  }
  std::cout << "amalgamation consumer: all checks passed\n";
  return 0;
}
