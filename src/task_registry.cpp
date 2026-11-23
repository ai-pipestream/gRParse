#include "task_registry.h"

#include <array>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

#include "parse_support.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// A random (version 4) UUID, the shape docling-serve's task ids take.
std::string new_task_id() {
  thread_local std::mt19937_64 generator{std::random_device{}()};
  std::array<uint64_t, 2> words{generator(), generator()};
  words[0] = (words[0] & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  words[1] = (words[1] & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
  std::array<char, 37> text{};
  std::snprintf(text.data(), text.size(), "%08llx-%04llx-%04llx-%04llx-%012llx",
                static_cast<unsigned long long>(words[0] >> 32U),
                static_cast<unsigned long long>((words[0] >> 16U) & 0xFFFFU),
                static_cast<unsigned long long>(words[0] & 0xFFFFU),
                static_cast<unsigned long long>(words[1] >> 48U),
                static_cast<unsigned long long>(words[1] & 0xFFFFFFFFFFFFULL));
  return std::string(text.data());
}

const char* task_type_name(pipestream::parse::v1::TaskType kind) {
  return kind == pipestream::parse::v1::TASK_TYPE_CHUNK ? "chunk" : "convert";
}

grpc::Status not_found(const std::string& task_id) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND,
                      "no task " + task_id +
                          " (unknown, expired, or cleared; task ids are per process and do "
                          "not survive a restart)");
}

}  // namespace

TaskRegistry::TaskRegistry(TaskRegistryOptions options, std::shared_ptr<InflightBytes> inflight)
    : options_(options), inflight_(std::move(inflight)), sweeper_([this] { run_sweeper(); }) {}

TaskRegistry::~TaskRegistry() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sweeper_done_ = true;
  }
  sweeper_wake_.notify_all();
  sweeper_.join();
}

std::expected<TaskRegistry::Status, grpc::Status> TaskRegistry::create(
    pipestream::parse::v1::TaskType kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_->load()) {
    return std::unexpected(grpc::Status(grpc::StatusCode::UNAVAILABLE, "the server is stopping"));
  }
  sweep_locked(Clock::now());
  if (tasks_.size() >= options_.max_tasks) {
    return std::unexpected(grpc::Status(
        grpc::StatusCode::RESOURCE_EXHAUSTED,
        "the server holds " + std::to_string(tasks_.size()) +
            " tasks, its GRPARSE_MAX_TASKS limit; fetch and clear finished results "
            "(ClearResults) or retry later"));
  }
  std::string task_id = new_task_id();
  while (tasks_.contains(task_id)) task_id = new_task_id();
  Task task;
  task.order = next_order_++;
  task.status.set_task_id(task_id);
  task.status.set_task_type(task_type_name(kind));
  task.status.set_task_kind(kind);
  task.status.set_task_status(pipestream::parse::v1::TASK_STATUS_PENDING);
  auto* meta = task.status.mutable_task_meta();
  meta->set_num_docs(1);
  const auto& stored = tasks_.emplace(task_id, std::move(task)).first->second;
  return snapshot_locked(stored);
}

void TaskRegistry::erase(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.erase(task_id);
}

void TaskRegistry::start(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end() || terminal(found->second.status.task_status())) return;
  found->second.status.set_task_status(pipestream::parse::v1::TASK_STATUS_STARTED);
  notify_locked(task_id, found->second);
}

