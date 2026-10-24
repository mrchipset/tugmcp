#pragma once

// Factory for the Valijson-backed validator (JSON Schema draft-07).
//
// This header intentionally does not include any Valijson header: the
// implementation lives in src/schema/valijson_validator.cpp and is only
// available when TUGMCP_WITH_VALIJSON=ON.

#include <tugmcp/schema.hpp>

namespace mcp {
namespace v1 {

SchemaValidatorPtr makeValijsonSchemaValidator();

} // namespace v1
} // namespace mcp
