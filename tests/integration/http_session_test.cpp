// 2025 Streamable HTTP sessions: header lifecycle, GET replay, server requests.

#include <doctest/doctest.h>

#include "tugmcp/detail/http_httplib.hpp"

#include <tugmcp/server.hpp>
#include <tugmcp/transport/http.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using mcp::v1::ErrorCode;
using mcp::v1::HttpServerTransport;
using mcp::v1::ITimeSource;
using mcp::v1::ITransport;
using mcp::v1::Json;
using mcp::v1::McpServer;
using mcp::v1::MessageHandlerEx;
using mcp::v1::RequestId;
using mcp::v1::ServerOptions;
using mcp::v1::Status;
using mcp::v1::StreamableHttpServerOptions;
using mcp::v1::makeStreamableHttpServerTransport;

namespace {

std::string baseUrlFor(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

class ReadyTransport final : public ITransport {
public:
  explicit ReadyTransport(std::shared_ptr<ITransport> inner)
      : inner_(std::move(inner)) {}

  Status start() override {
    Status status = inner_->start();
    if (status && !signaled_.exchange(true)) {
      ready_.set_value();
    }
    return status;
  }

  Status send(const Json& message) override { return inner_->send(message); }
  Status sendTo(std::string_view route, const Json& message) override {
    return inner_->sendTo(route, message);
  }
  Status sendToSession(std::string_view id, const Json& message) override {
    return inner_->sendToSession(id, message);
  }
  void setOnMessage(std::function<void(Json)> handler) override {
    inner_->setOnMessage(std::move(handler));
  }
  void setOnMessageEx(MessageHandlerEx handler) override {
    inner_->setOnMessageEx(std::move(handler));
  }
  void setOnError(std::function<void(mcp::v1::McpError)> handler) override {
    inner_->setOnError(std::move(handler));
  }
  void setOnClose(std::function<void()> handler) override {
    inner_->setOnClose(std::move(handler));
  }
  void setOnRouteClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnRouteClosed(std::move(handler));
  }
  void setOnSessionClosed(
      std::function<void(const std::string&)> handler) override {
    inner_->setOnSessionClosed(std::move(handler));
  }
  void close() override { inner_->close(); }
  void waitReady() { ready_.get_future().wait(); }

private:
  std::shared_ptr<ITransport> inner_;
  std::promise<void> ready_;
  std::atomic<bool> signaled_{false};
};

class RunningServer {
public:
  explicit RunningServer(StreamableHttpServerOptions http_options) {
    http_options.bind = "127.0.0.1";
    http_options.port = 0;
    auto created = makeStreamableHttpServerTransport(http_options);
    REQUIRE(created.hasValue());
    concrete_ = std::dynamic_pointer_cast<HttpServerTransport>(created.value());
    REQUIRE(concrete_ != nullptr);
    transport_ = std::make_shared<ReadyTransport>(created.value());
    server_ = std::make_unique<McpServer>(ServerOptions{}, transport_);
  }

  ~RunningServer() { stop(); }

  McpServer& server() { return *server_; }

  void start() {
    thread_ = std::thread([this] { server_->run(); });
    transport_->waitReady();
  }

  void stop() {
    if (!server_) {
      return;
    }
    server_->stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  int port() const { return concrete_->boundPort(); }

private:
  std::shared_ptr<HttpServerTransport> concrete_;
  std::shared_ptr<ReadyTransport> transport_;
  std::unique_ptr<McpServer> server_;
  std::thread thread_;
};

class ManualTimeSource final : public ITimeSource {
public:
  std::chrono::steady_clock::time_point now() override {
    return base_ + std::chrono::milliseconds(offset_.load());
  }

