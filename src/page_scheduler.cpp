#include "grparse/page_scheduler.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grparse {
namespace {

template <typename T>
class BoundedQueue final {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("Queue capacity must be positive");
  }

  // Pushes when there is room.  On a full queue the optional waiter is recorded
  // and run exactly once by whichever pop next frees a slot; registering it
  // under the same lock that observed "full" is what makes the handoff
  // race-free, so producers never have to poll or sleep.
  bool try_push(T value, std::function<void()> on_space = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    if (queue_.size() >= capacity_) {
      if (on_space) space_waiters_.push_back(std::move(on_space));
      return false;
    }
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  bool push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
    if (closed_) return false;
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  bool pop(T* value) {
    std::vector<std::function<void()>> waiters;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
      if (queue_.empty()) return false;
      *value = std::move(queue_.front());
      queue_.pop_front();
      waiters.swap(space_waiters_);
      not_full_.notify_one();
    }
    // Outside the queue lock: waiters take scheduler locks of their own.
    for (auto& waiter : waiters) waiter();
    return true;
  }

  void close() {
    std::vector<std::function<void()>> waiters;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      waiters.swap(space_waiters_);
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& waiter : waiters) waiter();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> queue_;
  std::vector<std::function<void()>> space_waiters_;
  bool closed_ = false;
};

// Terminal disposition of a page job; every page reaches exactly one of these.
enum class PageOutcome { kCompleted, kCancelled, kFailed };

// Accumulates wall time spent inside a stage's actual page work.  Queue pushes
// stay outside these scopes: blocking on a full downstream queue is
// backpressure, and counting it as busy would hide exactly the stall the
// metric exists to expose.
class BusyTimer final {
 public:
  explicit BusyTimer(std::atomic<uint64_t>& sink)
      : sink_(sink), started_(std::chrono::steady_clock::now()) {}
  BusyTimer(const BusyTimer&) = delete;
  BusyTimer& operator=(const BusyTimer&) = delete;
  ~BusyTimer() {
    sink_.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - started_)
                                              .count()));
  }

 private:
  std::atomic<uint64_t>& sink_;
  std::chrono::steady_clock::time_point started_;
};

}  // namespace

struct PageScheduler::Ticket::State {
  explicit State(Callbacks value) : callbacks(std::move(value)) {}

  void cancel() { cancelled.store(true); }

  void fail(std::exception_ptr value) {
    {
      std::lock_guard<std::mutex> lock(failure_mutex);
      if (!failure) failure = std::move(value);
    }
    cancel();
  }

  std::exception_ptr get_failure() const {
    std::lock_guard<std::mutex> lock(failure_mutex);
    return failure;
  }

  // Single implementation of "return page credits and re-run the scheduler",
  // shared by the delivery callback and the public Ticket.
  static void release_slots(const std::shared_ptr<State>& state, size_t slots) {
    if (slots == 0 || state->finish_called.load()) return;
    {
      std::lock_guard<std::mutex> lock(state->schedule_mutex);
      state->available_slots = std::min(state->page_window, state->available_slots + slots);
    }
    if (state->wake_scheduler) state->wake_scheduler(state);
  }

  Callbacks callbacks;
  std::atomic<bool> cancelled{false};
  std::atomic<int> remaining_pages{0};
  std::atomic<bool> finish_called{false};
  std::atomic<bool> reschedule_pending{false};
  mutable std::mutex failure_mutex;
  std::exception_ptr failure;
  std::mutex schedule_mutex;
  std::shared_ptr<PageSource> source;
  int total_pages = 0;
  int next_page_to_schedule = 1;
  size_t available_slots = 0;
  size_t page_window = 0;
  std::function<void(std::shared_ptr<State>)> wake_scheduler;
};

class PageScheduler::Impl final {
 public:
  struct DocumentJob {
    std::shared_ptr<const std::string> bytes;
    bool pdf = false;
    std::shared_ptr<Ticket::State> request;
  };

  struct PageJob {
    std::shared_ptr<PageSource> source;
    std::shared_ptr<Ticket::State> request;
    int page_number = 0;
    // Set when the page enters the render queue; delivery latency is measured
    // from here so it includes every queue wait, not just compute.
    std::chrono::steady_clock::time_point scheduled_at{};
  };

  struct InferenceJob {
    std::shared_ptr<PageJob> page;
    cv::Mat image;
    std::optional<OcrPage> digital_seed;
    // False when the digital layer is complete: layout still needs the raster
    // but RapidOCR does not.
    bool run_ocr = true;
  };

