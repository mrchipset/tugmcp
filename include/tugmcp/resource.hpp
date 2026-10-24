#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <tugmcp/error.hpp>
#include <tugmcp/operation.hpp>
#include <tugmcp/request_context.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

struct ResourceDescriptor {
  std::string uri;
  std::string name;
  std::string title;
  std::string description;
  std::string mimeType;
  std::uint64_t size = 0;
  Json annotations;
};

struct ResourceTemplate {
  std::string uriTemplate;
  std::string name;
  std::string description;
  std::string mimeType;
};

struct ResourceContent {
  std::string uri;
  std::string mimeType;
  Json text;
  Json blob;
  ResultType resultType = ResultType::Complete;
  std::uint32_t ttlMs = 0;
  std::string cacheScope;
};

using ResourceOutcome = OperationOutcome<ResourceContent>;

// A single provider owns the resource namespace for a server. Providers own
// both pagination (cursor semantics) and URI authorization.
class IResourceProvider {
public:
  virtual ~IResourceProvider() = default;
  virtual Result<Page<ResourceDescriptor>> list(const ListParams& params,
                                                RequestContext& context) = 0;
  virtual Result<Page<ResourceTemplate>> templates(const ListParams& params,
                                                   RequestContext& context) = 0;
  virtual Result<ResourceOutcome> read(std::string_view uri,
                                       RequestContext& context) = 0;
};

} // namespace v1
} // namespace mcp
