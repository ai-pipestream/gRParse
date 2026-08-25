#include "s3_uploader.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>

#include "grparse/call_executor.h"

namespace grparse::targets {
namespace {

// The upload pool is sized for concurrent network waits, not for CPU, and it
// is deliberately separate from the conversion executor.  Both bounds are
// environment-tunable because the right numbers depend on the store, not on
// this process.
size_t pool_setting(const char* name, size_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0) return fallback;
  return static_cast<size_t>(parsed);
}

// One pool for the process.  Constructed on first use so a server that never
// sees an S3 target never starts the threads.
CallExecutor& upload_executor() {
  static CallExecutor executor({.workers = pool_setting("GRPARSE_UPLOAD_WORKERS", 4),
                                .queue_capacity = pool_setting("GRPARSE_UPLOAD_QUEUE", 32)});
  return executor;
}

// The latch the fan-out waits on: every task reports in exactly once,
// whatever became of it.
class Completion final {
 public:
  explicit Completion(size_t outstanding) : outstanding_(outstanding) {}

  void finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    --outstanding_;
    if (outstanding_ == 0) done_.notify_all();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return outstanding_ == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable done_;
  size_t outstanding_;
};

}  // namespace

std::vector<UploadedObject> upload_bundle(const S3Config& config,
                                          const std::vector<BundleFile>& files) {
  const S3Client client(config);
  std::vector<UploadedObject> written(files.size());
  if (files.empty()) return written;

  std::atomic<bool> failed{false};
  std::mutex failure_mutex;
  std::string failure;
  Completion completion(files.size());

  const auto upload_one = [&](size_t index) {
    // The first hard failure ends the batch: the tasks that have not started
    // yet skip their transfer instead of piling more work onto a store that
    // already refused one.
    if (!failed.load(std::memory_order_acquire)) {
      try {
        auto& object = written[index];
        object.key = client.key_for(files[index].path);
        object.size_bytes = files[index].bytes.size();
        object.etag = client.put_object(object.key, files[index].bytes);
      } catch (const std::exception& error) {
        if (!failed.exchange(true, std::memory_order_acq_rel)) {
          std::lock_guard<std::mutex> lock(failure_mutex);
          failure = error.what();
        }
      }
    }
    completion.finish();
  };

  CallExecutor& executor = upload_executor();
  for (size_t index = 0; index < files.size(); ++index) {
    // A bundle can hold more members than the pool's queue bounds, and the
    // queue refusing one is backpressure, not an error: the conversion's own
    // worker takes that upload itself, which is exactly the thread that would
    // otherwise be idle waiting for it.
    if (!executor.submit([&upload_one, index] { upload_one(index); })) upload_one(index);
  }
  // Waited unconditionally: the tasks reference locals of this frame, so the
  // frame cannot leave before the last of them reports in.
  completion.wait();

  if (failed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(failure_mutex);
    throw std::runtime_error(failure);
  }
  return written;
}

}  // namespace grparse::targets
