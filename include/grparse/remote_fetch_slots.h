#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace grparse {

// GRPARSE_REMOTE_FETCH_CONCURRENCY's default.
inline constexpr std::size_t kDefaultRemoteFetchConcurrency = 4;

// The process-wide cap on remote source fetches running at once
// (GRPARSE_REMOTE_FETCH_CONCURRENCY). A remote source is a small request
// whose document is only known once fetched: while the fetch runs, the body
// and its base64 copy (up to GRPARSE_SOURCE_MAX_BYTES and a third more) are
// held before the in-flight byte budget can be charged for them, so the
// number of fetches is what bounds those bytes. A parse waits for a free
// slot on its executor worker and gives up when `stop` says so (the call
// was cancelled or its deadline passed). Slots are not handed out in
// arrival order: a freed slot wakes one waiter, and a fetch that arrives
// in the meantime may take it first, so a waiter can be overtaken.
class RemoteFetchSlots final {
 public:
  // One held slot, returned when it is destroyed or reset. Move-only; an
  // empty slot (default-constructed, moved from, or a refused acquire)
  // returns nothing. Not wrapped in std::optional: GCC's
  // -Wmaybe-uninitialized misreads an optional's inlined destructor.
  class Slot final {
   public:
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot(Slot&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    Slot& operator=(Slot&& other) noexcept {
      if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
      }
      return *this;
    }
    ~Slot() { reset(); }

    bool held() const { return owner_ != nullptr; }
    void reset() {
      if (owner_ != nullptr) std::exchange(owner_, nullptr)->release();
    }

   private:
    friend class RemoteFetchSlots;
    explicit Slot(RemoteFetchSlots* owner) : owner_(owner) {}
    RemoteFetchSlots* owner_ = nullptr;
  };

  explicit RemoteFetchSlots(std::size_t limit) : limit_(limit) {
    if (limit == 0) throw std::invalid_argument("RemoteFetchSlots needs at least one slot");
  }
  RemoteFetchSlots(const RemoteFetchSlots&) = delete;
  RemoteFetchSlots& operator=(const RemoteFetchSlots&) = delete;

  // Waits for a free slot, asking `stop` before each wait; an empty slot,
  // holding nothing, once `stop` returns true. A free slot is taken without
  // asking.
  Slot acquire(const std::function<bool()>& stop) {
    std::unique_lock lock(mutex_);
    if (in_use_ == limit_) {
      ++waiting_;
      while (in_use_ == limit_) {
        if (stop()) {
          --waiting_;
          return Slot();
        }
        freed_.wait_for(lock, kStopPoll);
      }
      --waiting_;
    }
    ++in_use_;
    return Slot(this);
  }

  std::size_t limit() const { return limit_; }
  std::size_t in_use() const {
    std::lock_guard lock(mutex_);
    return in_use_;
  }
  // The acquires waiting for a slot right now.
  std::size_t waiting() const {
    std::lock_guard lock(mutex_);
    return waiting_;
  }

 private:
  // How often a waiting parse asks whether its call is still wanted.
  static constexpr std::chrono::milliseconds kStopPoll{20};

  void release() {
    {
      std::lock_guard lock(mutex_);
      --in_use_;
    }
    freed_.notify_one();
  }

  const std::size_t limit_;
  mutable std::mutex mutex_;
  std::condition_variable freed_;
  std::size_t in_use_ = 0;
  std::size_t waiting_ = 0;
};

}  // namespace grparse
