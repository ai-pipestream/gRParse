#include "grparse/page_source_watch.h"

#include <utility>

namespace grparse {

PageSourceWatch::PageSourceWatch(std::shared_ptr<PageSource> source,
                                 std::chrono::system_clock::time_point deadline,
                                 std::function<bool()> stop,
                                 std::shared_ptr<CancelRegistry> registry) {
  if (source == nullptr) return;
  source->set_deadline(deadline);
  if (registry != nullptr) {
    registry_ = std::move(registry);
    token_ = registry_->add([source = std::move(source)] { source->cancel(); });
    return;
  }
  if (!stop) return;
  thread_ = std::thread([this, source = std::move(source), stop = std::move(stop)] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!wake_.wait_for(lock, kPoll, [this] { return stopping_; })) {
      if (stop()) {
        source->cancel();
        return;
      }
    }
  });
}

PageSourceWatch::~PageSourceWatch() {
  if (registry_ != nullptr) registry_->remove(token_);
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

}  // namespace grparse
