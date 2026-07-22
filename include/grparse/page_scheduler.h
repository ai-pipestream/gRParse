#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

#include "grparse/in_memory_document.h"
#include "grparse/layout_engine.h"
#include "grparse/ocr_engine.h"
#include "grparse/ocr_types.h"

namespace grparse {

class SchedulerSaturated final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class PageScheduler final {
 public:
  struct Options {
    size_t document_queue_capacity = 8;
    size_t render_queue_capacity = 8;
    size_t inference_queue_capacity = 4;
    size_t assembly_queue_capacity = 8;
    size_t render_workers = 2;
    size_t inference_workers = 1;
    size_t assembly_workers = 2;
    size_t page_window = 4;
    size_t max_active_documents = 32;
    // Concurrent Poppler parsers per PDF for the built-in page source.
    // 0 tracks render_workers, which is what keeps render fan-out real.
    size_t pdf_parsers = 0;
    // PNG-encode figure crops onto their regions in the inference stage.
    // Off by default: image bytes inflate every page event that has figures.
    bool capture_picture_images = false;
  };

  enum class DeliveryResult { kAccepted, kAcceptedAndRelease, kCancelled };

  struct Callbacks {
    std::function<void(int total_pages)> on_document;
    std::function<DeliveryResult(int page_number, std::shared_ptr<const OcrPage> page)> on_page;
    std::function<void(std::exception_ptr failure)> on_finish;
  };

  // Upper bounds in milliseconds for Metrics::page_latency buckets; the last
  // bucket counts everything slower than the final bound.
  static constexpr std::array<uint64_t, 9> kPageLatencyBoundsMs = {25,   50,   100,  250, 500,
                                                                   1000, 2500, 5000, 10000};

  struct Metrics {
    uint64_t documents_submitted = 0;
    uint64_t documents_rejected = 0;
    uint64_t pages_rendered = 0;
    uint64_t pages_read_digitally = 0;
    uint64_t pages_recognized = 0;
    // Pages that went through layout region detection.
    uint64_t pages_layout_labelled = 0;
    uint64_t pages_cancelled = 0;
    size_t documents_queued = 0;
    size_t pages_waiting_for_render = 0;
    size_t pages_waiting_for_inference = 0;
    size_t pages_waiting_for_assembly = 0;
    // Cumulative nanoseconds each stage's workers spent doing page work (not
    // blocked on queues).  Divide the delta between two samples by the sample
    // interval times the stage's worker count for a busy fraction; render and
    // inference both being busy is the anti-seesaw overlap made measurable.
    uint64_t render_busy_ns = 0;
    uint64_t inference_busy_ns = 0;
    uint64_t assembly_busy_ns = 0;
    // Completed pages by schedule-to-delivered latency, kPageLatencyBoundsMs
    // bucket bounds plus one overflow bucket.
    std::array<uint64_t, kPageLatencyBoundsMs.size() + 1> page_latency = {};
  };

  class Ticket {
   public:
    Ticket() = default;
    void cancel() const;
    void release(size_t page_slots = 1) const;
    bool valid() const;

   private:
    struct State;
    friend class PageScheduler;
    explicit Ticket(std::weak_ptr<State> state);
    std::weak_ptr<State> state_;
  };

  // An empty source_factory installs the in-memory PDF/image source sized from
  // Options.  A null region_detector disables layout labelling; when present,
  // every page (including full-digital ones) is rasterized so layout can see
  // pixels, and OCR remains selective.
  PageScheduler(PageRecognizer& recognizer, Options options,
                PageSourceFactory source_factory = PageSourceFactory{},
                RegionDetector* region_detector = nullptr);
  PageScheduler(const PageScheduler&) = delete;
  PageScheduler& operator=(const PageScheduler&) = delete;
  ~PageScheduler();

  Ticket submit(std::shared_ptr<const std::string> bytes, bool pdf, Callbacks callbacks);
  Metrics metrics() const;
  // Pages a single document may hold undelivered before it needs credits back.
  // Stream reactors must size their own buffers from this, not from a constant.
  size_t page_window() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace grparse
