#include <tugmcp/schema/valijson.hpp>

#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>

namespace mcp {
namespace v1 {
namespace {

bool hasRemoteRef(const Json& schema) {  if (schema.is_object()) {
    const auto ref = schema.find("$ref");
    if (ref != schema.end() && ref->is_string()) {
      const std::string target = ref->get<std::string>();
      if (target.rfind('#', 0) != 0) {
        return true;
      }
    }
    for (auto it = schema.begin(); it != schema.end(); ++it) {
      if (hasRemoteRef(it.value())) {
        return true;
      }
    }
  } else if (schema.is_array()) {
    for (const auto& item : schema) {
      if (hasRemoteRef(item)) {
        return true;
      }
    }
  }
  return false;
}

// Valijson asserts (and aborts in debug builds) on some malformed keyword
// values, for example a numeric "type". Reject the obvious cases first so
// compile() fails with InvalidParams instead of aborting.
bool schemaKeywordsValid(const Json& schema) {
  if (!schema.is_object()) {
    return true;
  }
  const auto type = schema.find("type");
  if (type != schema.end()) {
    if (!type->is_string()) {
      if (!type->is_array()) {
        return false;
      }
      for (const auto& item : *type) {
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
class ValijsonSchemaValidator final : public ISchemaValidator {
public:
  Status compile(std::string_view schemaId, const Json& schema,
                 const SchemaOptions& options) override {
    if (hasRemoteRef(schema)) {
      return invalid("schema " + std::string(schemaId) +
                     " uses a remote $ref; Valijson remote references are not "
                     "supported" +
                     (options.allowRemoteRefs ? "" : " (and are disabled)"));
    }

    if (!schemaKeywordsValid(schema)) {
      return invalid("schema " + std::string(schemaId) +
                     " has an invalid keyword value");
    }

    try {
      auto compiled = std::make_shared<valijson::Schema>();
      valijson::SchemaParser parser(valijson::SchemaParser::kDraft7);
      valijson::adapters::NlohmannJsonAdapter adapter(schema);
      parser.populateSchema(adapter, *compiled);

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
    std::shared_ptr<const valijson::Schema> schema;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = schemas_.find(std::string(schemaId));
      if (it == schemas_.end()) {
        return Result<ValidationReport>::failure(McpError::make(
            ErrorCode::InternalError, "schema is not compiled",
            Json{{"reason", "schema_not_compiled"},
               {"schemaId", std::string(schemaId)}}));
      }
      schema = it->second;
    }

    try {
      valijson::ValidationResults results;
      valijson::Validator validator;
      valijson::adapters::NlohmannJsonAdapter adapter(instance);
      ValidationReport report;
      report.ok = validator.validate(*schema, adapter, &results);

      if (!report.ok) {
        for (const auto& error : results) {
          ValidationIssue issue;
          issue.path = error.jsonPointer;
          issue.keyword = "validation";
          issue.message = error.description;
          report.issues.push_back(std::move(issue));
        }
      }
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
  std::map<std::string, std::shared_ptr<const valijson::Schema>> schemas_;
};

} // namespace

SchemaValidatorPtr makeValijsonSchemaValidator() {
  return std::make_shared<ValijsonSchemaValidator>();
}

} // namespace v1
} // namespace mcp
