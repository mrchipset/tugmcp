#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace mcp {
namespace v1 {

enum class Priority { Normal, High, Emergency };

struct ExecutorSubmitResult {
  enum class Kind { Accepted, RejectedQueueFull, RejectedStopped };

  Kind kind = Kind::Accepted;

  explicit operator bool() const { return kind == Kind::Accepted; }
};

class IExecutor {
public:
  virtual ~IExecutor() = default;

  // Thread-safe. High is served before Normal; Emergency before High.
  virtual ExecutorSubmitResult post(
      std::function<void()> task,
      Priority priority = Priority::Normal) = 0;

  // Stop accepting new tasks and wait for queued/running tasks to finish.
  virtual void drain() = 0;

  // Stop accepting new tasks and wait for running tasks only; queued tasks
  // are dropped.
  virtual void stop() = 0;

  virtual std::size_t workerCount() const = 0;
};

// maxQueue == 0 means unbounded. A zero worker count is clamped to one so the
// executor cannot deadlock immediately.
std::shared_ptr<IExecutor> makeThreadPoolExecutor(std::size_t workers,
                                                  std::size_t maxQueue);

} // namespace v1
} // namespace mcp
