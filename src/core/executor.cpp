#include "tugmcp/executor.hpp"

#include <array>
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
                            Priority priority) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!accepting_) {
        return ExecutorSubmitResult{
            ExecutorSubmitResult::Kind::RejectedStopped};
      }
      if (max_queue_ != 0 && queuedLocked() >= max_queue_) {
        return ExecutorSubmitResult{
            ExecutorSubmitResult::Kind::RejectedQueueFull};
      }
      queues_[indexOf(priority)].push_back(std::move(task));
    }
    cv_.notify_one();
    return ExecutorSubmitResult{};
  }

  void drain() override {
    std::unique_lock<std::mutex> lock(mutex_);
    accepting_ = false;
    cv_.wait(lock, [this] {
      return queuedLocked() == 0 && active_ == 0;
    });
  }

  void stop() override {
    std::vector<std::thread> workers;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      accepting_ = false;
      shutdown_ = true;
      for (auto& queue : queues_) {
        queue.clear();
      }
      cv_.notify_all();
      cv_.wait(lock, [this] { return active_ == 0; });
      workers.swap(workers_);
    }
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  std::size_t workerCount() const override { return worker_count_; }

private:
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
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
          return shutdown_ || !queuesEmptyLocked();
        });
        if (queuesEmptyLocked()) {
          if (shutdown_) {
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
  bool accepting_ = true;
  bool shutdown_ = false;
};

std::shared_ptr<IExecutor> makeThreadPoolExecutor(std::size_t workers,
                                                  std::size_t maxQueue) {
  return std::make_shared<ThreadPoolExecutor>(workers, maxQueue);
}

} // namespace v1
} // namespace mcp
