#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace mcp {
namespace v1 {

enum class Priority { Normal, High, Emergency };

struct ExecutorSubmitResult {
  enum class Kind { Accepted, RejectedQueueFull, RejectedStopped };

  Kind kind = Kind::Accepted;

  explicit operator bool() const noexcept { return kind == Kind::Accepted; }
};

// Snapshot of an executor's queue/worker state.
struct ExecutorStats {
  std::size_t queued = 0;
  std::size_t active = 0;
  std::size_t capacity = 0; // 0 == unbounded
};

// An executor runs submitted tasks. Implementations must keep the following
// contract; the library relies on it for cancellation and shutdown paths.
//
// Destroying an executor from inside one of its own tasks is unsupported.
// Calling stop() from a task is supported; releasing the last owner from a
// task is not.
class IExecutor {
public:
  virtual ~IExecutor() = default;

  // Thread-safe, non-blocking, never throws. High is served before Normal;
  // Emergency before High; FIFO within a priority level.
  //
  // Emergency submissions bypass the capacity bound because they belong to the
  // admission/safety path; they are still rejected with RejectedStopped after
  // drain/stop. The capacity bound counts queued tasks only.
  virtual ExecutorSubmitResult post(
      std::function<void()> task,
      Priority priority = Priority::Normal) noexcept = 0;

  // Terminal: rejects all later submissions, then waits for queued and running
  // tasks to finish. When invoked from one of the executor's own worker tasks
  // it must not wait for that calling task (self-exclusion) and must not
  // deadlock.
  virtual void drain() noexcept = 0;

  // Terminal: rejects later submissions, drops queued tasks, waits for running
  // tasks (self-excluding) to finish, then joins workers it can join.
  // Idempotent.
  virtual void stop() noexcept = 0;

  virtual std::size_t workerCount() const noexcept = 0;

  // Non-pure for source compatibility; a default implementation reports
  // nothing.
  virtual ExecutorStats stats() const noexcept { return {}; }

  // Non-terminal observable wait. Returns true when queued+active are zero
  // before the timeout. Does not change the accepting state.
  //
  // The default returns false because the base class cannot observe idleness.
  virtual bool waitIdle(std::chrono::milliseconds timeout) noexcept {
    (void)timeout;
    return false;
  }
};

// maxQueue == 0 means unbounded. A zero worker count is clamped to one so the
// executor cannot deadlock immediately.
std::shared_ptr<IExecutor> makeThreadPoolExecutor(std::size_t workers,
                                                  std::size_t maxQueue);

} // namespace v1
} // namespace mcp
