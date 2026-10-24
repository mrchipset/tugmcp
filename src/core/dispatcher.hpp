#pragma once

#include <optional>
#include <string>

#include <tugmcp/message.hpp>
#include <tugmcp/tool.hpp>

namespace mcp {
namespace v1 {

struct DispatchConfig {
  std::string serverName = "tugmcp";
  std::string serverVersion = "0.1.0";
};

// Internal M1 dispatcher. Stateless apart from the registry reference.
class Dispatcher {
public:
  Dispatcher(ToolRegistry& registry, DispatchConfig config);

  // Returns a response for requests, and std::nullopt for notifications or
  // messages that must not be answered.
  std::optional<Message> dispatch(const Message& request);

private:
  ToolRegistry& registry_;
  DispatchConfig config_;
};

} // namespace v1
} // namespace mcp
