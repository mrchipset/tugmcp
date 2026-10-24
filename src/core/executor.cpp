#include "tugmcp/executor.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {
namespace v1 {
namespace {

constexpr std::size_t kPriorityLevels = 3;

// Identifies the executor whose worker task is currently executing on this
// thread. A per-executor owner (rather than a plain bool) is required so that
// a task belonging to one pool calling stop()/drain() on another pool is not
// mistaken for a self-call.
thread_local const void* t_worker_owner = nullptr;

} // namespace

class ThreadPoolExecutor final : public IExecutor {
public:
  ThreadPoolExecutor(std::size_t workers, std::size_t maxQueue)
      : worker_count_(workers == 0 ? 1 : workers), max_queue_(maxQueue) {
    workers_.reserve(worker_count_);
    try {
      for (std::size_t index = 0; index < worker_count_; ++index) {
        workers_.emplace_back([this] { workerLoop(); });
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        shutdown_ = true;
        for (auto& queue : queues_) {
          queue.clear();
        }
      }
      cv_.notify_all();
      for (auto& worker : workers_) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      throw;
    }
  }

  ~ThreadPoolExecutor() override { stop(); }

  ExecutorSubmitResult post(std::function<void()> task,
                            Priority priority) noexcept override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!accepting_) {
        return ExecutorSubmitResult{
            ExecutorSubmitResult::Kind::RejectedStopped};
      }
      // Emergency submissions are part of the admission/safety path and
      // bypass the capacity bound. The bound counts queued tasks only.
      if (priority != Priority::Emergency && max_queue_ != 0 &&
          queuedLocked() >= max_queue_) {
        return ExecutorSubmitResult{
            ExecutorSubmitResult::Kind::RejectedQueueFull};
      }
      try {
        queues_[indexOf(priority)].push_back(std::move(task));
      } catch (...) {
        // Allocation failure while growing a queue is reported as queue full
        // because post() is noexcept and the task was not accepted.
        return ExecutorSubmitResult{
            ExecutorSubmitResult::Kind::RejectedQueueFull};
      }
    }
    cv_.notify_one();
    return ExecutorSubmitResult{};
  }

  void drain() noexcept override {
    const std::size_t self = isWorkerTask() ? 1 : 0;
    std::unique_lock<std::mutex> lock(mutex_);
    accepting_ = false;
    cv_.wait(lock, [this, self] {
      return queuedLocked() == 0 && active_ == self;
    });
  }

  void stop() noexcept override {
    const bool self = isWorkerTask();
    const std::size_t self_count = self ? 1 : 0;
    std::vector<std::thread> workers;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      accepting_ = false;
      shutdown_ = true;
      for (auto& queue : queues_) {
        queue.clear();
      }
      cv_.notify_all();
      cv_.wait(lock, [this, self_count] { return active_ == self_count; });
      workers.swap(workers_);

      const std::size_t expected_exit =
          self ? worker_count_ - 1 : worker_count_;
      for (auto& worker : workers) {
        if (!worker.joinable()) {
          continue;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
          worker.detach();
        }
      }
      // Wait until every non-self worker has fully left workerLoop before
      // returning. This keeps a later stop()/destructor from racing with a
      // detached worker that still touches `this`.
      cv_.wait(lock, [this, expected_exit] {
        return exited_ >= expected_exit;
      });
    }
    for (auto& worker : workers) {
      if (!worker.joinable()) {
        continue;
      }
      if (worker.get_id() == std::this_thread::get_id()) {
        continue;
      }
      try {
        worker.join();
      } catch (...) {
        // join() only throws on misuse; stop() must stay noexcept.
      }
    }
  }

  std::size_t workerCount() const noexcept override { return worker_count_; }

  ExecutorStats stats() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return ExecutorStats{queuedLocked(), active_, max_queue_};
  }

  bool waitIdle(std::chrono::milliseconds timeout) noexcept override {
    const std::size_t self = isWorkerTask() ? 1 : 0;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto idle = [this, self] {
      return queuedLocked() == 0 && active_ == self;
    };
    if (idle()) {
      return true;
    }
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + timeout,
                          idle);
  }

private:
  bool isWorkerTask() const noexcept {
    return t_worker_owner == static_cast<const void*>(this);
  }

  static std::size_t indexOf(Priority priority) {
    switch (priority) {
    case Priority::Emergency:
      return 0;
    case Priority::High:
      return 1;
    case Priority::Normal:
      return 2;
    }
    return 2;
  }

  bool queuesEmptyLocked() const {
    for (const auto& queue : queues_) {
      if (!queue.empty()) {
        return false;
      }
    }
    return true;
  }

  std::size_t queuedLocked() const {
    std::size_t total = 0;
    for (const auto& queue : queues_) {
      total += queue.size();
    }
    return total;
  }

  std::function<void()> takeNextLocked() {
    for (auto& queue : queues_) {
      if (!queue.empty()) {
        auto task = std::move(queue.front());
        queue.pop_front();
        return task;
      }
    }
    return {};
  }

  void workerLoop() noexcept {
    t_worker_owner = static_cast<const void*>(this);
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
          return shutdown_ || !queuesEmptyLocked();
        });
        if (queuesEmptyLocked()) {
          if (shutdown_) {
            t_worker_owner = nullptr;
            ++exited_;
            cv_.notify_all();
            return;
          }
          continue;
        }
        task = takeNextLocked();
        ++active_;
      }

      try {
        task();
      } catch (...) {
        // Task failures must not terminate a worker.
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
      }
      cv_.notify_all();
    }
  }

  const std::size_t worker_count_;
  const std::size_t max_queue_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::array<std::deque<std::function<void()>>, kPriorityLevels> queues_;
  std::vector<std::thread> workers_;
  std::size_t active_ = 0;
  std::size_t exited_ = 0;
  bool accepting_ = true;
  bool shutdown_ = false;
};

std::shared_ptr<IExecutor> makeThreadPoolExecutor(std::size_t workers,
                                                  std::size_t maxQueue) {
  return std::make_shared<ThreadPoolExecutor>(workers, maxQueue);
}

} // namespace v1
} // namespace mcp
