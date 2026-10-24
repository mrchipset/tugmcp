#pragma once

#include <chrono>
#include <memory>

namespace mcp {
namespace v1 {

class ITimeSource {
public:
  virtual ~ITimeSource() = default;
  virtual std::chrono::steady_clock::time_point now() = 0;
};

class SystemTimeSource final : public ITimeSource {
public:
  std::chrono::steady_clock::time_point now() override;
};

inline std::chrono::steady_clock::time_point SystemTimeSource::now() {
  return std::chrono::steady_clock::now();
}

inline std::shared_ptr<ITimeSource> makeSystemTimeSource() {
  return std::make_shared<SystemTimeSource>();
}

} // namespace v1
} // namespace mcp
