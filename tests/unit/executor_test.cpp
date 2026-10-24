#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
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

TEST_CASE("drain waits for an already-running task") {
  auto executor = makeThreadPoolExecutor(1, 0);
  constexpr auto kShort = std::chrono::milliseconds(50);

  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future();

  std::atomic<int> completed{0};
  std::promise<void> drain_returned;
  auto drain_returned_future = drain_returned.get_future();

  REQUIRE(accepted(executor->post([&] {
    blocker_started.set_value();
    release_blocker_future.wait();
    completed.fetch_add(1, std::memory_order_relaxed);
  })));
  REQUIRE(blocker_started_future.wait_for(kWait) ==
          std::future_status::ready);
  REQUIRE(accepted(executor->post(
      [&] { completed.fetch_add(1, std::memory_order_relaxed); })));

  std::thread drainer([&] {
    executor->drain();
    drain_returned.set_value();
  });

  // The blocker is still held, so drain must not have returned yet.
  CHECK(drain_returned_future.wait_for(kShort) ==
        std::future_status::timeout);
  CHECK(completed.load(std::memory_order_relaxed) == 0);

  release_blocker.set_value();
  CHECK(drain_returned_future.wait_for(kWait) == std::future_status::ready);
  drainer.join();
  CHECK(completed.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("zero workers clamps to one and runs tasks") {
  auto executor = makeThreadPoolExecutor(0, 0);
  CHECK(executor->workerCount() == 1);

  std::promise<void> ran;
  auto ran_future = ran.get_future();
  REQUIRE(accepted(executor->post([&] { ran.set_value(); })));
  CHECK(ran_future.wait_for(kWait) == std::future_status::ready);

  executor->drain();
  CHECK(executor->stats().active == 0);
}

TEST_CASE("stop drops queued tasks and waits for the running one") {
  auto executor = makeThreadPoolExecutor(1, 0);
  constexpr auto kShort = std::chrono::milliseconds(50);

  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future();

  std::atomic<bool> running_finished{false};
  std::atomic<bool> queued_ran{false};

  REQUIRE(accepted(executor->post([&] {
    blocker_started.set_value();
    release_blocker_future.wait();
    running_finished.store(true, std::memory_order_relaxed);
  })));
  REQUIRE(blocker_started_future.wait_for(kWait) ==
          std::future_status::ready);
  REQUIRE(accepted(executor->post(
      [&] { queued_ran.store(true, std::memory_order_relaxed); })));

  std::promise<void> stop_returned;
  auto stop_returned_future = stop_returned.get_future();
  std::thread stopper([&] {
    executor->stop();
    stop_returned.set_value();
  });

  CHECK(stop_returned_future.wait_for(kShort) == std::future_status::timeout);
  CHECK_FALSE(running_finished.load(std::memory_order_relaxed));

  release_blocker.set_value();
  CHECK(stop_returned_future.wait_for(kWait) == std::future_status::ready);
  stopper.join();

  CHECK(running_finished.load(std::memory_order_relaxed));
  CHECK_FALSE(queued_ran.load(std::memory_order_relaxed));
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedStopped);
}

TEST_CASE("capacity bound counts queued tasks only") {
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

  // One task is running (does not consume the queue slot) and one is queued.
  CHECK(accepted(executor->post([] {})));
  const mcp::v1::ExecutorStats busy = executor->stats();
  CHECK(busy.queued == 1);
  CHECK(busy.active == 1);
  CHECK(busy.capacity == 1);

  // A third submission is rejected even though one task is already running.
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedQueueFull);

  release_blocker.set_value();
  executor->drain();

  const mcp::v1::ExecutorStats idle = executor->stats();
  CHECK(idle.queued == 0);
  CHECK(idle.active == 0);
}

TEST_CASE("emergency submissions bypass a full queue") {
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

  REQUIRE(accepted(executor->post([] {})));
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedQueueFull);
  CHECK(accepted(executor->post([] {}, Priority::Emergency)));

  release_blocker.set_value();
  executor->drain();

  executor->stop();
  CHECK(executor->post([] {}, Priority::Emergency).kind ==
        ExecutorSubmitResult::Kind::RejectedStopped);
}

