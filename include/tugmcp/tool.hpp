#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <tugmcp/error.hpp>
#include <tugmcp/request_context.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

struct ContentBlock {
  std::string type = "text";
  std::string text;
};

struct ToolDescriptor {
  std::string name;
  std::string title;
  std::string description;
  Json inputSchema = Json::object();
  Json outputSchema;
  Json annotations;
  bool idempotent = false;
};

struct ToolResult {
  bool isError = false;
  std::vector<ContentBlock> content;
  Json structuredContent;
};

class IToolHandler {
public:
  virtual ~IToolHandler() = default;
  virtual Result<ToolResult> call(const Json& arguments,
                                  RequestContext& context) = 0;
};

using ToolHandlerPtr = std::shared_ptr<IToolHandler>;

class ToolRegistry {
public:
  Status add(ToolDescriptor descriptor, ToolHandlerPtr handler);

  std::optional<ToolDescriptor> find(std::string_view name) const;
  ToolHandlerPtr handler(std::string_view name) const;
  std::vector<ToolDescriptor> list() const;
  std::size_t size() const;

private:
  struct Entry {
    ToolDescriptor descriptor;
    ToolHandlerPtr handler;
  };

  mutable std::mutex mutex_;
  std::map<std::string, Entry> tools_;
};

} // namespace v1
} // namespace mcp
