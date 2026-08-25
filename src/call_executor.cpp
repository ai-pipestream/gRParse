#include "grparse/call_executor.h"

#include <algorithm>
#include <utility>

namespace grparse {

CallExecutor::CallExecutor() : CallExecutor(Options{}) {}

CallExecutor::CallExecutor(Options options)
    : queue_capacity_(std::max<size_t>(1, options.queue_capacity)) {
  const size_t count = std::max<size_t>(1, options.workers);
  workers_.reserve(count);
  for (size_t worker = 0; worker < count; ++worker) {
    workers_.emplace_back([this] { run(); });
  }
}

CallExecutor::~CallExecutor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  work_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

bool CallExecutor::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= queue_capacity_) return false;
    tasks_.push_back(std::move(task));
  }
  work_.notify_one();
  return true;
}

size_t CallExecutor::queued() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

void CallExecutor::run() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      // Shutdown drains rather than discards: the queue holds calls, not
      // work that can simply be abandoned.
      if (tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    // A task owns an RPC; letting an exception escape would leave the call
    // unfinished and take the worker with it.
    try {
      task();
    } catch (...) {
    }
  }
}

}  // namespace grparse
