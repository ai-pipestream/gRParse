#pragma once

#include <atomic>
#include <cstdint>

namespace grparse {

// The process-wide ceiling on document bytes the parsing surfaces hold at
// once (GRPARSE_MAX_INFLIGHT_BYTES). Admission by call count alone lets a
// few hundred-megabyte requests per worker, or a handful of clients each
// streaming on 32 streams, outgrow the container; gRPC's own ResourceQuota
// covers transport buffers only, never a deserialized request or a stream's
// accumulated upload. A unary call is charged on admission for its request
// message plus the copy its base64 source decodes to (and, on the chunk
// surfaces, the sources they copy into the parse request); a stream is
// charged each chunk as it arrives. Either is refused with
// RESOURCE_EXHAUSTED the moment the charge would pass the ceiling.
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

}  // namespace grparse
