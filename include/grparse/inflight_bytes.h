#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace grparse {

// The process-wide ceiling on document bytes the parsing surfaces hold at
// once (GRPARSE_MAX_INFLIGHT_BYTES). Admission by call count alone lets a
// few hundred-megabyte requests per worker, or a handful of clients each
// streaming on 32 streams, outgrow the container; gRPC's own ResourceQuota
// covers transport buffers only, never a deserialized request or a stream's
// accumulated upload. A unary call is charged on admission for its request
// message plus the copy its base64 source decodes to (and, on the chunk
// surfaces, the sources they copy into the parse request); a stream is
// charged each chunk as it arrives. A remote source (S3, HTTP, the stores,
// a plugin) is a small request that only learns its size once fetched, so
// the parse charges the fetched copy separately the moment the fetch
// returns (InflightCharge). Either is refused with RESOURCE_EXHAUSTED the
// moment the charge would pass the ceiling.
class InflightBytes final {
 public:
  explicit InflightBytes(uint64_t limit) : limit_(limit) {}
  InflightBytes(const InflightBytes&) = delete;
  InflightBytes& operator=(const InflightBytes&) = delete;

  // Charges `bytes` and returns true, or charges nothing and returns false
  // when the charge would take the total past the limit.
  bool try_acquire(uint64_t bytes) {
    uint64_t used = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > limit_ || used > limit_ - bytes) return false;
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
  }

  // Returns an earlier charge.
  void release(uint64_t bytes) { used_.fetch_sub(bytes, std::memory_order_relaxed); }

  uint64_t limit() const { return limit_; }
  uint64_t in_use() const { return used_.load(std::memory_order_relaxed); }

 private:
  const uint64_t limit_;
  std::atomic<uint64_t> used_{0};
};

// One charge against an InflightBytes budget, returned when the charge is
// destroyed, so every exit path gives it back. Move-only; an empty charge
// (default-constructed, moved from, or taken against a null budget) returns
// nothing.
class InflightCharge final {
 public:
  InflightCharge() = default;
  InflightCharge(const InflightCharge&) = delete;
  InflightCharge& operator=(const InflightCharge&) = delete;
  InflightCharge(InflightCharge&& other) noexcept
      : budget_(std::move(other.budget_)), bytes_(std::exchange(other.bytes_, 0)) {}
  InflightCharge& operator=(InflightCharge&& other) noexcept {
    if (this != &other) {
      release();
      budget_ = std::move(other.budget_);
      bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
  }
  ~InflightCharge() { release(); }

  // Charges `bytes` to `budget`; nullopt, with nothing charged, when the
  // budget refuses it. A null budget admits everything and charges nothing.
  static std::optional<InflightCharge> acquire(std::shared_ptr<InflightBytes> budget,
                                               uint64_t bytes) {
    if (budget == nullptr) return InflightCharge();
    if (!budget->try_acquire(bytes)) return std::nullopt;
    return InflightCharge(std::move(budget), bytes);
  }

  uint64_t bytes() const { return bytes_; }

 private:
  InflightCharge(std::shared_ptr<InflightBytes> budget, uint64_t bytes)
      : budget_(std::move(budget)), bytes_(bytes) {}

  void release() {
    if (budget_ != nullptr) budget_->release(bytes_);
    budget_.reset();
    bytes_ = 0;
  }

  std::shared_ptr<InflightBytes> budget_;
  uint64_t bytes_ = 0;
};

}  // namespace grparse