  struct AssemblyJob {
    std::shared_ptr<PageJob> page;
    std::shared_ptr<const OcrPage> result;
  };

  Impl(PageRecognizer& recognizer, Options options, PageSourceFactory source_factory,
       RegionDetector* region_detector)
      : recognizer_(recognizer),
        region_detector_(region_detector),
        options_(options),
        source_factory_(std::move(source_factory)),
        documents_(options.document_queue_capacity),
        render_(options.render_queue_capacity),
        inference_(options.inference_queue_capacity),
        assembly_(options.assembly_queue_capacity) {
    if (options_.render_workers == 0 || options_.inference_workers == 0 ||
        options_.assembly_workers == 0 || options_.page_window == 0 ||
        options_.max_active_documents == 0) {
      throw std::invalid_argument("Scheduler worker counts, page window, and document limit must be positive");
    }
    if (!source_factory_) {
      const size_t parsers = options_.pdf_parsers > 0 ? options_.pdf_parsers : options_.render_workers;
      source_factory_ = [parsers](std::shared_ptr<const std::string> bytes, bool pdf) {
        return open_in_memory_document(std::move(bytes), pdf, parsers);
      };
    }
    // Any thread that fails to start must not leave the already-started ones
    // joinable at destruction time.
    try {
      coordinator_ = std::thread([this] { coordinate(); });
      rescheduler_ = std::thread([this] { reschedule_requests(); });
      for (size_t index = 0; index < options_.render_workers; ++index) {
        render_workers_.emplace_back([this] { render_pages(); });
      }
      for (size_t index = 0; index < options_.inference_workers; ++index) {
        inference_workers_.emplace_back([this] { recognize_pages(); });
      }
      for (size_t index = 0; index < options_.assembly_workers; ++index) {
        assembly_workers_.emplace_back([this] { assemble_pages(); });
      }
    } catch (...) {
      stop();
      throw;
    }
  }

  ~Impl() { stop(); }

  Ticket submit(std::shared_ptr<const std::string> bytes, bool pdf, Callbacks callbacks) {
    if (!bytes || bytes->empty()) throw InvalidDocument("Document bytes are empty");
    if (!callbacks.on_document || !callbacks.on_page || !callbacks.on_finish) {
      throw std::invalid_argument("All scheduler callbacks are required");
    }
    if (active_documents_.fetch_add(1) >= options_.max_active_documents) {
      active_documents_.fetch_sub(1);
      documents_rejected_.fetch_add(1);
      throw SchedulerSaturated("Active document limit reached");
    }
    auto state = std::make_shared<Ticket::State>(std::move(callbacks));
    state->page_window = options_.page_window;
    state->wake_scheduler = [this](std::shared_ptr<Ticket::State> request) {
      queue_reschedule(std::move(request));
    };
    // The scheduler owns the request until it finishes.  Without this registry
    // the only strong references were the in-flight page jobs, so a document
    // whose whole page window was delivered but not yet credited could be
    // destroyed underneath its own Ticket: release() then silently did nothing
    // and the document never completed.
    {
      std::lock_guard<std::mutex> lock(active_mutex_);
      active_requests_.insert(state);
    }
    if (!documents_.try_push(DocumentJob{std::move(bytes), pdf, state})) {
      {
        std::lock_guard<std::mutex> lock(active_mutex_);
        active_requests_.erase(state);
      }
      active_documents_.fetch_sub(1);
      documents_rejected_.fetch_add(1);
      throw SchedulerSaturated("Document scheduler admission queue is full");
    }
    documents_submitted_.fetch_add(1);
    return Ticket(state);
  }

  Metrics metrics() const {
    Metrics snapshot{documents_submitted_.load(),    documents_rejected_.load(),
                     pages_rendered_.load(),         pages_read_digitally_.load(),
                     pages_recognized_.load(),       pages_layout_labelled_.load(),
                     pages_cancelled_.load(),        documents_.size(),
                     render_.size(),                 inference_.size(),
                     assembly_.size(),               render_busy_ns_.load(),
                     inference_busy_ns_.load(),      assembly_busy_ns_.load(),
                     {}};
    for (size_t bucket = 0; bucket < latency_buckets_.size(); ++bucket) {
      snapshot.page_latency[bucket] = latency_buckets_[bucket].load();
    }
    return snapshot;
  }

  size_t page_window() const { return options_.page_window; }

