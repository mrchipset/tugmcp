#include "tugmcp/tool.hpp"

#include <string>
#include <utility>

namespace mcp {
namespace v1 {
namespace {

constexpr const char* kOutputSchemaSuffix = "#output";

std::string outputSchemaId(std::string_view name) {
  return std::string(name) + kOutputSchemaSuffix;
}

Status reportFailure(const char* what, std::string_view schemaId,
                     const ValidationReport& report) {
  Json issues = Json::array();
  for (const auto& issue : report.issues) {
    issues.push_back(Json{{"path", issue.path},
                          {"keyword", issue.keyword},
                          {"message", issue.message}});
  }
  return Status::failure(McpError::make(
      ErrorCode::InvalidParams,
      std::string(what) + " schema validation failed: " +
          std::string(schemaId),
      Json{{"issues", std::move(issues)}}));
}

} // namespace

void ToolRegistry::setValidationPolicy(SchemaValidatorPtr validator,
                                       bool allowUnvalidatedTools) {
  std::lock_guard<std::mutex> lock(mutex_);
  validator_ = std::move(validator);
  allow_unvalidated_tools_ = allowUnvalidatedTools;
  validation_policy_set_ = true;
}

Status ToolRegistry::add(ToolDescriptor descriptor, ToolHandlerPtr handler) {
  if (descriptor.name.empty()) {
    return Status::failure(
        McpError::make(ErrorCode::InvalidParams, "tool name must not be empty"));
  }
  if (descriptor.name.find('#') != std::string::npos) {
    return Status::failure(McpError::make(
        ErrorCode::InvalidParams, "tool name must not contain '#'"));
  }
  if (descriptor.inputSchema.is_null()) {
    return Status::failure(McpError::make(
        ErrorCode::InvalidParams,
        "inputSchema must be an object; omit the field to declare no schema"));
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

  if (validation_policy_set_ && !allow_unvalidated_tools_ && !validator_ &&
      !descriptor.inputSchema.empty()) {
    return Status::failure(McpError::make(
        ErrorCode::InvalidParams,
        "tool '" + descriptor.name +
            "' declares an inputSchema but no schema validator is configured "
            "and unvalidated tools are not allowed"));
  }

  if (validator_) {
    if (!descriptor.inputSchema.empty()) {
      Status compiled = validator_->compile(descriptor.name,
                                            descriptor.inputSchema,
                                            SchemaOptions{});
      if (!compiled) {
        return Status::failure(McpError::make(
            ErrorCode::InvalidParams,
            "tool '" + descriptor.name + "' input schema rejected: " +
                compiled.error.message,
            compiled.error.data));
      }
    }
    if (!descriptor.outputSchema.is_null()) {
      const std::string id = outputSchemaId(descriptor.name);
      Status compiled =
          validator_->compile(id, descriptor.outputSchema, SchemaOptions{});
      if (!compiled) {
        return Status::failure(McpError::make(
            ErrorCode::InvalidParams,
            "tool '" + descriptor.name + "' output schema rejected: " +
                compiled.error.message,
            compiled.error.data));
      }
    }
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

Status ToolRegistry::validateWith(std::string_view schemaId,
                                  const Json& instance, std::string_view what,
                                  const SchemaValidatorPtr& validator) const {
  auto report = validator->validate(schemaId, instance);
  if (!report) {
    return Status::failure(report.error());
  }
  if (report.value().ok) {
    return Status::success();
  }
  std::string what_copy(what);
  return reportFailure(what_copy.c_str(), schemaId, report.value());
}

Status ToolRegistry::validateInput(std::string_view name,
                                   const Json& arguments) const {
  std::optional<ToolDescriptor> descriptor;
  SchemaValidatorPtr validator;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tools_.find(std::string(name));
    if (it == tools_.end()) {
      return Status::success();
    }
    descriptor = it->second.descriptor;
    validator = validator_;
  }

  if (!validator || descriptor->inputSchema.is_null() ||
      descriptor->inputSchema.empty()) {
    return Status::success();
  }
  const std::string id(descriptor->name);
  return validateWith(id, arguments, "input", validator);
}

Status ToolRegistry::validateOutput(std::string_view name,
                                    const Json& structured) const {
  std::optional<ToolDescriptor> descriptor;
  SchemaValidatorPtr validator;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tools_.find(std::string(name));
    if (it == tools_.end()) {
      return Status::success();
    }
    descriptor = it->second.descriptor;
    validator = validator_;
  }

  if (!validator || descriptor->outputSchema.is_null()) {
    return Status::success();
  }
  return validateWith(outputSchemaId(descriptor->name), structured, "output",
                      validator);
}

} // namespace v1
} // namespace mcp