TEST_CASE("a task can post nested work that still completes") {
  auto executor = makeThreadPoolExecutor(1, 0);

  std::promise<void> nested_ran;
  auto nested_ran_future = nested_ran.get_future();
  std::atomic<bool> nested_accepted{false};

  REQUIRE(accepted(executor->post([&] {
    const auto result = executor->post([&] { nested_ran.set_value(); });
    nested_accepted.store(static_cast<bool>(result),
                          std::memory_order_relaxed);
  })));

  CHECK(nested_ran_future.wait_for(kWait) == std::future_status::ready);
  CHECK(nested_accepted.load(std::memory_order_relaxed));

  executor->drain();
}

TEST_CASE("stop from inside a worker task does not deadlock") {
  auto executor = makeThreadPoolExecutor(1, 0);

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> stopped;
  auto stopped_future = stopped.get_future();
  std::atomic<bool> stop_returned{false};

  REQUIRE(accepted(executor->post([&] {
    entered.set_value();
    executor->stop();
    stop_returned.store(true, std::memory_order_relaxed);
    stopped.set_value();
  })));

  REQUIRE(entered_future.wait_for(kWait) == std::future_status::ready);
  CHECK(stopped_future.wait_for(kWait) == std::future_status::ready);
  CHECK(stop_returned.load(std::memory_order_relaxed));
  CHECK(executor->post([] {}).kind ==
        ExecutorSubmitResult::Kind::RejectedStopped);
}

TEST_CASE("waitIdle reports completion and times out while busy") {
  auto executor = makeThreadPoolExecutor(1, 0);

  CHECK(executor->waitIdle(std::chrono::milliseconds(10)));

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

  CHECK_FALSE(executor->waitIdle(std::chrono::milliseconds(0)));

  release_blocker.set_value();
  CHECK(executor->waitIdle(kWait));
}

TEST_CASE("stats reflect queued, running and capacity") {
  auto executor = makeThreadPoolExecutor(1, 3);
  const mcp::v1::ExecutorStats empty = executor->stats();
  CHECK(empty.queued == 0);
  CHECK(empty.active == 0);
  CHECK(empty.capacity == 3);

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
  REQUIRE(accepted(executor->post([] {})));
  REQUIRE(accepted(executor->post([] {})));

  const mcp::v1::ExecutorStats busy = executor->stats();
  CHECK(busy.queued == 2);
  CHECK(busy.active == 1);
  CHECK(busy.capacity == 3);

  release_blocker.set_value();
  executor->drain();

  const mcp::v1::ExecutorStats idle = executor->stats();
  CHECK(idle.queued == 0);
  CHECK(idle.active == 0);
}

TEST_CASE("concurrent posts racing with drain all complete") {
  auto executor = makeThreadPoolExecutor(2, 0);

  constexpr int kPosters = 4;
  constexpr int kPerPoster = 8;
  std::atomic<int> accepted_count{0};
  std::atomic<int> completed{0};

  // Seed one accepted task so the drainer cannot win the race before any
  // submission is admitted; this keeps the "at least one accepted" assertion
  // deterministic.
  REQUIRE(accepted(executor->post(
      [&] { completed.fetch_add(1, std::memory_order_relaxed); })));
  accepted_count.store(1, std::memory_order_relaxed);

  std::thread drainer([&] { executor->drain(); });

  std::vector<std::thread> posters;
  posters.reserve(kPosters);
  for (int poster = 0; poster < kPosters; ++poster) {
    posters.emplace_back([&] {
      for (int index = 0; index < kPerPoster; ++index) {
        if (accepted(executor->post(
                [&] { completed.fetch_add(1, std::memory_order_relaxed); }))) {
          accepted_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& poster : posters) {
    poster.join();
  }
  drainer.join();

  CHECK(accepted_count.load(std::memory_order_relaxed) > 0);
  CHECK(completed.load(std::memory_order_relaxed) ==
        accepted_count.load(std::memory_order_relaxed));
}
