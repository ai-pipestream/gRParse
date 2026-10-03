#pragma once

// Ties a PageSource to the request rendering from it, for the paths that
// render outside the page scheduler (previews, VLM convert): the source gets
// the request's deadline, and once `stop` answers true a helper thread calls
// cancel(), so a render blocked on a backend ends with the request instead
// of running out its own timeout.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "grparse/in_memory_document.h"

namespace grparse {

class PageSourceWatch {
 public:
  // How often `stop` is asked.
  static constexpr std::chrono::milliseconds kPoll{100};

  // An empty `stop` watches nothing and costs no thread; the deadline is
  // set either way. Declare it right after the source it watches.
  PageSourceWatch(std::shared_ptr<PageSource> source,
                  std::chrono::system_clock::time_point deadline, std::function<bool()> stop);
  ~PageSourceWatch();
  PageSourceWatch(const PageSourceWatch&) = delete;
  PageSourceWatch& operator=(const PageSourceWatch&) = delete;

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace grparse
