#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>
#include <tugmcp/executor.hpp>

using mcp::v1::ExecutorSubmitResult;
using mcp::v1::IExecutor;
using mcp::v1::Priority;
using mcp::v1::makeThreadPoolExecutor;

namespace {

constexpr auto kWait = std::chrono::seconds(2);

bool accepted(const ExecutorSubmitResult& result) {
  return static_cast<bool>(result);
}

} // namespace

TEST_CASE("thread pool reports its worker count") {
  auto executor = makeThreadPoolExecutor(3, 0);
  REQUIRE(static_cast<bool>(executor));
  CHECK(executor->workerCount() == 3);
  executor->stop();
}

TEST_CASE("all submitted tasks run") {
  auto executor = makeThreadPoolExecutor(4, 0);
  constexpr int kTasks = 64;

  std::atomic<int> completed{0};
  std::mutex mutex;
  std::condition_variable cv;

  for (int index = 0; index < kTasks; ++index) {
    REQUIRE(accepted(executor->post([&] {
      completed.fetch_add(1, std::memory_order_relaxed);
      cv.notify_all();
    })));
  }

  std::unique_lock<std::mutex> lock(mutex);
  CHECK(cv.wait_for(lock, kWait, [&] {
    return completed.load(std::memory_order_relaxed) == kTasks;
  }));
  lock.unlock();

  executor->drain();
  CHECK(completed.load(std::memory_order_relaxed) == kTasks);
}

TEST_CASE("High priority tasks run before queued Normal tasks") {
  auto executor = makeThreadPoolExecutor(1, 0);

  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future();

  std::mutex order_mutex;
  std::condition_variable order_cv;
  std::vector<int> order;

  auto record = [&](int value) {
    std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(value);
    order_cv.notify_all();
  };

  REQUIRE(accepted(executor->post([&] {
    blocker_started.set_value();
    release_blocker_future.wait();
    record(0);
  })));
  REQUIRE(blocker_started_future.wait_for(kWait) ==
          std::future_status::ready);

  REQUIRE(accepted(executor->post([&] { record(2); })));
  REQUIRE(accepted(executor->post([&] { record(1); }, Priority::High)));

  release_blocker.set_value();

  std::unique_lock<std::mutex> lock(order_mutex);
  CHECK(order_cv.wait_for(lock, kWait, [&] { return order.size() == 3; }));
  REQUIRE(order.size() == 3);
  CHECK(order[0] == 0);
  CHECK(order[1] == 1);
  CHECK(order[2] == 2);
  lock.unlock();

  executor->drain();
}

TEST_CASE("queue full submissions are rejected") {
  auto executor = makeThreadPoolExecutor(1, 1);

  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future();

  REQUIRE(accepted(executor->post([&] {
    blocker_started.set_value();
    release_blocker_future.wait();
  })));
  REQUIRE(blocker_started_future.wait_for(kWait) ==
          std::future_status::ready);

  CHECK(accepted(executor->post([] {})));
  const ExecutorSubmitResult rejected = executor->post([] {});
  CHECK(rejected.kind ==
        ExecutorSubmitResult::Kind::RejectedQueueFull);
  CHECK_FALSE(accepted(rejected));

  release_blocker.set_value();
  executor->drain();
}

TEST_CASE("post after stop is rejected") {
  auto executor = makeThreadPoolExecutor(2, 0);
  executor->stop();

  const ExecutorSubmitResult result = executor->post([] {});
  CHECK(result.kind == ExecutorSubmitResult::Kind::RejectedStopped);
  CHECK_FALSE(accepted(result));

  executor->stop();
}

TEST_CASE("drain waits for queued tasks and then rejects new work") {
  auto executor = makeThreadPoolExecutor(2, 0);
  constexpr int kTasks = 32;
  std::atomic<int> completed{0};

  for (int index = 0; index < kTasks; ++index) {
    REQUIRE(accepted(executor->post(
        [&] { completed.fetch_add(1, std::memory_order_relaxed); })));
  }

  executor->drain();
  CHECK(completed.load(std::memory_order_relaxed) == kTasks);

  const ExecutorSubmitResult result = executor->post([] {});
  CHECK(result.kind == ExecutorSubmitResult::Kind::RejectedStopped);
}

TEST_CASE("task exceptions are contained and workers keep running") {
  auto executor = makeThreadPoolExecutor(1, 0);
  std::atomic<int> completed{0};

  REQUIRE(accepted(executor->post(
      [] { throw std::runtime_error("task failure"); })));
  REQUIRE(accepted(executor->post(
      [&] { completed.fetch_add(1, std::memory_order_relaxed); })));

  executor->drain();
  CHECK(completed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("drain and stop are idempotent") {
  auto executor = makeThreadPoolExecutor(1, 0);

  executor->drain();
  executor->drain();
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedStopped);

  executor->stop();
  executor->stop();
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedStopped);

  executor->drain();
}