 private:
  void stop() {
    documents_.close();
    {
      std::lock_guard<std::mutex> lock(reschedule_mutex_);
      stopping_ = true;
      pending_reschedules_.clear();
    }
    reschedule_changed_.notify_all();
    render_.close();
    inference_.close();
    assembly_.close();
    if (coordinator_.joinable()) coordinator_.join();
    if (rescheduler_.joinable()) rescheduler_.join();
    for (auto& worker : render_workers_) {
      if (worker.joinable()) worker.join();
    }
    for (auto& worker : inference_workers_) {
      if (worker.joinable()) worker.join();
    }
    for (auto& worker : assembly_workers_) {
      if (worker.joinable()) worker.join();
    }
    // No workers remain: settle anything still in flight so a caller waiting on
    // on_finish is not left hanging by a shutdown.
    std::vector<std::shared_ptr<Ticket::State>> abandoned;
    {
      std::lock_guard<std::mutex> lock(active_mutex_);
      abandoned.assign(active_requests_.begin(), active_requests_.end());
    }
    for (const auto& request : abandoned) {
      request->fail(std::make_exception_ptr(SchedulerSaturated("Scheduler is shutting down")));
      finish_request(request);
    }
  }

  void finish_request(const std::shared_ptr<Ticket::State>& request) {
    if (!request->finish_called.exchange(true)) {
      // Hold a reference of our own: the registry may own the last one.
      const std::shared_ptr<Ticket::State> owned = request;
      {
        std::lock_guard<std::mutex> lock(active_mutex_);
        active_requests_.erase(owned);
      }
      active_documents_.fetch_sub(1);
      owned->callbacks.on_finish(owned->get_failure());
    }
  }

  void queue_reschedule(std::shared_ptr<Ticket::State> request) {
    if (request->finish_called.load() || request->reschedule_pending.exchange(true)) return;
    {
      std::lock_guard<std::mutex> lock(reschedule_mutex_);
      if (stopping_) return;
      pending_reschedules_.push_back(std::move(request));
    }
    reschedule_changed_.notify_one();
  }

  void schedule_pages(const std::shared_ptr<Ticket::State>& request) {
    int cancelled_pages = 0;
    {
      std::lock_guard<std::mutex> lock(request->schedule_mutex);
      if (!request->source || request->total_pages <= 0) return;
      if (request->cancelled.load()) {
        if (request->next_page_to_schedule <= request->total_pages) {
          cancelled_pages = request->total_pages - request->next_page_to_schedule + 1;
          request->next_page_to_schedule = request->total_pages + 1;
        }
      } else {
        while (request->available_slots > 0 &&
               request->next_page_to_schedule <= request->total_pages) {
          auto page = std::make_shared<PageJob>();
          page->source = request->source;
          page->request = request;
          page->page_number = request->next_page_to_schedule;
          page->scheduled_at = std::chrono::steady_clock::now();
          // A full render queue parks this document until a render worker
          // dequeues.  The old code slept on the single scheduler thread, which
          // stalled every other document behind one saturated one.
          if (!render_.try_push(std::move(page),
                                [this, request] { queue_reschedule(request); })) {
            break;
          }
          ++request->next_page_to_schedule;
          --request->available_slots;
        }
      }
    }
    if (cancelled_pages > 0) {
      pages_cancelled_.fetch_add(static_cast<uint64_t>(cancelled_pages));
      if (request->remaining_pages.fetch_sub(cancelled_pages) == cancelled_pages) {
        finish_request(request);
      }
    }
  }

  void reschedule_requests() {
    while (true) {
      std::shared_ptr<Ticket::State> request;
      {
        std::unique_lock<std::mutex> lock(reschedule_mutex_);
        reschedule_changed_.wait(lock, [this] { return stopping_ || !pending_reschedules_.empty(); });
        if (stopping_) return;
        request = std::move(pending_reschedules_.front());
        pending_reschedules_.pop_front();
      }
      request->reschedule_pending.store(false);
      schedule_pages(request);
    }
  }

