#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "grparse/page_scheduler.h"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSource final : public grparse::PageSource {
 public:
  explicit FakeSource(int pages) : pages_(pages) {}
  int page_count() const override { return pages_; }
  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }

 private:
  int pages_;
};

class FakeRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat& image) override {
    const int page_number = image.at<unsigned char>(0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds((8 - page_number % 8) * 2));
    calls.fetch_add(1);
    return grparse::OcrPage{100, 100,
                            {{"page-" + std::to_string(page_number), {{0, 0}, {1, 0}, {1, 1}, {0, 1}}}}};
  }

  std::atomic<int> calls{0};
};

class DigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 3; }

  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    grparse::OcrPage page{100, 100,
                          {{"digital-" + std::to_string(page_number),
                            {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = true;
    return page;
  }

  cv::Mat render_page(int) const override {
    throw std::runtime_error("digital pages must not be rasterized");
  }
};

struct Result {
  std::mutex mutex;
  std::condition_variable changed;
  int total_pages = 0;
  std::vector<int> completed_pages;
  std::exception_ptr failure;
  bool finished = false;
  int finish_calls = 0;
};

grparse::PageScheduler::Callbacks callbacks_for(Result* result) {
  return {[result](int total_pages) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->total_pages = total_pages;
            result->changed.notify_all();
          },
          [result](int page_number, std::shared_ptr<const grparse::OcrPage>) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->completed_pages.push_back(page_number);
            return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
          },
          [result](std::exception_ptr failure) {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->failure = std::move(failure);
            result->finished = true;
            ++result->finish_calls;
            result->changed.notify_all();
          }};
}

void wait_until_finished(Result* result) {
  std::unique_lock<std::mutex> lock(result->mutex);
  require(result->changed.wait_for(lock, 10s, [&] { return result->finished; }), "scheduler timed out");
}

void verify_pipeline_and_metrics() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 2, 2, 2},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<FakeSource>(6); });
  Result result;
  const auto ticket = scheduler.submit(std::make_shared<const std::string>("memory"), false,
                                       callbacks_for(&result));
  require(ticket.valid(), "scheduler ticket should be live");
  wait_until_finished(&result);

  std::lock_guard<std::mutex> lock(result.mutex);
  require(!result.failure, "scheduler returned a failure");
  require(result.total_pages == 6, "scheduler page count");
  require(result.completed_pages.size() == 6, "scheduler completion count");
  const auto metrics = scheduler.metrics();
  require(metrics.documents_submitted == 1, "submitted metric");
  require(metrics.pages_rendered == 6, "rendered metric");
  require(metrics.pages_recognized == 6, "recognized metric");
}

void verify_cancellation_drains_bounded_work() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 2, 1, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<FakeSource>(20); });
  Result result;
  const auto ticket = scheduler.submit(std::make_shared<const std::string>("memory"), false,
                                       callbacks_for(&result));
  {
    std::unique_lock<std::mutex> lock(result.mutex);
    require(result.changed.wait_for(lock, 5s, [&] { return result.total_pages == 20; }),
            "scheduler did not inspect document");
  }
  ticket.cancel();
  wait_until_finished(&result);
  require(!result.failure, "cancellation should not become an internal failure");
  require(result.finish_calls == 1, "cancellation must finish exactly once");
  require(scheduler.metrics().pages_cancelled > 0, "cancelled page metric");
}

void verify_digital_pages_bypass_render_and_inference() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 2, 1, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<DigitalSource>(); });
  Result result;
  scheduler.submit(std::make_shared<const std::string>("memory"), true, callbacks_for(&result));
  wait_until_finished(&result);

  require(!result.failure, "digital scheduler returned a failure");
  require(result.completed_pages.size() == 3, "digital completion count");
  require(recognizer.calls.load() == 0, "digital text must bypass RapidOCR");
  const auto metrics = scheduler.metrics();
  require(metrics.pages_read_digitally == 3, "digital page metric");
  require(metrics.pages_rendered == 0 && metrics.pages_recognized == 0,
          "digital pages must not enter render or inference queues");
}

void verify_delivery_cancellation_drains_queued_work() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 4, 2, 2, 2, 2, 1},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<FakeSource>(20); });
  Result result;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [](int, std::shared_ptr<const grparse::OcrPage>) {
    return grparse::PageScheduler::DeliveryResult::kCancelled;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), false, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "delivery cancellation should not become an internal failure");
  require(result.finish_calls == 1, "delivery cancellation must finish exactly once");
  require(scheduler.metrics().pages_cancelled > 0, "delivery cancellation must drain queued pages");
  require(recognizer.calls.load() < 20, "delivery cancellation must stop pending inference");
}

void verify_page_credits_bound_a_document() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 4, 2, 2, 2, 2, 1, 2, 4},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<FakeSource>(8); });
  Result result;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&result](int page_number, std::shared_ptr<const grparse::OcrPage>) {
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    result.changed.notify_all();
    return grparse::PageScheduler::DeliveryResult::kAccepted;
  };
  const auto ticket = scheduler.submit(std::make_shared<const std::string>("memory"), false,
                                       std::move(callbacks));
  {
    std::unique_lock<std::mutex> lock(result.mutex);
    require(result.changed.wait_for(lock, 5s, [&] { return result.completed_pages.size() == 2; }),
            "initial page window did not complete");
  }
  std::this_thread::sleep_for(50ms);
  {
    std::lock_guard<std::mutex> lock(result.mutex);
    require(result.completed_pages.size() == 2,
            "scheduler advanced without delivery credits");
  }

  size_t released = 0;
  while (true) {
    size_t completed = 0;
    bool finished = false;
    {
      std::unique_lock<std::mutex> lock(result.mutex);
      result.changed.wait_for(lock, 5s, [&] {
        return result.finished || result.completed_pages.size() > released;
      });
      completed = result.completed_pages.size();
      finished = result.finished;
    }
    if (completed > released) {
      ticket.release(completed - released);
      released = completed;
    }
    if (finished) break;
  }
  wait_until_finished(&result);
  require(!result.failure && result.completed_pages.size() == 8,
          "credit-driven scheduler did not finish all pages");
}

