#pragma once

// TEST-ONLY validator. `NullSchemaValidator` compiles every schema and then
// fails every validation closed. It exists so tests and examples can exercise
// the wiring without pulling in a real schema engine; production hosts must
// inject a real validator (jsoncons or Valijson) instead.

#include <string_view>

#include <tugmcp/schema.hpp>

namespace mcp {
namespace v1 {

namespace detail {

class NullSchemaValidator final : public ISchemaValidator {
public:
  Status compile(std::string_view, const Json&, const SchemaOptions&) override {
    return Status::success();
  }

  Result<ValidationReport> validate(std::string_view,
                                    const Json&) const override {
    return Result<ValidationReport>::failure(McpError::make(
        ErrorCode::InternalError, "schema validation unavailable",
        Json{{"reason", "schema_validator_unavailable"}}));
  }
};

} // namespace detail

inline SchemaValidatorPtr makeNullSchemaValidator() {
  return std::make_shared<detail::NullSchemaValidator>();
}

} // namespace v1
} // namespace mcp
