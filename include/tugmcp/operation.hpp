#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <tugmcp/error.hpp>

namespace mcp {
namespace v1 {

using Json = nlohmann::json;

// Wire-level outcome discriminator used by 2026-07-28 results.
enum class ResultType { Complete, InputRequired };

std::string_view toString(ResultType type) noexcept;

// Multi-round-trip request payload. Both fields must be present so a retry can
// round-trip the server state; an invalid value is a handler bug and is reported
// as an internal error.
struct InputRequired {
  std::optional<Json> inputRequests;
  std::optional<std::string> requestState;

  bool valid() const noexcept {
    return inputRequests.has_value() && requestState.has_value();
  }
};

// A handler either produced a complete value or asked for more client input.
// Accessing the wrong alternative is a programming error and throws
// std::logic_error, matching Result's accessor policy.
template <class T>
struct OperationOutcome {
  std::variant<T, InputRequired> value;

  // Intentionally implicit so handlers can `return ToolResult{...};`.
  OperationOutcome(T complete) : value(std::move(complete)) {}
  OperationOutcome(InputRequired input) : value(std::move(input)) {}

  bool isInputRequired() const noexcept {
    return std::holds_alternative<InputRequired>(value);
  }

  T& result() {
    if (isInputRequired()) {
      throw std::logic_error(
          "OperationOutcome::result() called for input_required");
    }
    return std::get<T>(value);
  }

  const T& result() const {
    if (isInputRequired()) {
      throw std::logic_error(
          "OperationOutcome::result() called for input_required");
    }
    return std::get<T>(value);
  }

  InputRequired& inputRequired() {
    if (!isInputRequired()) {
      throw std::logic_error(
          "OperationOutcome::inputRequired() called for a complete outcome");
    }
    return std::get<InputRequired>(value);
  }

  const InputRequired& inputRequired() const {
    if (!isInputRequired()) {
      throw std::logic_error(
          "OperationOutcome::inputRequired() called for a complete outcome");
    }
    return std::get<InputRequired>(value);
  }
};

using ProgressToken = std::variant<std::string, std::int64_t>;

// Accepts only JSON strings and signed 64-bit integers. Unsigned values above
// INT64_MAX, booleans, floating point, null, and composite values are rejected
// with InvalidParams.
Result<ProgressToken> progressTokenFromJson(const Json& json);

Json progressTokenToJson(const ProgressToken& token);

struct Progress {
  ProgressToken token;
  double progress = 0.0;
  std::optional<double> total;
  std::string message;
};

// Sink handed to handlers for in-request progress. The dispatcher binds it to
// the request's progressToken and the transport notification hook.
class IProgressSink {
public:
  virtual ~IProgressSink() = default;
  virtual void report(double fraction, std::string_view message) = 0;
};

struct ListParams {
  std::string cursor;
};

// Opaque cursor pagination envelope. Providers own cursor semantics; the
// dispatcher only slices tool pages and serializes the envelope.
template <class T>
struct Page {
  std::vector<T> items;
  std::optional<std::string> nextCursor;
  std::uint32_t ttlMs = 0;
  std::string cacheScope;
};

enum class ListKind { Tools, Prompts, Resources };

using SubscriptionId = std::string;

struct SubscriptionFilter {
  std::vector<ListKind> listChanged;
  std::vector<std::string> resources;
};

} // namespace v1
} // namespace mcp
