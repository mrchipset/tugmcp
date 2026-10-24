#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <tugmcp/message.hpp>
#include <tugmcp/prompt.hpp>
#include <tugmcp/resource.hpp>
#include <tugmcp/tool.hpp>

namespace mcp {
namespace v1 {

struct DispatchConfig {
  std::string serverName = "tugmcp";
  std::string serverVersion = "0.1.0";
  std::string instructions;
  std::vector<std::string> supportedVersions{"2026-07-28", "2025-11-25"};
  std::size_t pageSize = 100; // 0 disables pagination (tests)
};

using NotificationSink = std::function<void(Json)>;

// Internal M1 dispatcher. Stateless apart from the registry/provider
// references and static configuration.
class Dispatcher {
public:
  Dispatcher(ToolRegistry& registry,
             IResourceProvider* resources = nullptr,
             IPromptProvider* prompts = nullptr,
             DispatchConfig config = {});

  // Returns a response for requests, and std::nullopt for notifications or
  // messages that must not be answered. An empty protocolVersion resolves to
  // the 2025-11-25 era.
  std::optional<Message> dispatch(
      const Message& request, const std::string& protocolVersion = {},
      const NotificationSink& notify = {},
      std::shared_ptr<CancellationToken> token = nullptr);

private:
  ToolRegistry& registry_;
  // Providers are borrowed for the duration of run(). They are non-const
  // because list/read may advance provider-owned pagination state.
  IResourceProvider* resources_;
  IPromptProvider* prompts_;
  DispatchConfig config_;
};

} // namespace v1
} // namespace mcp
