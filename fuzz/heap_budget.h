// A heap budget for one fuzz input: the peak bytes the harness thread holds
// on the heap while the door runs, measured with the sanitizer allocator
// hooks. A door that turns a few header fields into hundreds of megabytes
// (#225: a looping DIFAT chain inflating the compound-file FAT) never
// crashes, so without a budget libFuzzer would only notice it past
// -rss_limit_mb, and then only for inputs large enough to cross it. The
// budget is linear in the input: what the doors may legitimately build
// (chains, directory entries, a copy of a stream head) is bounded by the
// bytes they were given.
#ifndef GRPARSE_FUZZ_HEAP_BUDGET_H
#define GRPARSE_FUZZ_HEAP_BUDGET_H

#include <sanitizer/allocator_interface.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace grparse_fuzz {

namespace heap_detail {

// Thread-local so the libFuzzer RSS thread's own allocations never count.
inline thread_local bool armed = false;
inline thread_local long long live = 0;
inline thread_local long long peak = 0;

inline void on_malloc(const volatile void* /*ptr*/, size_t size) {
  if (!armed) return;
  live += static_cast<long long>(size);
  if (live > peak) peak = live;
}

inline void on_free(const volatile void* ptr) {
  if (!armed || ptr == nullptr) return;
  // The hook runs before the chunk is released, so its size is still known.
  live -= static_cast<long long>(
      __sanitizer_get_allocated_size(const_cast<const void*>(ptr)));
}

}  // namespace heap_detail

// Call once from LLVMFuzzerInitialize.
inline void install_heap_budget() {
  if (__sanitizer_install_malloc_and_free_hooks(heap_detail::on_malloc,
                                                heap_detail::on_free) == 0) {
    std::fprintf(stderr, "heap budget: the sanitizer refused the allocator hooks\n");
    std::abort();
  }
}

// Measures the peak heap the scope's thread holds above where it started,
// and aborts (a libFuzzer crash, with the input saved) when it passes
// `per_input_byte` times the input size plus `fixed_bytes`.
class HeapBudget {
 public:
  HeapBudget(size_t input_size, size_t per_input_byte, size_t fixed_bytes)
      : limit_(static_cast<long long>(input_size * per_input_byte + fixed_bytes)) {
    heap_detail::live = 0;
    heap_detail::peak = 0;
    heap_detail::armed = true;
  }
  ~HeapBudget() {
    heap_detail::armed = false;
    if (heap_detail::peak > limit_) {
      std::fprintf(stderr,
                   "heap budget: the door held %lld bytes at its peak, over the "
                   "%lld-byte budget for this input\n",
                   heap_detail::peak, limit_);
      std::abort();
    }
  }
  HeapBudget(const HeapBudget&) = delete;
  HeapBudget& operator=(const HeapBudget&) = delete;

 private:
  long long limit_;
};

}  // namespace grparse_fuzz

#endif
