#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace grparse {

// Fixed-capacity pool of exclusively leased, expensive-to-build resources
// (ONNX Runtime sessions, Poppler documents).  Slots are filled lazily on first
// use, so a pool sized for peak concurrency costs nothing until the concurrency
// actually arrives; call prime() when construction must fail loudly at startup.
//
// Entries are built outside the pool mutex: acquiring a slot grants exclusive
// ownership of it, and the mutex handoff on release publishes the write.
template <typename T>
class ResourcePool final {
 public:
  using Factory = std::function<std::unique_ptr<T>()>;

  class Lease final {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Lease(Lease&& other) noexcept
        : pool_(other.pool_), slot_(other.slot_), value_(other.value_) {
      other.pool_ = nullptr;
      other.value_ = nullptr;
    }

    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        pool_ = other.pool_;
        slot_ = other.slot_;
        value_ = other.value_;
        other.pool_ = nullptr;
        other.value_ = nullptr;
      }
      return *this;
    }

    ~Lease() { release(); }

    T& operator*() const { return *value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

   private:
    friend class ResourcePool;
    Lease(ResourcePool* pool, size_t slot, T* value) : pool_(pool), slot_(slot), value_(value) {}

    void release() {
      if (pool_ != nullptr) {
        pool_->release(slot_);
        pool_ = nullptr;
        value_ = nullptr;
      }
    }

    ResourcePool* pool_ = nullptr;
    size_t slot_ = 0;
    T* value_ = nullptr;
  };

  ResourcePool(size_t capacity, Factory factory)
      : factory_(std::move(factory)), slots_(capacity) {
    if (capacity == 0) throw std::invalid_argument("Resource pool capacity must be positive");
    if (!factory_) throw std::invalid_argument("Resource pool factory is required");
    free_slots_.reserve(capacity);
    for (size_t slot = capacity; slot > 0; --slot) free_slots_.push_back(slot - 1);
  }

  ResourcePool(const ResourcePool&) = delete;
  ResourcePool& operator=(const ResourcePool&) = delete;

  // Blocks until a slot is free.  Propagates whatever the factory throws.
  Lease acquire() {
    size_t slot = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      available_.wait(lock, [this] { return !free_slots_.empty(); });
      slot = free_slots_.back();
      free_slots_.pop_back();
    }
    try {
      if (!slots_[slot]) {
        slots_[slot] = factory_();
        if (!slots_[slot]) throw std::runtime_error("Resource pool factory returned no resource");
        live_.fetch_add(1);
      }
    } catch (...) {
      release(slot);
      throw;
    }
    return Lease(this, slot, slots_[slot].get());
  }

  // Builds every slot up front so configuration or device failures surface at
  // startup rather than on the first request.
  void prime() {
    std::vector<Lease> leases;
    leases.reserve(slots_.size());
    for (size_t slot = 0; slot < slots_.size(); ++slot) leases.push_back(acquire());
  }

  size_t capacity() const { return slots_.size(); }
  size_t live() const { return live_.load(); }

 private:
  void release(size_t slot) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      free_slots_.push_back(slot);
    }
    available_.notify_one();
  }

  Factory factory_;
  // Sized once at construction: element addresses are stable, and each element
  // is only touched by the thread currently holding its slot.
  std::vector<std::unique_ptr<T>> slots_;
  std::vector<size_t> free_slots_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::atomic<size_t> live_{0};
};

}  // namespace grparse
