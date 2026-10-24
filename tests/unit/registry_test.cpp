#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <tugmcp/tool.hpp>

using mcp::v1::ContentBlock;
using mcp::v1::ErrorCode;
using mcp::v1::IProgressSink;
using mcp::v1::IToolHandler;
using mcp::v1::Json;
using mcp::v1::RequestContext;
using mcp::v1::Result;
using mcp::v1::Status;
using mcp::v1::ToolDescriptor;
using mcp::v1::ToolHandlerPtr;
using mcp::v1::ToolOutcome;
using mcp::v1::ToolRegistry;
using mcp::v1::ToolResult;

namespace {

class NamedHandler final : public IToolHandler {
public:
  explicit NamedHandler(std::string name) : name_(std::move(name)) {}

  Result<ToolOutcome> invoke(const Json&, RequestContext&,
                             IProgressSink&) override {
    ToolResult result;
    result.content.push_back(ContentBlock{"text", name_});
    return ToolOutcome(std::move(result));
  }

private:
  std::string name_;
};

ToolDescriptor descriptor(const std::string& name) {
  ToolDescriptor value;
  value.name = name;
  value.title = name + " title";
  value.description = name + " description";
  value.inputSchema = Json{{"type", "object"}};
  return value;
}

} // namespace

TEST_CASE("ToolRegistry add/find/handler/size") {
  ToolRegistry registry;
  auto handler = std::make_shared<NamedHandler>("echo");
  auto status = registry.add(descriptor("echo"), handler);
  REQUIRE(status.ok);
  CHECK(registry.size() == 1);

  auto found = registry.find("echo");
  REQUIRE(found.has_value());
  CHECK(found->name == "echo");
  CHECK(found->title == "echo title");

  ToolHandlerPtr resolved = registry.handler("echo");
  REQUIRE(resolved != nullptr);
  CHECK(resolved.get() == handler.get());
}

TEST_CASE("ToolRegistry rejects duplicate names") {
  ToolRegistry registry;
  REQUIRE(registry.add(descriptor("echo"),
                       std::make_shared<NamedHandler>("one"))
              .ok);

  Status duplicate = registry.add(descriptor("echo"),
                                  std::make_shared<NamedHandler>("two"));
  REQUIRE_FALSE(duplicate.ok);
  CHECK(static_cast<int>(duplicate.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(duplicate.error.message == "duplicate tool: echo");
  CHECK(registry.size() == 1);
}

TEST_CASE("ToolRegistry list is sorted by name") {
  ToolRegistry registry;
  REQUIRE(registry.add(descriptor("zeta"),
                       std::make_shared<NamedHandler>("zeta"))
              .ok);
  REQUIRE(registry.add(descriptor("alpha"),
                       std::make_shared<NamedHandler>("alpha"))
              .ok);
  REQUIRE(registry.add(descriptor("middle"),
                       std::make_shared<NamedHandler>("middle"))
              .ok);

  const auto tools = registry.list();
  REQUIRE(tools.size() == 3);
  CHECK(tools[0].name == "alpha");
  CHECK(tools[1].name == "middle");
  CHECK(tools[2].name == "zeta");
}

TEST_CASE("ToolRegistry returns nullopt for missing tools") {
  ToolRegistry registry;
  CHECK_FALSE(registry.find("missing").has_value());
  CHECK(registry.handler("missing") == nullptr);
  CHECK(registry.size() == 0);
}

TEST_CASE("ToolRegistry rejects empty names and null handlers") {
  ToolRegistry registry;

  Status empty_name =
      registry.add(descriptor(""), std::make_shared<NamedHandler>("x"));
  CHECK_FALSE(empty_name.ok);
  CHECK(static_cast<int>(empty_name.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));

  Status null_handler = registry.add(descriptor("x"), nullptr);
  CHECK_FALSE(null_handler.ok);
  CHECK(static_cast<int>(null_handler.error.code) ==
        static_cast<int>(ErrorCode::InvalidParams));
  CHECK(registry.size() == 0);
}

TEST_CASE("ToolRegistry concurrent add and lookup is safe") {
  ToolRegistry registry;
  static constexpr int kThreads = 8;
  static constexpr int kPerThread = 5;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  std::atomic<bool> failed{false};
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&registry, &failed, thread] {
      for (int index = 0; index < kPerThread; ++index) {
        const std::string name =
            "tool_" + std::to_string(thread) + "_" + std::to_string(index);
        if (!registry.add(descriptor(name),
                          std::make_shared<NamedHandler>(name))
                 .ok) {
          failed.store(true);
        }
        if (!registry.find(name).has_value()) {
          failed.store(true);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_FALSE(failed.load());
  CHECK(registry.size() ==
        static_cast<std::size_t>(kThreads * kPerThread));
  const auto tools = registry.list();
  REQUIRE(tools.size() == static_cast<std::size_t>(kThreads * kPerThread));
  for (std::size_t index = 1; index < tools.size(); ++index) {
    CHECK(tools[index - 1].name < tools[index].name);
  }
}
