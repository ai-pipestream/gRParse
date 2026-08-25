#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>

#include "grparse/call_executor.h"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// A latch the tests park tasks on so a worker is provably occupied without a
// sleep deciding the outcome.
class Gate final {
 public:
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    open_changed_.wait(lock, [this] { return open_; });
  }

  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    open_changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable open_changed_;
  bool open_ = false;
};

void await(const std::atomic<int>& counter, int target, const std::string& message) {
  for (int attempt = 0; attempt < 1000 && counter.load() < target; ++attempt) {
    std::this_thread::sleep_for(5ms);
  }
  require(counter.load() >= target, message);
}

// Every submitted task runs, and on a worker thread rather than the caller's.
void verify_tasks_run_off_the_submitting_thread() {
  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<int> ran{0};
  std::atomic<int> on_caller{0};
  // Declared after what the tasks capture: the executor joins its workers in
  // its destructor, so it has to be destroyed first.
  grparse::CallExecutor executor({.workers = 4, .queue_capacity = 64});
  for (int task = 0; task < 32; ++task) {
    require(executor.submit([&] {
              if (std::this_thread::get_id() == caller) on_caller.fetch_add(1);
              ran.fetch_add(1);
            }),
            "an idle executor must accept work");
  }
  await(ran, 32, "every submitted task must run");
  require(on_caller.load() == 0, "no task may run on the submitting thread");
}

// A full queue refuses instead of growing: the caller holds a live RPC and is
// the only thing that can answer it, so the rejection has to be visible.
void verify_a_full_queue_refuses() {
  Gate gate;
  std::atomic<int> started{0};
  grparse::CallExecutor executor({.workers = 1, .queue_capacity = 2});
  require(executor.submit([&] {
            started.fetch_add(1);
            gate.wait();
          }),
          "the first task must be accepted");
  await(started, 1, "the only worker must pick up the first task");
  require(executor.submit([&] { gate.wait(); }), "queue slot one must be accepted");
  require(executor.submit([&] { gate.wait(); }), "queue slot two must be accepted");
  require(!executor.submit([] {}), "a full queue must refuse");
  require(executor.queued() == 2, "the refused task must not have been queued");
  gate.open();
}

// Shutdown drains: a queued task owns a call, so dropping it would leave that
// call hanging until its deadline.
void verify_shutdown_drains_the_queue() {
  Gate gate;
  std::atomic<int> started{0};
  std::atomic<int> ran{0};
  {
    grparse::CallExecutor executor({.workers = 1, .queue_capacity = 16});
    require(executor.submit([&] {
              started.fetch_add(1);
              gate.wait();
              ran.fetch_add(1);
            }),
            "the blocking task must be accepted");
    await(started, 1, "the only worker must pick up the blocking task");
    for (int task = 0; task < 8; ++task) {
      require(executor.submit([&] { ran.fetch_add(1); }), "queued tasks must be accepted");
    }
    gate.open();
  }
  require(ran.load() == 9, "shutdown must drain every queued task, ran " +
                               std::to_string(ran.load()));
}

// A throwing task must not take its worker down with it: the pool has to keep
// serving the calls behind it.
void verify_a_throwing_task_leaves_the_pool_serving() {
  std::atomic<int> ran{0};
  grparse::CallExecutor executor({.workers = 1, .queue_capacity = 8});
  require(executor.submit([] { throw std::runtime_error("task failed"); }),
          "the throwing task must be accepted");
  for (int task = 0; task < 4; ++task) {
    require(executor.submit([&] { ran.fetch_add(1); }), "later tasks must be accepted");
  }
  await(ran, 4, "the pool must keep running tasks after one throws");
}

}  // namespace

int main() {
  try {
    verify_tasks_run_off_the_submitting_thread();
    verify_a_full_queue_refuses();
    verify_shutdown_drains_the_queue();
    verify_a_throwing_task_leaves_the_pool_serving();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "call-executor-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
