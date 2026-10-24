#include "tugmcp/tool.hpp"

#include <string>
#include <utility>

namespace mcp {
namespace v1 {

Status ToolRegistry::add(ToolDescriptor descriptor, ToolHandlerPtr handler) {
  if (descriptor.name.empty()) {
    return Status::failure(
        McpError::make(ErrorCode::InvalidParams, "tool name must not be empty"));
  }
  if (!handler) {
    return Status::failure(McpError::make(
        ErrorCode::InvalidParams,
        "tool handler must not be null: " + descriptor.name));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (tools_.find(descriptor.name) != tools_.end()) {
    return Status::failure(McpError::make(
        ErrorCode::InvalidParams, "duplicate tool: " + descriptor.name));
  }

  Entry entry;
  entry.descriptor = std::move(descriptor);
  entry.handler = std::move(handler);
  tools_.emplace(entry.descriptor.name, std::move(entry));
  return Status::success();
}

std::optional<ToolDescriptor> ToolRegistry::find(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = tools_.find(std::string(name));
  if (it == tools_.end()) {
    return std::nullopt;
  }
  return it->second.descriptor;
}

ToolHandlerPtr ToolRegistry::handler(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = tools_.find(std::string(name));
  if (it == tools_.end()) {
    return nullptr;
  }
  return it->second.handler;
}

std::vector<ToolDescriptor> ToolRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolDescriptor> result;
  result.reserve(tools_.size());
  for (const auto& item : tools_) {
    result.push_back(item.second.descriptor);
  }
  return result;
}

std::size_t ToolRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tools_.size();
}

} // namespace v1
} // namespace mcp
