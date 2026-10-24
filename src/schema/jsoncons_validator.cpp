#include <tugmcp/schema/jsoncons.hpp>

#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace mcp {
namespace v1 {
namespace {

using JsonconsJson = jsoncons::json;

bool hasRemoteRef(const JsonconsJson& schema) {
  if (schema.is_object()) {
    const auto ref = schema.find("$ref");
    if (ref != schema.object_range().end() && ref->value().is_string()) {
      const std::string target = ref->value().as<std::string>();
      if (target.rfind('#', 0) != 0) {
        return true;
      }
    }
    for (const auto& member : schema.object_range()) {
      if (hasRemoteRef(member.value())) {
        return true;
      }
    }
  } else if (schema.is_array()) {
    for (const auto& item : schema.array_range()) {
      if (hasRemoteRef(item)) {
        return true;
      }
    }
  }
  return false;
}

bool keywordsAllowed(const JsonconsJson& schema,
                     const std::vector<std::string>& allowed) {
  if (allowed.empty() || !schema.is_object()) {
    return true;
  }
  for (const auto& member : schema.object_range()) {
    const std::string key = member.key();
    bool found = false;
    for (const auto& candidate : allowed) {
      if (candidate == key) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// jsoncons silently accepts some malformed keyword values (for example a
// numeric "type"). Reject the obvious cases so compile() fails closed.
bool schemaKeywordsValid(const JsonconsJson& schema) {
  if (!schema.is_object()) {
    return true;
  }
  const auto type = schema.find("type");
  if (type != schema.object_range().end()) {
    const JsonconsJson& value = type->value();
    if (!value.is_string()) {
      if (!value.is_array()) {
        return false;
      }
      for (const auto& item : value.array_range()) {
        if (!item.is_string()) {
          return false;
        }
      }
    }
  }
  return true;
}

Status invalid(std::string message) {
  return Status::failure(
      McpError::make(ErrorCode::InvalidParams, std::move(message)));
}

class JsonconsSchemaValidator final : public ISchemaValidator {
public:
  Status compile(std::string_view schemaId, const Json& schema,
                 const SchemaOptions& options) override {
    try {
      JsonconsJson parsed = JsonconsJson::parse(schema.dump());
      if (!options.allowRemoteRefs && hasRemoteRef(parsed)) {
        return invalid("schema " + std::string(schemaId) +
                       " uses a remote $ref but remote references are "
                       "disabled");
      }
      if (!keywordsAllowed(parsed, options.keywords)) {
        return invalid("schema " + std::string(schemaId) +
                       " uses a keyword outside SchemaOptions.keywords");
      }
      if (!schemaKeywordsValid(parsed)) {
        return invalid("schema " + std::string(schemaId) +
                       " has an invalid keyword value");
      }

      jsoncons::jsonschema::evaluation_options evaluation;
      evaluation.default_version(
          jsoncons::jsonschema::schema_version::draft202012());
      if (!options.baseUri.empty()) {
        evaluation.default_base_uri(options.baseUri);
      }

      auto compiled =
          jsoncons::jsonschema::make_json_schema(std::move(parsed), evaluation);

      std::lock_guard<std::mutex> lock(mutex_);
      schemas_.erase(std::string(schemaId));
      schemas_.emplace(std::string(schemaId), std::move(compiled));
      return Status::success();
    } catch (const std::exception& error) {
      return invalid("schema " + std::string(schemaId) +
                     " failed to compile: " + error.what());
    } catch (...) {
      return invalid("schema " + std::string(schemaId) +
                     " failed to compile: unknown error");
    }
  }

  Result<ValidationReport> validate(std::string_view schemaId,
                                    const Json& instance) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = schemas_.find(std::string(schemaId));
    if (it == schemas_.end()) {
      return Result<ValidationReport>::failure(McpError::make(
          ErrorCode::InternalError, "schema is not compiled",
          Json{{"reason", "schema_not_compiled"},
             {"schemaId", std::string(schemaId)}}));
    }

    try {
      const JsonconsJson parsed = JsonconsJson::parse(instance.dump());
      ValidationReport report;
      report.ok = true;
      // Belt-and-braces: jsoncons' reporter-based validate() returns void, so
      // also consult is_valid() and never allow a false negative to pass.
      const bool passed = it->second.is_valid(parsed);
      it->second.validate(
          parsed,
          [&report](const jsoncons::jsonschema::validation_message& message)
              -> jsoncons::jsonschema::walk_state {
            report.ok = false;
            ValidationIssue issue;
            issue.path = message.instance_location().to_string();
            issue.keyword = message.keyword();
            issue.message = message.message();
            report.issues.push_back(std::move(issue));
            return jsoncons::jsonschema::walk_state::advance;
          });
      report.ok = report.ok && passed;
      return report;
    } catch (const std::exception& error) {
      return Result<ValidationReport>::failure(McpError::make(
          ErrorCode::InternalError,
          std::string("schema validation failed: ") + error.what()));
    } catch (...) {
      return Result<ValidationReport>::failure(McpError::make(
          ErrorCode::InternalError, "schema validation failed"));
    }
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string,
           jsoncons::jsonschema::json_schema<jsoncons::json>>
      schemas_;
};

} // namespace

SchemaValidatorPtr makeJsonconsSchemaValidator() {
  return std::make_shared<JsonconsSchemaValidator>();
}

} // namespace v1
} // namespace mcp
