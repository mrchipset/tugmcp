#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <tugmcp/error.hpp>
#include <tugmcp/operation.hpp>
#include <tugmcp/request_context.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

struct PromptDescriptor {
  std::string name;
  std::string description;
  Json arguments;
};

struct PromptResult {
  std::string description;
  Json messages;
  ResultType resultType = ResultType::Complete;
};

using PromptOutcome = OperationOutcome<PromptResult>;

class ICompletionProvider {
public:
  virtual ~ICompletionProvider() = default;
  virtual Result<Json> complete(const Json& ref, const Json& argument,
                              RequestContext& context) = 0;
};

class IPromptProvider {
public:
  virtual ~IPromptProvider() = default;
  virtual Result<Page<PromptDescriptor>> list(const ListParams& params,
                                              RequestContext& context) = 0;
  virtual Result<PromptOutcome> get(std::string_view name, const Json& args,
                                    RequestContext& context) = 0;
};

} // namespace v1
} // namespace mcp
