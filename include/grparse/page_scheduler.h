#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

#include "grparse/in_memory_document.h"
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
  };

  enum class DeliveryResult { kAccepted, kAcceptedAndRelease, kCancelled };

  struct Callbacks {
    std::function<void(int total_pages)> on_document;
    std::function<DeliveryResult(int page_number, std::shared_ptr<const OcrPage> page)> on_page;
    std::function<void(std::exception_ptr failure)> on_finish;
  };

  struct Metrics {
    uint64_t documents_submitted = 0;
    uint64_t documents_rejected = 0;
    uint64_t pages_rendered = 0;
    uint64_t pages_read_digitally = 0;
    uint64_t pages_recognized = 0;
    uint64_t pages_cancelled = 0;
    size_t documents_queued = 0;
    size_t pages_waiting_for_render = 0;
    size_t pages_waiting_for_inference = 0;
    size_t pages_waiting_for_assembly = 0;
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

  PageScheduler(PageRecognizer& recognizer, Options options,
                PageSourceFactory source_factory = open_in_memory_document);
  PageScheduler(const PageScheduler&) = delete;
  PageScheduler& operator=(const PageScheduler&) = delete;
  ~PageScheduler();

  Ticket submit(std::shared_ptr<const std::string> bytes, bool pdf, Callbacks callbacks);
  Metrics metrics() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace grparse
