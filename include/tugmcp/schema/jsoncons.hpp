#pragma once

// Factory for the jsoncons-backed validator (JSON Schema 2020-12).
//
// This header intentionally does not include any jsoncons header: the
// implementation lives in src/schema/jsoncons_validator.cpp and is only
// available when TUGMCP_WITH_JSONCONS=ON.

#include <tugmcp/schema.hpp>

namespace mcp {
namespace v1 {

SchemaValidatorPtr makeJsonconsSchemaValidator();

} // namespace v1
} // namespace mcp