  void advance(std::chrono::milliseconds delta) {
    offset_.fetch_add(delta.count());
  }

private:
  std::chrono::steady_clock::time_point base_ =
      std::chrono::steady_clock::now();
  std::atomic<std::int64_t> offset_{0};
};

httplib::Headers sessionHeaders(const std::string& session_id) {
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("MCP-Session-Id", session_id);
  headers.emplace("Accept", "application/json");
  return headers;
}

Json initializeBody(int id = 1) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "initialize"},
              {"params", Json{{"protocolVersion", "2025-11-25"}}}};
}

std::string initializeSession(int port) {
  httplib::Client client(baseUrlFor(port));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("Accept", "application/json");
  auto response = client.Post("/mcp", headers, initializeBody().dump(),
                              "application/json");
  REQUIRE(response);
  REQUIRE(response->status == 200);
  const std::string session_id =
      response->get_header_value("MCP-Session-Id");
  REQUIRE(session_id.size() == 32);
  for (const char character : session_id) {
    CHECK(((character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f')));
  }
  client.stop();
  return session_id;
}

struct Reader {
  std::mutex mutex;
  std::condition_variable cv;
  std::string data;
  std::thread thread;
};

void startReader(int port, httplib::Headers headers, Reader& reader) {
  reader.thread = std::thread([port, headers, &reader] {
    httplib::Client client(baseUrlFor(port));
    client.set_read_timeout(std::chrono::seconds(10));
    client.Get("/mcp", headers, [&reader](const char* data, std::size_t size) {
      std::lock_guard<std::mutex> lock(reader.mutex);
      reader.data.append(data, size);
      reader.cv.notify_all();
      return true;
    });
    client.stop();
  });
}

bool waitForText(Reader& reader, const std::string& needle,
                 std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(reader.mutex);
  return reader.cv.wait_for(lock, timeout, [&] {
    return reader.data.find(needle) != std::string::npos;
  });
}

std::string readerSnapshot(Reader& reader) {
  std::lock_guard<std::mutex> lock(reader.mutex);
  return reader.data;
}

std::vector<Json> parseSse(const std::string& body) {
  std::vector<Json> frames;
  std::size_t offset = 0;
  while (offset < body.size()) {
    const std::size_t end = body.find("\n\n", offset);
    const std::string frame =
        body.substr(offset, end == std::string::npos ? std::string::npos
                                                     : end - offset);
    const std::size_t data_pos = frame.find("data: ");
    if (data_pos != std::string::npos) {
      const std::size_t line_end = frame.find('\n', data_pos);
      const std::string data =
          frame.substr(data_pos + 6,
                       line_end == std::string::npos
                           ? std::string::npos
                           : line_end - (data_pos + 6));
      if (!data.empty()) {
        frames.push_back(Json::parse(data));
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 2;
  }
  return frames;
}

bool ensureStreamOpen(McpServer& server, const std::string& session_id) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server
            .sendNotification(session_id, "notifications/message",
                              Json{{"level", "info"}, {"data", "ready"}})
            .ok) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

} // namespace

TEST_CASE("2025 initialize creates a session and authenticated POST works") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("Accept", "application/json");
  auto initialized = client.Post("/mcp", headers, initializeBody().dump(),
                                 "application/json");
  REQUIRE(initialized);
  REQUIRE(initialized->status == 200);
  const std::string session_id =
      initialized->get_header_value("MCP-Session-Id");
  REQUIRE(session_id.size() == 32);
  const Json init_result = Json::parse(initialized->body);
  CHECK(init_result.at("result").at("protocolVersion") == "2025-11-25");

  const Json list_body = Json{{"jsonrpc", "2.0"},
                              {"id", 2},
                              {"method", "tools/list"},
                              {"params", Json::object()}};
  auto listed = client.Post("/mcp", sessionHeaders(session_id),
                            list_body.dump(), "application/json");
  REQUIRE(listed);
  CHECK(listed->status == 200);
  CHECK(Json::parse(listed->body).at("result").contains("tools"));

  client.stop();
  running.stop();
}

TEST_CASE("2025 POST requires a valid session header") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 7},
                         {"method", "tools/list"},
                         {"params", Json::object()}};

  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  auto missing = client.Post("/mcp", headers, body.dump(),
                             "application/json");
  REQUIRE(missing);
  CHECK(missing->status == 400);
  CHECK(Json::parse(missing->body).at("error").at("data").at("reason") ==
        "missing_session");

  headers.emplace("MCP-Session-Id", "deadbeef");
  auto unknown = client.Post("/mcp", headers, body.dump(),
                             "application/json");
  REQUIRE(unknown);
  CHECK(unknown->status == 404);

  client.stop();
  running.stop();
}

