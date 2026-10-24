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
#include <tugmcp/schema.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

struct ContentBlock {
  std::string type = "text";
  std::string text;
};

// Tool names must not contain '#': the registry derives the output-schema id
// as "<name>#output". An empty inputSchema object means "no schema declared";
// an explicit null is rejected at registration.
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
  // Installs the schema validation policy. A standalone registry stays
  // permissive until this is called; McpServer always installs the policy from
  // ServerOptions at construction, which is what makes the server fail closed.
  void setValidationPolicy(SchemaValidatorPtr validator,
                           bool allowUnvalidatedTools);

  Status add(ToolDescriptor descriptor, ToolHandlerPtr handler);

  std::optional<ToolDescriptor> find(std::string_view name) const;
  ToolHandlerPtr handler(std::string_view name) const;
  std::vector<ToolDescriptor> list() const;
  std::size_t size() const;

  // Validate `arguments` against the tool's inputSchema. Returns success when
  // the tool has no schema or no validator is configured. On a validation
  // failure the returned error is InvalidParams with data.issues.
  Status validateInput(std::string_view name, const Json& arguments) const;

  // Validate `structured` against the tool's outputSchema. Same success rules
  // as validateInput; used to turn protocol errors into tool errors.
  Status validateOutput(std::string_view name, const Json& structured) const;

private:
  struct Entry {
    ToolDescriptor descriptor;
    ToolHandlerPtr handler;
  };

  Status validateWith(std::string_view schemaId, const Json& instance,
                      std::string_view what,
                      const SchemaValidatorPtr& validator) const;

  mutable std::mutex mutex_;
  std::map<std::string, Entry> tools_;
  SchemaValidatorPtr validator_;
  bool allow_unvalidated_tools_ = false;
  bool validation_policy_set_ = false;
};

} // namespace v1
} // namespace mcp
