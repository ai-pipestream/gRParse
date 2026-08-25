#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace grparse {

// The thread pool a gRPC callback handler hands its blocking work to.  gRPC's
// callback API runs reactors on event-manager threads, and a conversion waits
// on the page scheduler and on remote collectors for as long as the document
// takes; doing that inline would pin an event thread and stall every other
// call sharing it.  Workers here may spend nearly all their time blocked, so
// the pool size bounds concurrent conversions, not CPU use.
class CallExecutor final {
 public:
  struct Options {
    size_t workers = 16;
    // Tasks that may wait for a free worker.  Past it submit() refuses, so a
    // saturated server answers RESOURCE_EXHAUSTED instead of growing a queue
    // of calls whose deadlines have already passed.
    size_t queue_capacity = 64;
  };

  // Two constructors rather than a defaulted argument: Options carries member
  // initializers, which a nested type cannot supply inside its own enclosing
  // class definition.
  CallExecutor();
  explicit CallExecutor(Options options);
  CallExecutor(const CallExecutor&) = delete;
  CallExecutor& operator=(const CallExecutor&) = delete;
  // Drains the queue and joins every worker.  Queued tasks still run: each one
  // owns a live RPC that must be finished, and dropping it would leave the
  // call hanging until its deadline.
  //
  // The drain is bounded by the work, not by a clock: a worker already inside
  // a collector leg is joined only when that leg's deadline expires, and the
  // longest cap is the ASR one at 30 minutes.  So SIGTERM can hold the
  // process for as long as the slowest leg still in flight, well past the
  // server's own 10 s Shutdown deadline (which force-cancels the inbound
  // calls but cannot reach the outbound ones).  Every leg inherits its
  // inbound call's deadline, so the bound in practice is the longest
  // client-set deadline among the calls in flight; an orchestrator grace
  // period shorter than that ends in SIGKILL.
  ~CallExecutor();

  // Queues `task`, or returns false when the queue is full or the pool is
  // shutting down.  The rejection belongs to the caller: it holds the call and
  // is the only thing that can answer it.
  bool submit(std::function<void()> task);

  size_t workers() const { return workers_.size(); }
  // Tasks queued but not yet picked up.  Sampled, so it is a gauge, not a
  // synchronization point.
  size_t queued() const;

 private:
  void run();

  mutable std::mutex mutex_;
  std::condition_variable work_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  const size_t queue_capacity_;
  bool stopping_ = false;
};

}  // namespace grparse