TEST_CASE("DELETE closes the session and later POST is unknown") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();
  const std::string session_id = initializeSession(running.port());

  httplib::Client client(baseUrlFor(running.port()));
  auto removed = client.Delete("/mcp", sessionHeaders(session_id));
  REQUIRE(removed);
  CHECK(removed->status == 204);

  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 9},
                         {"method", "tools/list"},
                         {"params", Json::object()}};
  auto after = client.Post("/mcp", sessionHeaders(session_id), body.dump(),
                           "application/json");
  REQUIRE(after);
  CHECK(after->status == 404);

  client.stop();
  running.stop();
}

TEST_CASE("GET stream delivers notifications and replays by Last-Event-ID") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();
  const std::string session_id = initializeSession(running.port());

  httplib::Headers headers = sessionHeaders(session_id);
  Reader first;
  startReader(running.port(), headers, first);
  REQUIRE(ensureStreamOpen(running.server(), session_id));
  REQUIRE(waitForText(first, "notifications/message",
                      std::chrono::seconds(2)));
  CHECK(waitForText(first, "id: 1", std::chrono::seconds(1)));

  httplib::Headers replay_zero = headers;
  replay_zero.emplace("Last-Event-ID", "0");
  Reader second;
  startReader(running.port(), replay_zero, second);
  REQUIRE(waitForText(second, "notifications/message",
                      std::chrono::seconds(2)));
  REQUIRE(waitForText(second, "id: 1", std::chrono::seconds(1)));
  first.thread.join();

  httplib::Headers replay_one = headers;
  replay_one.emplace("Last-Event-ID", "1");
  Reader third;
  startReader(running.port(), replay_one, third);
  REQUIRE(ensureStreamOpen(running.server(), session_id));
  REQUIRE(waitForText(third, "id: 2", std::chrono::seconds(2)));
  CHECK(readerSnapshot(third).find("id: 1") == std::string::npos);
  second.thread.join();

  running.stop();
  if (third.thread.joinable()) {
    third.thread.join();
  }
}

TEST_CASE("requestClient round-trips a response posted by the client") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();
  const std::string session_id = initializeSession(running.port());

  httplib::Headers headers = sessionHeaders(session_id);
  Reader reader;
  startReader(running.port(), headers, reader);
  REQUIRE(ensureStreamOpen(running.server(), session_id));

  auto future = std::async(std::launch::async, [&] {
    return running.server().requestClient(session_id, "roots/list",
                                          Json::object(),
                                          std::chrono::seconds(2));
  });
  REQUIRE(waitForText(reader, "roots/list", std::chrono::seconds(2)));

  Json server_request;
  for (const auto& frame : parseSse(readerSnapshot(reader))) {
    if (frame.value("method", std::string{}) == "roots/list") {
      server_request = frame;
      break;
    }
  }
  REQUIRE(server_request.contains("id"));

  httplib::Client client(baseUrlFor(running.port()));
  const Json response = Json{{"jsonrpc", "2.0"},
                             {"id", server_request.at("id")},
                             {"result", Json{{"roots", Json::array()}}}};
  auto posted = client.Post("/mcp", headers, response.dump(),
                            "application/json");
  REQUIRE(posted);
  CHECK(posted->status == 202);

  auto result = future.get();
  REQUIRE(result.hasValue());
  CHECK(result.value().at("roots").is_array());

  auto timed_out = running.server().requestClient(
      session_id, "roots/list", Json::object(),
      std::chrono::milliseconds(100));
  REQUIRE_FALSE(timed_out.hasValue());
  CHECK(timed_out.error().data.at("reason") == "timeout");

  client.stop();
  running.stop();
  if (reader.thread.joinable()) {
    reader.thread.join();
  }
}

