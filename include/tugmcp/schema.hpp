#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <tugmcp/error.hpp>

namespace mcp {
namespace v1 {

// A single JSON Schema validation problem.
struct ValidationIssue {
  std::string path;
  std::string keyword;
  std::string message;
};

// Result of validating one instance against one compiled schema.
struct ValidationReport {
  bool ok = false;
  std::vector<ValidationIssue> issues;
};

// Options supplied when compiling a schema.
//
// `baseUri` is used to resolve relative references. Remote references are
// disabled by default; implementations that cannot fetch them must reject the
// schema at compile time rather than silently ignoring the reference.
// `keywords` optionally restricts the accepted keyword set; an empty vector
// means "the implementation's default set".
struct SchemaOptions {
  std::string baseUri;
  bool allowRemoteRefs = false;
  std::vector<std::string> keywords;
};

// Pluggable JSON Schema validator. Implementations are expected to be
// thread-safe for concurrent validate() calls once compile() has completed.
class ISchemaValidator {
public:
  virtual ~ISchemaValidator() = default;

  virtual Status compile(std::string_view schemaId, const Json& schema,
                         const SchemaOptions& options) = 0;

  virtual Result<ValidationReport> validate(std::string_view schemaId,
                                            const Json& instance) const = 0;
};

using SchemaValidatorPtr = std::shared_ptr<ISchemaValidator>;

} // namespace v1
} // namespace mcp