void TaskRegistry::succeed(const std::string& task_id,
                           pipestream::parse::v1::ConvertDocumentResponse result) {
  const uint64_t bytes = result.ByteSizeLong();
  auto charge = InflightCharge::acquire(inflight_, bytes);
  if (!charge.has_value()) {
    fail(task_id,
         grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      "the task's " + std::to_string(bytes) +
                          " byte result would exceed GRPARSE_MAX_INFLIGHT_BYTES while stored"),
         pipestream::parse::v1::FAILURE_PHASE_ORCHESTRATION);
    return;
  }
  const bool partial =
      result.status() == pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS;
  auto stored = std::make_shared<const pipestream::parse::v1::ConvertDocumentResponse>(
      std::move(result));
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end() || terminal(found->second.status.task_status())) return;
  Task& task = found->second;
  task.convert = std::move(stored);
  task.charge = std::move(*charge);
  task.status.set_task_status(pipestream::parse::v1::TASK_STATUS_SUCCESS);
  auto* meta = task.status.mutable_task_meta();
  meta->set_num_processed(1);
  if (partial) {
    meta->set_num_partially_succeeded(1);
  } else {
    meta->set_num_succeeded(1);
  }
  finish_locked(task, task_id);
}

void TaskRegistry::succeed(const std::string& task_id,
                           pipestream::parse::v1::ChunkDocumentResponse result) {
  const uint64_t bytes = result.ByteSizeLong();
  auto charge = InflightCharge::acquire(inflight_, bytes);
  if (!charge.has_value()) {
    fail(task_id,
         grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      "the task's " + std::to_string(bytes) +
                          " byte result would exceed GRPARSE_MAX_INFLIGHT_BYTES while stored"),
         pipestream::parse::v1::FAILURE_PHASE_ORCHESTRATION);
    return;
  }
  // A chunk parse some collector failed says so on a documents entry (see
  // ChunkDocumentResponse), which is what partial means here.
  bool partial = false;
  for (const auto& document : result.documents()) {
    partial = partial ||
              document.status() == pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS;
  }
  auto stored =
      std::make_shared<const pipestream::parse::v1::ChunkDocumentResponse>(std::move(result));
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end() || terminal(found->second.status.task_status())) return;
  Task& task = found->second;
  task.chunk = std::move(stored);
  task.charge = std::move(*charge);
  task.status.set_task_status(pipestream::parse::v1::TASK_STATUS_SUCCESS);
  auto* meta = task.status.mutable_task_meta();
  meta->set_num_processed(1);
  if (partial) {
    meta->set_num_partially_succeeded(1);
  } else {
    meta->set_num_succeeded(1);
  }
  finish_locked(task, task_id);
}

void TaskRegistry::fail(const std::string& task_id, const grpc::Status& status,
                        pipestream::parse::v1::FailurePhase phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end() || terminal(found->second.status.task_status())) return;
  Task& task = found->second;
  task.failure = status;
  task.status.set_task_status(pipestream::parse::v1::TASK_STATUS_FAILURE);
  task.status.set_error_message(status.error_message());
  *task.status.mutable_failure() = public_failure(status, phase);
  auto* meta = task.status.mutable_task_meta();
  meta->set_num_processed(1);
  meta->set_num_failed(1);
  finish_locked(task, task_id);
}

std::optional<TaskRegistry::Status> TaskRegistry::status(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sweep_locked(Clock::now());
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end()) return std::nullopt;
  return snapshot_locked(found->second);
}

template <typename Result>
std::expected<std::shared_ptr<const Result>, grpc::Status> TaskRegistry::result_locked(
    const std::string& task_id, pipestream::parse::v1::TaskType kind,
    std::shared_ptr<const Result> Task::* field, const char* other_rpc) {
  sweep_locked(Clock::now());
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end()) return std::unexpected(not_found(task_id));
  const Task& task = found->second;
  if (task.status.task_kind() != kind) {
    return std::unexpected(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT,
        "task " + task_id + " is a " + task.status.task_type() + " task; read it with " +
            other_rpc));
  }
  switch (task.status.task_status()) {
    case pipestream::parse::v1::TASK_STATUS_SUCCESS:
      return task.*field;
    case pipestream::parse::v1::TASK_STATUS_FAILURE:
      return std::unexpected(task.failure);
    default:
      return std::unexpected(grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "task " + task_id + " has not finished; poll it with PollTaskStatus or watch it"));
  }
}