TEST_CASE("2026 GET and DELETE remain 405") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2026-07-28");
  auto get = client.Get("/mcp", headers);
  REQUIRE(get);
  CHECK(get->status == 405);
  auto remove = client.Delete("/mcp", headers);
  REQUIRE(remove);
  CHECK(remove->status == 405);
  client.stop();
  running.stop();
}

TEST_CASE("maxSessions rejects extra initialize requests") {
  StreamableHttpServerOptions options;
  options.maxSessions = 1;
  RunningServer running(options);
  running.start();
  initializeSession(running.port());

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("MCP-Protocol-Version", "2025-11-25");
  headers.emplace("Accept", "application/json");
  auto second = client.Post("/mcp", headers, initializeBody(2).dump(),
                            "application/json");
  REQUIRE(second);
  CHECK(second->status == 429);
  client.stop();
  running.stop();
}

TEST_CASE("expired sessions return 404") {
  auto time_source = std::make_shared<ManualTimeSource>();
  StreamableHttpServerOptions options;
  options.timeSource = time_source;
  options.sessionTimeout = std::chrono::milliseconds(100);
  RunningServer running(options);
  running.start();
  const std::string session_id = initializeSession(running.port());

  time_source->advance(std::chrono::milliseconds(200));

  httplib::Client client(baseUrlFor(running.port()));
  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 11},
                         {"method", "tools/list"},
                         {"params", Json::object()}};
  auto expired = client.Post("/mcp", sessionHeaders(session_id), body.dump(),
                             "application/json");
  REQUIRE(expired);
  CHECK(expired->status == 404);
  client.stop();
  running.stop();
}
TEST_CASE("failed session send is not replayed after opening a stream") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();
  const std::string session_id = initializeSession(running.port());

  REQUIRE_FALSE(running.server()
                    .sendNotification(session_id, "notifications/message",
                                      Json{{"data", "failed-event"}})
                    .ok);

  httplib::Headers headers = sessionHeaders(session_id);
  Reader reader;
  startReader(running.port(), headers, reader);
  REQUIRE(ensureStreamOpen(running.server(), session_id));
  REQUIRE(waitForText(reader, "notifications/message",
                      std::chrono::seconds(2)));
  CHECK(readerSnapshot(reader).find("failed-event") == std::string::npos);

  running.stop();
  if (reader.thread.joinable()) {
    reader.thread.join();
  }
}

TEST_CASE("initialize without version header still creates a session") {
  RunningServer running(StreamableHttpServerOptions{});
  running.start();

  httplib::Client client(baseUrlFor(running.port()));
  httplib::Headers headers;
  headers.emplace("Accept", "application/json");
  auto initialized = client.Post("/mcp", headers, initializeBody().dump(),
                                 "application/json");
  REQUIRE(initialized);
  REQUIRE(initialized->status == 200);
  const std::string session_id =
      initialized->get_header_value("MCP-Session-Id");
  REQUIRE(session_id.size() == 32);

  const Json body = Json{{"jsonrpc", "2.0"},
                         {"id", 3},
                         {"method", "tools/list"},
                         {"params", Json::object()}};
  auto missing = client.Post("/mcp", headers, body.dump(),
                             "application/json");
  REQUIRE(missing);
  CHECK(missing->status == 400);
  CHECK(Json::parse(missing->body).at("error").at("data").at("reason") ==
        "missing_session");

  client.stop();
  running.stop();
}
