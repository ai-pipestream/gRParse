#pragma once

// The memory behind the async task surfaces. ConvertSourceAsync, the two
// chunk *Async RPCs and the Watch* streams register a task here; the
// executor worker that runs it records each status transition and the
// finished result; PollTaskStatus, GetConvertResult, GetChunkResult,
// ClearResults and the watchers read them back.
//
// In memory only, in keeping with the diskless rule: task ids are per
// process, and a poll after a restart answers NOT_FOUND. Bounded three
// ways: at most GRPARSE_MAX_TASKS tasks at once (finished ones included
// until they expire or are cleared), each finished task kept for
// GRPARSE_TASK_RESULT_TTL_SECONDS, and at most GRPARSE_MAX_WATCHERS
// listeners at once. A stored result is charged to the in-flight byte
// budget (GRPARSE_MAX_INFLIGHT_BYTES) for its serialized size, so it counts
// against the same ceiling the live calls do, and the charge is returned
// when the task expires or is cleared.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/inflight_bytes.h"

namespace grparse {

inline constexpr size_t kDefaultMaxTasks = 256;
inline constexpr std::chrono::seconds kDefaultTaskResultTtl{3600};
inline constexpr size_t kDefaultMaxTaskWatchers = 256;

struct TaskRegistryOptions {
  // GRPARSE_MAX_TASKS: tasks held at once, pending, running or finished.
  size_t max_tasks = kDefaultMaxTasks;
  // GRPARSE_TASK_RESULT_TTL_SECONDS: how long a finished task (its status
  // and its result) is kept after it finished.
  std::chrono::milliseconds result_ttl = kDefaultTaskResultTtl;
  // GRPARSE_MAX_WATCHERS: Watch* streams and waiting polls at once.
  size_t max_watchers = kDefaultMaxTaskWatchers;
};

class TaskRegistry final {
 public:
  using Status = ai::pipestream::parse::v1::TaskStatusPollResponse;
  // Called with every status the task takes after the call that
  // registered it, in order, on the thread that recorded it and under the
  // registry's lock: it must not block and must not call the registry.
  using Listener = std::function<void(const Status&)>;

  // `inflight` is the budget stored results are charged to; null stores
  // them uncharged.
  TaskRegistry(TaskRegistryOptions options, std::shared_ptr<InflightBytes> inflight);
  TaskRegistry(const TaskRegistry&) = delete;
  TaskRegistry& operator=(const TaskRegistry&) = delete;
  // Stops the sweeper. Call stop() first and join the workers that run
  // tasks, so nothing records into a registry that is going away.
  ~TaskRegistry();

  const TaskRegistryOptions& options() const { return options_; }

  // Registers a pending task of `kind` and returns its first status, or
  // RESOURCE_EXHAUSTED naming GRPARSE_MAX_TASKS when the registry is full,
  // UNAVAILABLE once stopped.
  std::expected<Status, grpc::Status> create(ai::pipestream::parse::v1::TaskType kind);
  // Forgets a task that was never run (its submission was refused).
  void erase(const std::string& task_id);

  // The worker's transitions: STARTED, then SUCCESS with the result or
  // FAILURE with the status that ended it. A result the in-flight budget
  // has no room for fails the task with RESOURCE_EXHAUSTED instead.
  void start(const std::string& task_id);
  void succeed(const std::string& task_id,
               ai::pipestream::parse::v1::ConvertDocumentResponse result);
  void succeed(const std::string& task_id, ai::pipestream::parse::v1::ChunkDocumentResponse result);
  void fail(const std::string& task_id, const grpc::Status& status,
            ai::pipestream::parse::v1::FailurePhase phase);

  // The task's current status; nullopt for an unknown or expired id.
  std::optional<Status> status(const std::string& task_id);

  // A finished task's result. NOT_FOUND for an unknown or expired id,
  // INVALID_ARGUMENT for a task of the other kind, FAILED_PRECONDITION
  // while it runs, and the task's own failure when it failed.
  std::expected<std::shared_ptr<const ai::pipestream::parse::v1::ConvertDocumentResponse>,
                grpc::Status>
  convert_result(const std::string& task_id);
  std::expected<std::shared_ptr<const ai::pipestream::parse::v1::ChunkDocumentResponse>,
                grpc::Status>
  chunk_result(const std::string& task_id);

  // Subscribes `listener` to the task's later statuses and returns the
  // current one, atomically, so no transition falls between them; the
  // token undoes it. NOT_FOUND for an unknown id, RESOURCE_EXHAUSTED naming
  // GRPARSE_MAX_WATCHERS when that many listen already.
  struct Subscription {
    Status current;
    uint64_t token = 0;
  };
  std::expected<Subscription, grpc::Status> watch(const std::string& task_id, Listener listener);
  // Removes a subscription; once it returns, its listener is not running
  // and never runs again. Unknown tokens are ignored.
  void unwatch(uint64_t token);

  // Drops finished tasks that finished at least `older_than` ago and
  // returns their charges; returns how many were dropped.
  size_t clear(std::chrono::milliseconds older_than);
  // Drops finished tasks past the result TTL; the sweeper runs it on its
  // own, and every call above runs it first.
  size_t sweep();

  // Refuses new tasks and raises the stop flag every task's ParseCall reads,
  // so queued and running parses end CANCELLED instead of running to the
  // end while the server shuts down.
  void stop();
  std::shared_ptr<const std::atomic<bool>> stop_flag() const { return stopping_; }

  size_t size();
  size_t watchers();

  static bool terminal(ai::pipestream::parse::v1::TaskStatus status) {
    return status == ai::pipestream::parse::v1::TASK_STATUS_SUCCESS ||
           status == ai::pipestream::parse::v1::TASK_STATUS_FAILURE;
  }

 private:
  using Clock = std::chrono::steady_clock;
  struct Task {
    Status status;
    uint64_t order = 0;
    std::optional<Clock::time_point> finished;
    grpc::Status failure;
    std::shared_ptr<const ai::pipestream::parse::v1::ConvertDocumentResponse> convert;
    std::shared_ptr<const ai::pipestream::parse::v1::ChunkDocumentResponse> chunk;
    InflightCharge charge;
  };
  struct Watcher {
    std::string task_id;
    Listener listener;
  };

  // The status as a reader sees it: the queue position filled in while the
  // task waits.
  Status snapshot_locked(const Task& task) const;
  // Records a terminal transition and tells the watchers.
  void finish_locked(Task& task, const std::string& task_id);
  void notify_locked(const std::string& task_id, const Task& task);
  size_t sweep_locked(Clock::time_point now);
  template <typename Result>
  std::expected<std::shared_ptr<const Result>, grpc::Status> result_locked(
      const std::string& task_id, ai::pipestream::parse::v1::TaskType kind,
      std::shared_ptr<const Result> Task::* field, const char* other_rpc);
  void run_sweeper();

  const TaskRegistryOptions options_;
  const std::shared_ptr<InflightBytes> inflight_;
  const std::shared_ptr<std::atomic<bool>> stopping_ = std::make_shared<std::atomic<bool>>(false);

  std::mutex mutex_;
  std::condition_variable sweeper_wake_;
  std::map<std::string, Task> tasks_;
  std::map<uint64_t, Watcher> watchers_;
  uint64_t next_order_ = 0;
  uint64_t next_token_ = 0;
  bool sweeper_done_ = false;
  // Started last, after everything it reads.
  std::thread sweeper_;
};

}  // namespace grparse