// A document that has delivered its whole page window and is waiting on credits
// has no page jobs left in flight.  The scheduler must still own it: otherwise
// its state dies, the caller's Ticket goes stale, release() silently no-ops and
// the document never finishes.  Submitting a second document used to be enough
// to trigger it, because the coordinator's local job was the only owner left.
void verify_uncredited_document_survives_later_submissions() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {4, 4, 2, 2, 2, 2, 1, 2, 4},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<FakeSource>(6); });

  Result first;
  auto first_callbacks = callbacks_for(&first);
  first_callbacks.on_page = [&first](int page_number, std::shared_ptr<const grparse::OcrPage>) {
    std::lock_guard<std::mutex> lock(first.mutex);
    first.completed_pages.push_back(page_number);
    first.changed.notify_all();
    return grparse::PageScheduler::DeliveryResult::kAccepted;  // deliberately withhold credits
  };
  const auto first_ticket = scheduler.submit(std::make_shared<const std::string>("first"), false,
                                             std::move(first_callbacks));
  {
    std::unique_lock<std::mutex> lock(first.mutex);
    require(first.changed.wait_for(lock, 5s, [&] { return first.completed_pages.size() == 2; }),
            "first document did not fill its page window");
  }

  // A second document runs to completion while the first is parked on credits.
  Result second;
  scheduler.submit(std::make_shared<const std::string>("second"), false, callbacks_for(&second));
  wait_until_finished(&second);
  require(!second.failure, "second document failed");

  require(first_ticket.valid(), "an uncredited document must stay owned by the scheduler");
  size_t released = 2;
  first_ticket.release(2);
  while (true) {
    size_t completed = 0;
    bool finished = false;
    {
      std::unique_lock<std::mutex> lock(first.mutex);
      require(first.changed.wait_for(lock, 5s,
                                     [&] {
                                       return first.finished || first.completed_pages.size() > released;
                                     }),
              "parked document did not resume after credits were returned");
      completed = first.completed_pages.size();
      finished = first.finished;
    }
    if (completed > released) {
      first_ticket.release(completed - released);
      released = completed;
    }
    if (finished) break;
  }
  require(!first.failure && first.completed_pages.size() == 6,
          "parked document did not deliver every page");
}

}  // namespace


class PartialDigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 1; }

  std::optional<grparse::OcrPage> extract_digital_page(int) const override {
    grparse::OcrPage page{100, 100,
                          {{"header", {{0, 0}, {40, 0}, {40, 10}, {0, 10}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = false;  // weak coverage → must OCR + merge
    return page;
  }

  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }
};

class BodyRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    calls.fetch_add(1);
    // Non-overlapping with the digital header box at y=0..10.
    return grparse::OcrPage{
        100, 100,
        {{"body-ocr", {{0, 50}, {80, 50}, {80, 60}, {0, 60}}, 0.9F, grparse::TextOrigin::kOcr}}};
  }
  std::atomic<int> calls{0};
};

void verify_partial_digital_merges_with_ocr() {
  BodyRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 2, 1, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool) { return std::make_shared<PartialDigitalSource>(); });

  struct Capture {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const grparse::OcrPage> page;
    bool finished = false;
    std::exception_ptr failure;
  } capture;

  scheduler.submit(
      std::make_shared<const std::string>("memory"), true,
      grparse::PageScheduler::Callbacks{
          [](int) {},
          [&capture](int, std::shared_ptr<const grparse::OcrPage> page) {
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.page = std::move(page);
            return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
          },
          [&capture](std::exception_ptr failure) {
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.failure = std::move(failure);
            capture.finished = true;
            capture.changed.notify_all();
          }});

  {
    std::unique_lock<std::mutex> lock(capture.mutex);
    require(capture.changed.wait_for(lock, 10s, [&] { return capture.finished; }),
            "partial digital merge timed out");
    require(!capture.failure, "partial digital merge failed");
    require(capture.page != nullptr, "missing merged page");
    require(capture.page->source == grparse::OcrPage::Source::kMerged, "expected merged source");
    require(capture.page->lines.size() == 2, "expected digital header + OCR body");
    require(capture.page->lines[0].text == "header", "digital line should sort first");
    require(capture.page->lines[1].text == "body-ocr", "OCR line missing after merge");
    require(capture.page->lines[0].origin == grparse::TextOrigin::kDigitalPdf, "digital origin");
    require(capture.page->lines[1].origin == grparse::TextOrigin::kOcr, "ocr origin");
  }
  require(recognizer.calls.load() == 1, "partial digital must still run OCR");
  const auto metrics = scheduler.metrics();
  require(metrics.pages_read_digitally == 1, "digital metric");
  require(metrics.pages_recognized == 1, "ocr metric");
  require(metrics.pages_rendered == 1, "render metric");
}

int main() {
  try {
    verify_pipeline_and_metrics();
    verify_cancellation_drains_bounded_work();
    verify_digital_pages_bypass_render_and_inference();
    verify_partial_digital_merges_with_ocr();
    verify_delivery_cancellation_drains_queued_work();
    verify_page_credits_bound_a_document();
    verify_uncredited_document_survives_later_submissions();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "page-scheduler-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
