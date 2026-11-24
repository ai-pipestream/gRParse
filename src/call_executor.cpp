#include "grparse/call_executor.h"

#include <cxxabi.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <typeinfo>
#include <utility>

#include "grparse/prometheus_metrics.h"

namespace grparse {
namespace {

// The exception's dynamic type as the source spells it; the mangled name
// when it cannot be demangled.
std::string type_name(const std::type_info& type) {
  int status = 0;
  const std::unique_ptr<char, void (*)(void*)> demangled(
      abi::__cxa_demangle(type.name(), nullptr, nullptr, &status), std::free);
  return status == 0 && demangled != nullptr ? demangled.get() : type.name();
}

// Leaves a trace of an exception a task let escape: a count for the scrape
// and a line on stderr naming its type and message. This runs inside a catch
// that must keep the worker alive, so a failure to log (an allocation, a
// closed stderr) is itself swallowed; the count is taken first.
void note_escaped(const std::exception_ptr& failure) noexcept {
  record_executor_task_exception();
  try {
    try {
      std::rethrow_exception(failure);
    } catch (const std::exception& error) {
      std::println(stderr, "call executor: a task let {} escape: {}", type_name(typeid(error)),
                   error.what());
    } catch (...) {
      std::println(stderr, "call executor: a task let a non-standard exception escape");
    }
  } catch (...) {
  }
}

}  // namespace

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
  return submit(std::move(task), nullptr);
}

bool CallExecutor::submit(std::function<void()> task,
                          std::function<void(std::exception_ptr)> on_escape) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= queue_capacity_) return false;
    tasks_.push_back({std::move(task), std::move(on_escape)});
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
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      // Shutdown drains rather than discards: the queue holds calls, not
      // work that can simply be abandoned.
      if (tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    // A task owns an RPC and must finish it itself (status_from_exception
    // maps any throw to a status for that). Catching here keeps the worker
    // alive; it does not finish a call whose task threw. So the escape is
    // counted and logged, and a task that named what finishes its call is
    // given the exception. A handler that throws in turn has nothing left
    // to be told through.
    try {
      task.body();
    } catch (...) {
      const std::exception_ptr failure = std::current_exception();
      note_escaped(failure);
      if (task.on_escape) {
        try {
          task.on_escape(failure);
        } catch (...) {
          note_escaped(std::current_exception());
        }
      }
    }
  }
}

}  // namespace grparse
