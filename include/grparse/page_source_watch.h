#pragma once

// Ties a PageSource to the request rendering from it, for the paths that
// render outside the page scheduler (previews, VLM convert): the source gets
// the request's deadline, and once the request is gone the source's
// cancel() runs, so a render blocked on a backend ends with the request
// instead of running out its own timeout. A request that owns a
// CancelRegistry pushes that cancel; otherwise a helper thread polls `stop`.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "grparse/collector_cancel.h"
#include "grparse/in_memory_document.h"

namespace grparse {

class PageSourceWatch {
 public:
  // How often `stop` is asked.
  static constexpr std::chrono::milliseconds kPoll{100};

  // A non-null `registry` registers the source's cancel() there for the
  // life of the watch and costs no thread; `stop` is then not polled. An
  // empty `stop` without a registry watches nothing and costs no thread
  // either; the deadline is set every way. Declare it right after the
  // source it watches.
  PageSourceWatch(std::shared_ptr<PageSource> source,
                  std::chrono::system_clock::time_point deadline, std::function<bool()> stop,
                  std::shared_ptr<CancelRegistry> registry = {});
  ~PageSourceWatch();
  PageSourceWatch(const PageSourceWatch&) = delete;
  PageSourceWatch& operator=(const PageSourceWatch&) = delete;

 private:
  std::shared_ptr<CancelRegistry> registry_;
  CancelRegistry::Token token_ = 0;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace grparse
