#include "grparse/page_scheduler.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
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

  bool try_push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || queue_.size() >= capacity_) return false;
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
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return false;
    *value = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  // Block until there is room, the queue closes, or the timeout elapses.
  bool wait_for_space(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return not_full_.wait_for(lock, timeout, [&] { return closed_ || queue_.size() < capacity_; });
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
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
  bool closed_ = false;
};

enum class PageJobState {
  kQueuedForRender,
  kRendering,
  kQueuedForInference,
  kInferencing,
  kQueuedForAssembly,
  kAssembling,
  kCompleted,
  kCancelled,
  kFailed,
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
    std::atomic<PageJobState> state{PageJobState::kQueuedForRender};
  };

  struct InferenceJob {
    std::shared_ptr<PageJob> page;
    cv::Mat image;
    std::optional<OcrPage> digital_seed;
  };

  struct AssemblyJob {
    std::shared_ptr<PageJob> page;
    std::shared_ptr<const OcrPage> result;
  };

  Impl(PageRecognizer& recognizer, Options options, PageSourceFactory source_factory)
      : recognizer_(recognizer),
        options_(options),
        source_factory_(std::move(source_factory)),
        documents_(options.document_queue_capacity),
        render_(options.render_queue_capacity),
        inference_(options.inference_queue_capacity),
        assembly_(options.assembly_queue_capacity) {
    if (!source_factory_) throw std::invalid_argument("Page source factory is required");
    if (options_.render_workers == 0 || options_.inference_workers == 0 || options_.assembly_workers == 0 ||
        options_.page_window == 0 || options_.max_active_documents == 0) {
      throw std::invalid_argument("Scheduler worker counts, page window, and document limit must be positive");
    }
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
  }

  ~Impl() {
    documents_.close();
    {
      std::lock_guard<std::mutex> lock(reschedule_mutex_);
      stopping_ = true;
    }
    reschedule_changed_.notify_all();
    render_.close();
    inference_.close();
    assembly_.close();
    if (coordinator_.joinable()) coordinator_.join();
    if (rescheduler_.joinable()) rescheduler_.join();
    for (auto& worker : render_workers_) worker.join();
    for (auto& worker : inference_workers_) worker.join();
    for (auto& worker : assembly_workers_) worker.join();
  }

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
    if (!documents_.try_push(DocumentJob{std::move(bytes), pdf, state})) {
      active_documents_.fetch_sub(1);
      documents_rejected_.fetch_add(1);
      throw SchedulerSaturated("Document scheduler admission queue is full");
    }
    documents_submitted_.fetch_add(1);
    return Ticket(state);
  }

  Metrics metrics() const {
    return Metrics{documents_submitted_.load(), documents_rejected_.load(), pages_rendered_.load(),
                   pages_read_digitally_.load(), pages_recognized_.load(), pages_cancelled_.load(),
                   documents_.size(), render_.size(), inference_.size(), assembly_.size()};
  }

 private:
  void finish_request(const std::shared_ptr<Ticket::State>& request) {
    if (!request->finish_called.exchange(true)) {
      active_documents_.fetch_sub(1);
      request->callbacks.on_finish(request->get_failure());
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

  void release_slots(const std::shared_ptr<Ticket::State>& request, size_t slots) {
    if (slots == 0 || request->finish_called.load()) return;
    {
      std::lock_guard<std::mutex> lock(request->schedule_mutex);
      request->available_slots = std::min(request->page_window, request->available_slots + slots);
    }
    queue_reschedule(request);
  }

  void schedule_pages(const std::shared_ptr<Ticket::State>& request) {
    int cancelled_pages = 0;
    bool retry = false;
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
          if (!render_.try_push(page)) {
            retry = true;
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
    if (retry) {
      // Wait for render capacity instead of spinning on a 1ms sleep.
      render_.wait_for_space(std::chrono::milliseconds(50));
      queue_reschedule(request);
    }
  }

  void reschedule_requests() {
    while (true) {
      std::shared_ptr<Ticket::State> request;
      {
        std::unique_lock<std::mutex> lock(reschedule_mutex_);
        reschedule_changed_.wait(lock, [this] { return stopping_ || !pending_reschedules_.empty(); });
        if (stopping_ && pending_reschedules_.empty()) return;
        request = std::move(pending_reschedules_.front());
        pending_reschedules_.pop_front();
      }
      request->reschedule_pending.store(false);
      schedule_pages(request);
    }
  }

  void complete_page(const std::shared_ptr<PageJob>& page, PageJobState final_state) {
    page->state.store(final_state);
    if (final_state == PageJobState::kCancelled) pages_cancelled_.fetch_add(1);
    if (page->request->cancelled.load()) queue_reschedule(page->request);
    if (page->request->remaining_pages.fetch_sub(1) == 1) finish_request(page->request);
  }

  bool enqueue_assembly(const std::shared_ptr<PageJob>& page, std::shared_ptr<const OcrPage> result) {
    page->state.store(PageJobState::kQueuedForAssembly);
    if (!assembly_.push(AssemblyJob{page, std::move(result)})) {
      page->request->fail(std::make_exception_ptr(std::runtime_error("Assembly queue closed")));
      complete_page(page, PageJobState::kFailed);
      return false;
    }
    return true;
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
    }
  }

  void render_pages() {
    std::shared_ptr<PageJob> page;
    while (render_.pop(&page)) {
      if (page->request->cancelled.load()) {
        complete_page(page, PageJobState::kCancelled);
        continue;
      }
      try {
        page->state.store(PageJobState::kRendering);
        std::optional<OcrPage> digital = page->source->extract_digital_page(page->page_number);
        if (digital.has_value()) {
          pages_read_digitally_.fetch_add(1);
          if (digital->skip_ocr) {
            auto result = std::make_shared<const OcrPage>(std::move(*digital));
            enqueue_assembly(page, std::move(result));
            continue;
          }
        }

        cv::Mat image = page->source->render_page(page->page_number);
        pages_rendered_.fetch_add(1);
        if (page->request->cancelled.load()) {
          complete_page(page, PageJobState::kCancelled);
          continue;
        }
        page->state.store(PageJobState::kQueuedForInference);
        InferenceJob job{page, std::move(image), std::move(digital)};
        if (!inference_.push(std::move(job))) {
          page->request->fail(std::make_exception_ptr(std::runtime_error("Inference queue closed")));
          complete_page(page, PageJobState::kFailed);
        }
      } catch (...) {
        page->request->fail(std::current_exception());
        complete_page(page, PageJobState::kFailed);
      }
    }
  }

  void recognize_pages() {
    InferenceJob job;
    while (inference_.pop(&job)) {
      if (job.page->request->cancelled.load()) {
        complete_page(job.page, PageJobState::kCancelled);
        continue;
      }
      try {
        job.page->state.store(PageJobState::kInferencing);
        OcrPage ocr = recognizer_.extract_page(job.image);
        job.image.release();
        pages_recognized_.fetch_add(1);

        std::shared_ptr<const OcrPage> result;
        if (job.digital_seed.has_value() && !job.digital_seed->lines.empty()) {
          result = std::make_shared<const OcrPage>(
              merge_digital_and_ocr(std::move(*job.digital_seed), std::move(ocr)));
        } else {
          result = std::make_shared<const OcrPage>(std::move(ocr));
        }
        enqueue_assembly(job.page, std::move(result));
      } catch (...) {
        job.page->request->fail(std::current_exception());
        complete_page(job.page, PageJobState::kFailed);
      }
    }
  }

  void assemble_pages() {
    AssemblyJob job;
    while (assembly_.pop(&job)) {
      if (job.page->request->cancelled.load()) {
        complete_page(job.page, PageJobState::kCancelled);
        continue;
      }
      try {
        job.page->state.store(PageJobState::kAssembling);
        const DeliveryResult delivery =
            job.page->request->callbacks.on_page(job.page->page_number, job.result);
        if (delivery == DeliveryResult::kAccepted ||
            delivery == DeliveryResult::kAcceptedAndRelease) {
          if (delivery == DeliveryResult::kAcceptedAndRelease) {
            release_slots(job.page->request, 1);
          }
          complete_page(job.page, PageJobState::kCompleted);
        } else {
          job.page->request->cancel();
          complete_page(job.page, PageJobState::kCancelled);
        }
      } catch (...) {
        job.page->request->fail(std::current_exception());
        complete_page(job.page, PageJobState::kFailed);
      }
    }
  }

  PageRecognizer& recognizer_;
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
  std::atomic<size_t> active_documents_{0};
  std::atomic<uint64_t> documents_submitted_{0};
  std::atomic<uint64_t> documents_rejected_{0};
  std::atomic<uint64_t> pages_rendered_{0};
  std::atomic<uint64_t> pages_read_digitally_{0};
  std::atomic<uint64_t> pages_recognized_{0};
  std::atomic<uint64_t> pages_cancelled_{0};
};

PageScheduler::Ticket::Ticket(std::weak_ptr<State> state) : state_(std::move(state)) {}

void PageScheduler::Ticket::cancel() const {
  if (const auto state = state_.lock()) {
    state->cancel();
    if (state->wake_scheduler) state->wake_scheduler(state);
  }
}

void PageScheduler::Ticket::release(size_t page_slots) const {
  if (const auto state = state_.lock()) {
    if (page_slots == 0 || state->finish_called.load()) return;
    {
      std::lock_guard<std::mutex> lock(state->schedule_mutex);
      state->available_slots = std::min(state->page_window, state->available_slots + page_slots);
    }
    if (state->wake_scheduler) state->wake_scheduler(state);
  }
}

bool PageScheduler::Ticket::valid() const { return !state_.expired(); }

PageScheduler::PageScheduler(PageRecognizer& recognizer, Options options, PageSourceFactory source_factory)
    : impl_(std::make_unique<Impl>(recognizer, options, std::move(source_factory))) {}

PageScheduler::~PageScheduler() = default;

PageScheduler::Ticket PageScheduler::submit(std::shared_ptr<const std::string> bytes, bool pdf,
                                            Callbacks callbacks) {
  return impl_->submit(std::move(bytes), pdf, std::move(callbacks));
}

PageScheduler::Metrics PageScheduler::metrics() const { return impl_->metrics(); }

}  // namespace grparse