std::expected<std::shared_ptr<const pipestream::parse::v1::ConvertDocumentResponse>, grpc::Status>
TaskRegistry::convert_result(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_locked(task_id, pipestream::parse::v1::TASK_TYPE_CONVERT, &Task::convert,
                       "GetChunkResult");
}

std::expected<std::shared_ptr<const pipestream::parse::v1::ChunkDocumentResponse>, grpc::Status>
TaskRegistry::chunk_result(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_locked(task_id, pipestream::parse::v1::TASK_TYPE_CHUNK, &Task::chunk,
                       "GetConvertResult");
}

std::expected<TaskRegistry::Subscription, grpc::Status> TaskRegistry::watch(
    const std::string& task_id, Listener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  sweep_locked(Clock::now());
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end()) return std::unexpected(not_found(task_id));
  if (watchers_.size() >= options_.max_watchers) {
    return std::unexpected(grpc::Status(
        grpc::StatusCode::RESOURCE_EXHAUSTED,
        "the server has " + std::to_string(watchers_.size()) +
            " task watchers, its GRPARSE_MAX_WATCHERS limit; poll with PollTaskStatus instead"));
  }
  const uint64_t token = ++next_token_;
  watchers_.emplace(token, Watcher{task_id, std::move(listener)});
  return Subscription{snapshot_locked(found->second), token};
}

void TaskRegistry::unwatch(uint64_t token) {
  std::lock_guard<std::mutex> lock(mutex_);
  watchers_.erase(token);
}

size_t TaskRegistry::clear(std::chrono::milliseconds older_than) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = Clock::now();
  size_t dropped = 0;
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    if (it->second.finished.has_value() && now - *it->second.finished >= older_than) {
      it = tasks_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

size_t TaskRegistry::sweep() {
  std::lock_guard<std::mutex> lock(mutex_);
  return sweep_locked(Clock::now());
}

void TaskRegistry::stop() {
  stopping_->store(true, std::memory_order_release);
}

size_t TaskRegistry::size() {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

size_t TaskRegistry::watchers() {
  std::lock_guard<std::mutex> lock(mutex_);
  return watchers_.size();
}

TaskRegistry::Status TaskRegistry::snapshot_locked(const Task& task) const {
  Status status = task.status;
  if (status.task_status() == pipestream::parse::v1::TASK_STATUS_PENDING) {
    int64_t ahead = 0;
    for (const auto& [id, other] : tasks_) {
      if (other.order < task.order &&
          other.status.task_status() == pipestream::parse::v1::TASK_STATUS_PENDING) {
        ++ahead;
      }
    }
    status.set_task_position(ahead);
  }
  return status;
}

void TaskRegistry::finish_locked(Task& task, const std::string& task_id) {
  task.finished = Clock::now();
  notify_locked(task_id, task);
  // The sweeper may be asleep with no deadline, or a later one.
  sweeper_wake_.notify_all();
}

void TaskRegistry::notify_locked(const std::string& task_id, const Task& task) {
  const Status status = snapshot_locked(task);
  for (const auto& [token, watcher] : watchers_) {
    if (watcher.task_id == task_id) watcher.listener(status);
  }
}

size_t TaskRegistry::sweep_locked(Clock::time_point now) {
  size_t dropped = 0;
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    if (it->second.finished.has_value() && now - *it->second.finished >= options_.result_ttl) {
      it = tasks_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

void TaskRegistry::run_sweeper() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!sweeper_done_) {
    sweep_locked(Clock::now());
    std::optional<Clock::time_point> next;
    for (const auto& [id, task] : tasks_) {
      if (!task.finished.has_value()) continue;
      const auto expiry = *task.finished + options_.result_ttl;
      if (!next.has_value() || expiry < *next) next = expiry;
    }
    if (next.has_value()) {
      sweeper_wake_.wait_until(lock, *next);
    } else {
      sweeper_wake_.wait(lock);
    }
  }
}

}  // namespace grparse