  void record_page_latency(std::chrono::steady_clock::time_point scheduled_at) {
    const auto elapsed_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now() - scheduled_at)
                                                      .count());
    size_t bucket = 0;
    while (bucket < kPageLatencyBoundsMs.size() && elapsed_ms > kPageLatencyBoundsMs[bucket]) {
      ++bucket;
    }
    latency_buckets_[bucket].fetch_add(1);
  }

  void complete_page(const std::shared_ptr<PageJob>& page, PageOutcome outcome) {
    if (outcome == PageOutcome::kCancelled) pages_cancelled_.fetch_add(1);
    if (page->request->cancelled.load()) queue_reschedule(page->request);
    if (page->request->remaining_pages.fetch_sub(1) == 1) finish_request(page->request);
  }

  void enqueue_assembly(const std::shared_ptr<PageJob>& page, std::shared_ptr<const OcrPage> result) {
    if (!assembly_.push(AssemblyJob{page, std::move(result)})) {
      page->request->fail(std::make_exception_ptr(std::runtime_error("Assembly queue closed")));
      complete_page(page, PageOutcome::kFailed);
    }
  }

  void coordinate() {
    DocumentJob document;
    while (documents_.pop(&document)) {
      if (document.request->cancelled.load()) {
        finish_request(document.request);
        continue;
      }
      try {
        auto source = source_factory_(document.bytes, document.pdf);
        if (!source) throw InvalidDocument("Document source could not be opened");
        const int pages = source->page_count();
        if (pages <= 0) throw InvalidDocument("Document does not contain a page");
        document.request->remaining_pages.store(pages);
        {
          std::lock_guard<std::mutex> lock(document.request->schedule_mutex);
          document.request->source = std::move(source);
          document.request->total_pages = pages;
          document.request->available_slots = document.request->page_window;
        }
        document.request->callbacks.on_document(pages);
        queue_reschedule(document.request);
      } catch (...) {
        document.request->fail(std::current_exception());
        if (document.request->remaining_pages.load() == 0) {
          finish_request(document.request);
        } else {
          queue_reschedule(document.request);
        }
      }
      // Release the request and its bytes before blocking on the next document.
      document = DocumentJob{};
    }
  }

  void render_pages() {
    std::shared_ptr<PageJob> page;
    while (render_.pop(&page)) {
      if (page->request->cancelled.load()) {
        complete_page(page, PageOutcome::kCancelled);
        page.reset();
        continue;
      }
      try {
        std::optional<OcrPage> digital;
        {
          const BusyTimer timer(render_busy_ns_);
          digital = page->source->extract_digital_page(page->page_number);
        }
        if (digital.has_value()) {
          pages_read_digitally_.fetch_add(1);
          if (digital->skip_ocr && region_detector_ == nullptr) {
            enqueue_assembly(page, std::make_shared<const OcrPage>(std::move(*digital)));
            page.reset();
            continue;
          }
        }
        const bool run_ocr = !(digital.has_value() && digital->skip_ocr);

        cv::Mat image;
        {
          const BusyTimer timer(render_busy_ns_);
          image = page->source->render_page(page->page_number);
        }
        pages_rendered_.fetch_add(1);
        if (page->request->cancelled.load()) {
          complete_page(page, PageOutcome::kCancelled);
          page.reset();
          continue;
        }
        InferenceJob job{page, std::move(image), std::move(digital), run_ocr};
        if (!inference_.push(std::move(job))) {
          page->request->fail(std::make_exception_ptr(std::runtime_error("Inference queue closed")));
          complete_page(page, PageOutcome::kFailed);
        }
      } catch (...) {
        page->request->fail(std::current_exception());
        complete_page(page, PageOutcome::kFailed);
      }
      page.reset();
    }
  }

  void recognize_pages() {
    InferenceJob job;
    while (inference_.pop(&job)) {
      if (job.page->request->cancelled.load()) {
        complete_page(job.page, PageOutcome::kCancelled);
        job = InferenceJob{};
        continue;
      }
      try {
        std::shared_ptr<const OcrPage> result;
        {
          const BusyTimer timer(inference_busy_ns_);
          std::vector<LayoutRegion> regions;
          if (region_detector_ != nullptr) {
            regions = region_detector_->detect_regions(job.image);
            pages_layout_labelled_.fetch_add(1);
          }

          OcrPage assembled;
          if (job.run_ocr) {
            OcrPage ocr = recognizer_.extract_page(job.image);
            pages_recognized_.fetch_add(1);
            if (job.digital_seed.has_value() && !job.digital_seed->lines.empty()) {
              assembled = merge_digital_and_ocr(std::move(*job.digital_seed), std::move(ocr));
            } else {
              assembled = std::move(ocr);
            }
          } else {
            // Full digital coverage: the raster existed only for layout.
            assembled = std::move(*job.digital_seed);
          }
          // Drop the raster the moment the device stage is done with it (B5).
          job.image.release();
          assembled.regions = std::move(regions);
          result = std::make_shared<const OcrPage>(std::move(assembled));
        }
        enqueue_assembly(job.page, std::move(result));
      } catch (...) {
        job.page->request->fail(std::current_exception());
        complete_page(job.page, PageOutcome::kFailed);
      }
      job = InferenceJob{};
    }
  }

  void assemble_pages() {
    AssemblyJob job;
    while (assembly_.pop(&job)) {
      if (job.page->request->cancelled.load()) {
        complete_page(job.page, PageOutcome::kCancelled);
        job = AssemblyJob{};
        continue;
      }
      try {
        DeliveryResult delivery = DeliveryResult::kCancelled;
        {
          const BusyTimer timer(assembly_busy_ns_);
          delivery =
              job.page->request->callbacks.on_page(job.page->page_number, std::move(job.result));
        }
        if (delivery == DeliveryResult::kAcceptedAndRelease) {
          Ticket::State::release_slots(job.page->request, 1);
        }
        if (delivery == DeliveryResult::kCancelled) {
          job.page->request->cancel();
          complete_page(job.page, PageOutcome::kCancelled);
        } else {
          record_page_latency(job.page->scheduled_at);
          complete_page(job.page, PageOutcome::kCompleted);
        }
      } catch (...) {
        job.page->request->fail(std::current_exception());
        complete_page(job.page, PageOutcome::kFailed);
      }
      job = AssemblyJob{};
    }
  }

  PageRecognizer& recognizer_;
  RegionDetector* region_detector_;
  Options options_;
  PageSourceFactory source_factory_;
  BoundedQueue<DocumentJob> documents_;
  BoundedQueue<std::shared_ptr<PageJob>> render_;
  BoundedQueue<InferenceJob> inference_;
  BoundedQueue<AssemblyJob> assembly_;
  std::thread coordinator_;
  std::thread rescheduler_;
  std::vector<std::thread> render_workers_;
  std::vector<std::thread> inference_workers_;
  std::vector<std::thread> assembly_workers_;
  std::mutex reschedule_mutex_;
  std::condition_variable reschedule_changed_;
  std::deque<std::shared_ptr<Ticket::State>> pending_reschedules_;
  bool stopping_ = false;
  mutable std::mutex active_mutex_;
  std::unordered_set<std::shared_ptr<Ticket::State>> active_requests_;
  std::atomic<size_t> active_documents_{0};
  std::atomic<uint64_t> documents_submitted_{0};
  std::atomic<uint64_t> documents_rejected_{0};
  std::atomic<uint64_t> pages_rendered_{0};
  std::atomic<uint64_t> pages_read_digitally_{0};
  std::atomic<uint64_t> pages_recognized_{0};
  std::atomic<uint64_t> pages_layout_labelled_{0};
  std::atomic<uint64_t> pages_cancelled_{0};
  std::atomic<uint64_t> render_busy_ns_{0};
  std::atomic<uint64_t> inference_busy_ns_{0};
  std::atomic<uint64_t> assembly_busy_ns_{0};
  std::array<std::atomic<uint64_t>, kPageLatencyBoundsMs.size() + 1> latency_buckets_{};
};

PageScheduler::Ticket::Ticket(std::weak_ptr<State> state) : state_(std::move(state)) {}

void PageScheduler::Ticket::cancel() const {
  if (const auto state = state_.lock()) {
    state->cancel();
    if (state->wake_scheduler) state->wake_scheduler(state);
  }
}

void PageScheduler::Ticket::release(size_t page_slots) const {
  if (const auto state = state_.lock()) State::release_slots(state, page_slots);
}

bool PageScheduler::Ticket::valid() const { return !state_.expired(); }

PageScheduler::PageScheduler(PageRecognizer& recognizer, Options options,
                             PageSourceFactory source_factory, RegionDetector* region_detector)
    : impl_(std::make_unique<Impl>(recognizer, options, std::move(source_factory),
                                   region_detector)) {}

PageScheduler::~PageScheduler() = default;

PageScheduler::Ticket PageScheduler::submit(std::shared_ptr<const std::string> bytes, bool pdf,
                                            Callbacks callbacks) {
  return impl_->submit(std::move(bytes), pdf, std::move(callbacks));
}

PageScheduler::Metrics PageScheduler::metrics() const { return impl_->metrics(); }

size_t PageScheduler::page_window() const { return impl_->page_window(); }

}  // namespace grparse
