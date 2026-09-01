#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/page_scheduler.h"
#include "support/check.h"

namespace {

using namespace std::chrono_literals;

using grparse_test::require;

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
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<FakeSource>(6); });
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
  require(metrics.inference_busy_ns > 0, "inference busy time must accumulate");
  uint64_t delivered = 0;
  for (const uint64_t bucket : metrics.page_latency) delivered += bucket;
  require(delivered == 6, "every completed page must land in one latency bucket");
}

void verify_cancellation_drains_bounded_work() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 2, 1, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<FakeSource>(20); });
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
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<DigitalSource>(); });
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

// Full digital coverage but with a renderable raster, for layout labelling.
class RenderableDigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 2; }

  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    grparse::OcrPage page{100, 100,
                          {{"digital-" + std::to_string(page_number),
                            {{0, 0}, {40, 0}, {40, 8}, {0, 8}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = true;
    return page;
  }

  cv::Mat render_page(int) const override { return cv::Mat(4, 4, CV_8UC3, cv::Scalar(255)); }
};

class FakeDetector final : public grparse::RegionDetector {
 public:
  std::vector<grparse::LayoutRegion> detect_regions(const cv::Mat&) override {
    calls.fetch_add(1);
    return regions;
  }
  std::vector<grparse::LayoutRegion> regions = {grparse::LayoutRegion{"title", 0.9F, 0, 0, 50, 10}};
  std::atomic<int> calls{0};
};

void verify_layout_labels_digital_pages_without_ocr() {
  FakeRecognizer recognizer;
  FakeDetector detector;
  grparse::PageScheduler scheduler(
      recognizer, {2, 2, 2, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      },
      &detector);
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "layout-labelled digital document failed");
  require(result.completed_pages.size() == 2, "layout digital completion count");
  require(recognizer.calls.load() == 0, "full digital coverage must still bypass RapidOCR");
  require(detector.calls.load() == 2, "every page must pass through layout detection");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    for (const auto& page : delivered) {
      require(page->source == grparse::OcrPage::Source::kDigitalPdf,
              "digital text must survive the layout pass");
      require(page->regions.size() == 1 && page->regions.front().label == "title",
              "layout regions must ride on the delivered page");
    }
  }
  const auto metrics = scheduler.metrics();
  require(metrics.pages_rendered == 2, "layout requires the raster even for digital pages");
  require(metrics.pages_layout_labelled == 2, "layout metric must count labelled pages");
  require(metrics.pages_recognized == 0, "OCR remains selective under layout");
}

class FakeStructurer final : public grparse::TableStructurer {
 public:
  grparse::TableStructure recognize(const cv::Mat& crop) override {
    calls.fetch_add(1);
    crop_width.store(crop.cols);
    crop_height.store(crop.rows);
    grparse::TableStructure structure;
    structure.rows = 1;
    structure.cols = 1;
    structure.cells = {grparse::StructuredCell{0, 0, 1, 1, false, 0, 0, 2, 2}};
    return structure;
  }
  std::atomic<int> calls{0};
  std::atomic<int> crop_width{0};
  std::atomic<int> crop_height{0};
};

// Structure runs once per table region on the clipped crop, and its cells
// arrive shifted into page coordinates.
void verify_table_structure_runs_on_crops() {
  FakeRecognizer recognizer;
  FakeDetector detector;
  detector.regions = {
      grparse::LayoutRegion{"table", 0.8F, 1, 1, 3, 3},
      grparse::LayoutRegion{"title", 0.9F, 0, 0, 50, 10},
  };
  FakeStructurer structurer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 2, 2, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      },
      &detector, &structurer);
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "table-structure document failed");
  require(structurer.calls.load() == 2, "one structure call per table region per page");
  require(structurer.crop_width.load() == 2 && structurer.crop_height.load() == 2,
          "structure must see the clipped crop, not the full raster");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    for (const auto& page : delivered) {
      const auto& table = page->regions[0];
      require(table.label == "table" && table.structured_cells.size() == 1,
              "structured cells must ride on the delivered table region");
      const auto& cell = table.structured_cells[0];
      require(cell.left == 1 && cell.top == 1 && cell.right == 3 && cell.bottom == 3,
              "cells must shift from crop to page coordinates");
      require(page->regions[1].structured_cells.empty(),
              "non-table regions must stay unstructured");
    }
  }
  require(scheduler.metrics().tables_structured == 2, "structure metric must count table crops");
}

class FakeClassifier final : public grparse::FigureClassifierBase {
 public:
  std::vector<grparse::FigureClass> classify(const cv::Mat& crop) override {
    calls.fetch_add(1);
    crop_width.store(crop.cols);
    return classes;
  }
  std::vector<grparse::FigureClass> classes = {{"bar_chart", 0.9F}, {"other", 0.1F}};
  std::atomic<int> calls{0};
  std::atomic<int> crop_width{0};
};

// Classification runs once per figure region on its clipped crop and the
// sorted classes ride on the delivered region.
void verify_figure_classification_runs_on_crops() {
  FakeRecognizer recognizer;
  FakeDetector detector;
  detector.regions = {
      grparse::LayoutRegion{"picture", 0.8F, 1, 1, 3, 3},
      grparse::LayoutRegion{"title", 0.9F, 0, 0, 50, 10},
  };
  FakeClassifier classifier;
  grparse::PageScheduler scheduler(
      recognizer, {2, 2, 2, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      },
      &detector, nullptr, &classifier);
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "figure-classification document failed");
  require(classifier.calls.load() == 2, "one classification per figure region per page");
  require(classifier.crop_width.load() == 2, "classifier must see the clipped crop");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    for (const auto& page : delivered) {
      require(page->regions[0].figure_classes.size() == 2 &&
                  page->regions[0].figure_classes[0].label == "bar_chart",
              "classes must ride on the delivered figure region");
      require(page->regions[1].figure_classes.empty(),
              "non-figure regions must stay unclassified");
    }
  }
  require(scheduler.metrics().figures_classified == 2,
          "classification metric must count figure crops");
}

// With picture capture enabled, figure regions arrive carrying a PNG crop of
// the raster while other regions stay byte-free.
void verify_picture_capture_encodes_figure_crops() {
  FakeRecognizer recognizer;
  FakeDetector detector;
  detector.regions = {
      grparse::LayoutRegion{"picture", 0.8F, 0, 0, 3, 3},
      grparse::LayoutRegion{"title", 0.9F, 0, 0, 50, 10},
  };
  grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
  options.capture_picture_images = true;
  grparse::PageScheduler scheduler(
      recognizer, options,
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      },
      &detector);
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "picture-capture document failed");
  std::lock_guard<std::mutex> lock(pages_mutex);
  require(delivered.size() == 2, "picture-capture delivery count");
  for (const auto& page : delivered) {
    require(page->regions.size() == 2, "both regions must survive capture");
    const auto& figure = page->regions[0];
    require(figure.label == "picture" && figure.image_png.size() > 8,
            "figure region must carry an encoded crop");
    require(figure.image_png[0] == 0x89 && figure.image_png[1] == 'P' &&
                figure.image_png[2] == 'N' && figure.image_png[3] == 'G',
            "figure crop must be PNG-encoded");
    require(page->regions[1].image_png.empty(), "non-figure regions must stay byte-free");
  }
}

// With page capture enabled, every delivered page carries a PNG preview of
// its raster — including full-digital pages, which are rasterized for the
// preview even with no layout detector to force it.
void verify_page_capture_encodes_previews() {
  FakeRecognizer recognizer;
  grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
  options.capture_page_images = true;
  grparse::PageScheduler scheduler(
      recognizer, options,
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      });
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "page-capture document failed");
  require(scheduler.metrics().pages_rendered == 2,
          "full-digital pages must rasterize when previews are on");
  std::lock_guard<std::mutex> lock(pages_mutex);
  require(delivered.size() == 2, "page-capture delivery count");
  for (const auto& page : delivered) {
    require(page->skip_ocr, "the digital layer must survive the raster detour");
    require(page->preview_png.size() > 8, "every page must carry a preview");
    require(page->preview_png[0] == 0x89 && page->preview_png[1] == 'P' &&
                page->preview_png[2] == 'N' && page->preview_png[3] == 'G',
            "the preview must be PNG-encoded");
  }
}

// Full digital coverage rendering a page that holds the committed QR fixture,
// so barcode decoding sees real pixels.
class QrPageSource final : public grparse::PageSource {
 public:
  explicit QrPageSource(cv::Mat page) : page_(std::move(page)) {}
  int page_count() const override { return 2; }

  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    grparse::OcrPage page{page_.cols, page_.rows,
                          {{"digital-" + std::to_string(page_number),
                            {{0, 0}, {40, 0}, {40, 8}, {0, 8}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = true;
    return page;
  }

  cv::Mat render_page(int) const override { return page_.clone(); }

 private:
  cv::Mat page_;
};

// Loads the committed QR fixture and pastes it into a white page at (20, 20).
// Also reports the region box covering the pasted code, corners inclusive.
cv::Mat qr_page(grparse::LayoutRegion* qr_region) {
  const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
  const std::string path =
      std::string(data_dir == nullptr ? "tests/data" : data_dir) + "/qr_code.png";
  const cv::Mat fixture = cv::imread(path, cv::IMREAD_COLOR);
  require(!fixture.empty(), "QR fixture must load: " + path);
  cv::Mat page(fixture.rows + 40, fixture.cols + 40, CV_8UC3, cv::Scalar(255, 255, 255));
  fixture.copyTo(page(cv::Rect(20, 20, fixture.cols, fixture.rows)));
  *qr_region = grparse::LayoutRegion{"picture", 0.8F, 20, 20, 20 + fixture.cols - 1,
                                     20 + fixture.rows - 1};
  return page;
}

constexpr const char* kQrPayload = "https://github.com/krickert/gRParse/e3";

void run_qr_document(grparse::PageScheduler& scheduler,
                     std::vector<std::shared_ptr<const grparse::OcrPage>>* delivered) {
  Result result;
  std::mutex pages_mutex;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&](int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered->push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, std::move(callbacks));
  wait_until_finished(&result);
  require(!result.failure, "barcode document failed");
}

// Class-triggered decoding runs only when the classifier's top call is a
// barcode class, and the payload rides on the delivered region.
void verify_barcode_decode_triggers_on_class() {
  grparse::LayoutRegion qr_region;
  const cv::Mat page = qr_page(&qr_region);
  FakeRecognizer recognizer;
  FakeDetector detector;
  detector.regions = {qr_region, grparse::LayoutRegion{"title", 0.9F, 0, 0, 50, 10}};
  FakeClassifier classifier;
  classifier.classes = {{"qr_code", 0.95F}, {"other", 0.05F}};
  grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
  options.barcode_mode = grparse::PageScheduler::BarcodeMode::kClassTriggered;
  grparse::PageScheduler scheduler(
      recognizer, options,
      [&](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<QrPageSource>(page); },
      &detector, nullptr, &classifier);
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  run_qr_document(scheduler, &delivered);

  require(delivered.size() == 2, "barcode delivery count");
  for (const auto& delivered_page : delivered) {
    const auto& figure = delivered_page->regions[0];
    require(figure.barcodes.size() == 1, "the QR figure must carry one payload");
    require(figure.barcodes[0].format == "QRCode" && figure.barcodes[0].text == kQrPayload,
            "decoded payload mismatch");
    require(delivered_page->regions[1].barcodes.empty(), "non-figure regions must stay untouched");
  }
  require(scheduler.metrics().barcodes_decoded == 2, "barcode metric must count payloads");
}

// A non-barcode top class must skip decoding entirely, even with a code in
// the pixels; kAll decodes without any classifier at all.
void verify_barcode_gate_and_forced_mode() {
  grparse::LayoutRegion qr_region;
  const cv::Mat page = qr_page(&qr_region);
  FakeRecognizer recognizer;
  FakeDetector detector;
  detector.regions = {qr_region};
  {
    FakeClassifier classifier;  // default top class is bar_chart
    grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
    options.barcode_mode = grparse::PageScheduler::BarcodeMode::kClassTriggered;
    grparse::PageScheduler scheduler(
        recognizer, options,
        [&](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<QrPageSource>(page);
        },
        &detector, nullptr, &classifier);
    std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
    run_qr_document(scheduler, &delivered);
    for (const auto& delivered_page : delivered) {
      require(delivered_page->regions[0].barcodes.empty(),
              "a bar_chart top class must not trigger decoding");
    }
    require(scheduler.metrics().barcodes_decoded == 0, "gated decode must not count");
  }
  {
    grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
    options.barcode_mode = grparse::PageScheduler::BarcodeMode::kAll;
    grparse::PageScheduler scheduler(
        recognizer, options,
        [&](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<QrPageSource>(page);
        },
        &detector);
    std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
    run_qr_document(scheduler, &delivered);
    for (const auto& delivered_page : delivered) {
      require(delivered_page->regions[0].barcodes.size() == 1 &&
                  delivered_page->regions[0].barcodes[0].text == kQrPayload,
              "kAll must decode figure crops without a classifier");
    }
    require(scheduler.metrics().barcodes_decoded == 2, "kAll barcode metric");
  }
}

void verify_delivery_cancellation_drains_queued_work() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {1, 4, 2, 2, 2, 2, 1},
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<FakeSource>(20); });
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
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<FakeSource>(8); });
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
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<FakeSource>(6); });

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
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<PartialDigitalSource>(); });

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

// Collects each delivered page alongside the shared Result bookkeeping.
grparse::PageScheduler::Callbacks capturing_callbacks_for(
    Result* result, std::mutex* pages_mutex,
    std::vector<std::shared_ptr<const grparse::OcrPage>>* delivered) {
  auto callbacks = callbacks_for(result);
  callbacks.on_page = [result, pages_mutex, delivered](
                          int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(*pages_mutex);
      delivered->push_back(std::move(page));
    }
    std::lock_guard<std::mutex> lock(result->mutex);
    result->completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  return callbacks;
}

// kOff reads only the embedded layer: a weak layer that would normally merge
// with recognition is delivered as-is without touching the recognizer or the
// raster, and a page with no embedded layer at all assembles empty at raster
// size.
void verify_ocr_off_reads_only_the_embedded_layer() {
  {
    BodyRecognizer recognizer;
    grparse::PageScheduler scheduler(
        recognizer, {1, 2, 1, 2, 1, 1, 1},
        [](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<PartialDigitalSource>();
        });
    grparse::PageScheduler::OcrTuning tuning;
    tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kOff;
    Result result;
    std::mutex pages_mutex;
    std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
    scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                     capturing_callbacks_for(&result, &pages_mutex, &delivered));
    wait_until_finished(&result);

    require(!result.failure, "recognition-off weak-layer document failed");
    require(recognizer.calls.load() == 0, "recognition off must never invoke the recognizer");
    std::lock_guard<std::mutex> lock(pages_mutex);
    require(delivered.size() == 1, "recognition-off delivery count");
    require(delivered[0]->lines.size() == 1 && delivered[0]->lines[0].text == "header",
            "recognition off must deliver the embedded layer as-is");
    require(delivered[0]->source == grparse::OcrPage::Source::kDigitalPdf,
            "recognition off must keep the embedded source");
    require(scheduler.metrics().pages_rendered == 0,
            "an embedded-settled page needs no raster without layout or previews");
  }
  {
    BodyRecognizer recognizer;
    grparse::PageScheduler scheduler(
        recognizer, {1, 2, 1, 2, 1, 1, 1},
        [](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<FakeSource>(1);
        });
    grparse::PageScheduler::OcrTuning tuning;
    tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kOff;
    Result result;
    std::mutex pages_mutex;
    std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
    scheduler.submit(std::make_shared<const std::string>("memory"), false, tuning,
                     capturing_callbacks_for(&result, &pages_mutex, &delivered));
    wait_until_finished(&result);

    require(!result.failure, "recognition-off empty-layer document failed");
    require(recognizer.calls.load() == 0,
            "recognition off must not recognize a page without an embedded layer");
    std::lock_guard<std::mutex> lock(pages_mutex);
    require(delivered.size() == 1 && delivered[0]->lines.empty(),
            "a page with no embedded layer must assemble empty under recognition off");
    require(delivered[0]->width == 1 && delivered[0]->height == 1,
            "the empty page must keep the raster dimensions");
  }
}

// kOff with a region detector: layout still sees pixels and its regions ride
// on the delivered page, while the text stays the embedded layer alone.
void verify_ocr_off_still_runs_layout() {
  BodyRecognizer recognizer;
  FakeDetector detector;
  grparse::PageScheduler scheduler(
      recognizer, {1, 2, 1, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<PartialDigitalSource>();
      },
      &detector);
  grparse::PageScheduler::OcrTuning tuning;
  tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kOff;
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                   capturing_callbacks_for(&result, &pages_mutex, &delivered));
  wait_until_finished(&result);

  require(!result.failure, "recognition-off layout document failed");
  require(recognizer.calls.load() == 0, "layout under recognition off must not recognize");
  require(detector.calls.load() == 1, "layout must still see the raster under recognition off");
  std::lock_guard<std::mutex> lock(pages_mutex);
  require(delivered.size() == 1, "recognition-off layout delivery count");
  require(delivered[0]->lines.size() == 1 && delivered[0]->lines[0].text == "header",
          "layout under recognition off must keep the embedded text alone");
  require(delivered[0]->regions.size() == 1 && delivered[0]->regions[0].label == "title",
          "layout regions must ride on the recognition-off page");
  require(scheduler.metrics().pages_rendered == 1,
          "layout keeps the recognition-off page on the raster path");
}

// kForce recognizes every page, full-digital ones included, and the
// recognized text replaces the embedded layer instead of merging with it.
void verify_force_ocr_replaces_the_embedded_layer() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 2, 2, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RenderableDigitalSource>();
      });
  grparse::PageScheduler::OcrTuning tuning;
  tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kForce;
  Result result;
  std::mutex pages_mutex;
  std::vector<std::shared_ptr<const grparse::OcrPage>> delivered;
  scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                   capturing_callbacks_for(&result, &pages_mutex, &delivered));
  wait_until_finished(&result);

  require(!result.failure, "forced-recognition document failed");
  require(recognizer.calls.load() == 2,
          "forced recognition must run on every page, full-digital ones included");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    require(delivered.size() == 2, "forced-recognition delivery count");
    for (const auto& page : delivered) {
      require(page->lines.size() == 1 && page->lines[0].text.starts_with("page-"),
              "forced recognition must deliver the recognized text");
      require(page->source == grparse::OcrPage::Source::kOcr,
              "recognized text must replace the embedded layer, not merge with it");
    }
  }
  const auto metrics = scheduler.metrics();
  require(metrics.pages_recognized == 2 && metrics.pages_rendered == 2,
          "forced recognition must rasterize and recognize every page");
  require(metrics.pages_read_digitally == 0,
          "forced recognition must not read the embedded layer");
}

// A three-page source whose embedded layers are all weak (skip_ocr false),
// so the unrouted kSelective heuristic would recognize every page; page 3
// has no embedded layer at all.
class WeakDigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 3; }

  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    if (page_number == 3) return std::nullopt;
    grparse::OcrPage page{100, 100,
                          {{"digital-" + std::to_string(page_number),
                            {{0, 50}, {40, 50}, {40, 58}, {0, 58}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = false;  // weak coverage: unrouted selective OCRs it
    return page;
  }

  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }
};

// The pdf inspector's page set replaces the embedded-layer heuristic in
// kSelective mode: exactly the named pages recognize, every other page
// trusts its embedded layer even when that layer is weak, and a page the
// inspector cleared but Poppler reads as layerless still recognizes (an
// empty page is a worse answer than the two extractors disagreeing).
void verify_inspector_page_set_restricts_ocr() {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<WeakDigitalSource>();
      });
  grparse::PageScheduler::OcrTuning tuning;
  tuning.ocr_pages.insert(2);  // the inspector's 1-indexed answer, verbatim
  Result result;
  std::mutex pages_mutex;
  std::map<int, std::shared_ptr<const grparse::OcrPage>> delivered;
  auto callbacks = callbacks_for(&result);
  callbacks.on_page = [&result, &pages_mutex, &delivered](
                          int page_number, std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(pages_mutex);
      delivered.emplace(page_number, std::move(page));
    }
    std::lock_guard<std::mutex> lock(result.mutex);
    result.completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                   std::move(callbacks));
  wait_until_finished(&result);

  require(!result.failure, "inspector-routed document failed");
  require(recognizer.calls.load() == 2,
          "only the named page and the genuinely layerless page recognize");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    require(delivered.size() == 3, "every page still delivers");
    require(delivered.at(1)->lines.size() == 1 &&
                delivered.at(1)->lines[0].text == "digital-1" &&
                delivered.at(1)->source == grparse::OcrPage::Source::kDigitalPdf,
            "page 1 settles on its weak embedded layer without OCR");
    require(delivered.at(2)->source == grparse::OcrPage::Source::kMerged &&
                delivered.at(2)->lines.size() == 2,
            "page 2 merges its embedded layer with the recognized text");
    require(delivered.at(3)->source == grparse::OcrPage::Source::kOcr,
            "page 3 has no embedded layer, so recognition settles it alone");
  }
  const auto metrics = scheduler.metrics();
  require(metrics.pages_recognized == 2 && metrics.pages_rendered == 2,
          "only the recognized pages rasterize");
  require(metrics.pages_read_digitally == 2,
          "the embedded layers are still read for every page that has one");
}

// kForce and kOff are explicit caller overrides: they outrank the
// classification, so the page set is ignored in both.
void verify_recognition_modes_outrank_the_page_set() {
  {
    FakeRecognizer recognizer;
    grparse::PageScheduler scheduler(
        recognizer, {2, 2, 2, 2, 1, 1, 1},
        [](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<RenderableDigitalSource>();
        });
    grparse::PageScheduler::OcrTuning tuning;
    tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kForce;
    tuning.ocr_pages.insert(1);
    Result result;
    scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                     callbacks_for(&result));
    wait_until_finished(&result);
    require(!result.failure, "forced document with a page set failed");
    require(recognizer.calls.load() == 2, "kForce recognizes every page, page set or not");
  }
  {
    FakeRecognizer recognizer;
    grparse::PageScheduler scheduler(
        recognizer, {2, 2, 2, 2, 1, 1, 1},
        [](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<RenderableDigitalSource>();
        });
    grparse::PageScheduler::OcrTuning tuning;
    tuning.mode = grparse::PageScheduler::OcrTuning::Mode::kOff;
    tuning.ocr_pages.insert(1);
    Result result;
    scheduler.submit(std::make_shared<const std::string>("memory"), true, tuning,
                     callbacks_for(&result));
    wait_until_finished(&result);
    require(!result.failure, "recognition-off document with a page set failed");
    require(recognizer.calls.load() == 0, "kOff recognizes nothing, page set or not");
  }
}

// The per-document render DPI reaches the source factory: unset resolves to
// the scheduler default, a tuned value passes through untouched.
void verify_render_dpi_reaches_source_factory() {
  FakeRecognizer recognizer;
  std::atomic<double> factory_dpi{0.0};
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 2, 2, 2},
      [&factory_dpi](std::shared_ptr<const std::string>, bool, double render_dpi) {
        factory_dpi.store(render_dpi);
        return std::make_shared<FakeSource>(1);
      });

  Result defaulted;
  scheduler.submit(std::make_shared<const std::string>("memory"), false,
                   callbacks_for(&defaulted));
  wait_until_finished(&defaulted);
  require(!defaulted.failure, "default-DPI document failed");
  require(factory_dpi.load() == grparse::kDefaultRenderDpi,
          "an unset render DPI must resolve to the scheduler default");

  grparse::PageScheduler::OcrTuning tuning;
  tuning.render_dpi = 300.0;
  Result tuned;
  scheduler.submit(std::make_shared<const std::string>("memory"), false, tuning,
                   callbacks_for(&tuned));
  wait_until_finished(&tuned);
  require(!tuned.failure, "tuned-DPI document failed");
  require(factory_dpi.load() == 300.0, "a tuned render DPI must reach the source factory");
}

// Orientation recovery through the pipeline.  The source renders the
// upright 4x2 marker raster turned a quarter turn clockwise; the recognizer
// reads a portrait raster as tall boxes, a landscape raster with the marker
// top-left as upright confident lines, and one with the marker bottom-right
// as lines the classifier flipped.  Same fake as orientation_recovery_test.
constexpr unsigned char kMarker = 255;

cv::Mat upright_marker_raster() {
  cv::Mat raster(2, 4, CV_8UC1, cv::Scalar(0));
  raster.at<unsigned char>(0, 0) = kMarker;
  return raster;
}

class TurnedSource final : public grparse::PageSource {
 public:
  explicit TurnedSource(int degrees, bool digital_layer = false)
      : degrees_(degrees), digital_layer_(digital_layer) {}
  int page_count() const override { return 1; }

  std::optional<grparse::OcrPage> extract_digital_page(int) const override {
    if (!digital_layer_) return std::nullopt;
    // A partial layer: one line, not complete, so recognition still runs.
    grparse::OcrPage page{100, 100,
                          {{"digital", {{0, 0}, {40, 0}, {40, 8}, {0, 8}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = false;
    return page;
  }

  cv::Mat render_page(int) const override {
    return grparse::turn_raster(upright_marker_raster(), degrees_);
  }

 private:
  int degrees_;
  bool digital_layer_;
};

class MarkerRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat& image) override {
    calls.fetch_add(1);
    grparse::OcrPage page{image.cols, image.rows, {}};
    page.source = grparse::OcrPage::Source::kOcr;
    if (image.rows > image.cols) {
      for (int line = 0; line < 5; ++line) {
        page.lines.push_back(grparse::OcrLine{
            "tall", {{30 * line, 10}, {30 * line + 20, 10}, {30 * line + 20, 400}, {30 * line, 400}},
            0.7F, grparse::TextOrigin::kOcr});
      }
      return page;
    }
    const bool upright = image.at<unsigned char>(0, 0) == kMarker;
    for (int line = 0; line < 5; ++line) {
      grparse::OcrLine read{upright ? "upright" : "flipped",
                            {{10, 30 * line}, {400, 30 * line}, {400, 30 * line + 20}, {10, 30 * line + 20}},
                            upright ? 0.95F : 0.9F, grparse::TextOrigin::kOcr};
      read.flipped = !upright;
      page.lines.push_back(std::move(read));
    }
    return page;
  }

  std::atomic<int> calls{0};
};

struct DeliveredPage {
  std::mutex mutex;
  std::shared_ptr<const grparse::OcrPage> page;
};

grparse::PageScheduler::Callbacks capturing_callbacks(Result* result, DeliveredPage* delivered) {
  grparse::PageScheduler::Callbacks callbacks = callbacks_for(result);
  callbacks.on_page = [result, delivered](int page_number,
                                          std::shared_ptr<const grparse::OcrPage> page) {
    {
      std::lock_guard<std::mutex> lock(delivered->mutex);
      delivered->page = std::move(page);
    }
    std::lock_guard<std::mutex> lock(result->mutex);
    result->completed_pages.push_back(page_number);
    return grparse::PageScheduler::DeliveryResult::kAcceptedAndRelease;
  };
  return callbacks;
}

void verify_turned_scan_is_rerecognized_upright() {
  MarkerRecognizer recognizer;
  grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
  options.capture_page_images = true;
  grparse::PageScheduler scheduler(
      recognizer, options,
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<TurnedSource>(90); });
  Result result;
  DeliveredPage delivered;
  scheduler.submit(std::make_shared<const std::string>("memory"), false,
                   capturing_callbacks(&result, &delivered));
  wait_until_finished(&result);
  require(!result.failure, "turned scan failed");
  require(recognizer.calls.load() == 3, "one read plus one per quarter turn, never more");

  std::lock_guard<std::mutex> lock(delivered.mutex);
  require(delivered.page != nullptr, "the page must be delivered");
  require(delivered.page->rotation_degrees == 270, "a 90 clockwise feed is undone by 270");
  require(delivered.page->width == 4 && delivered.page->height == 2,
          "the delivered page is the upright raster's size");
  require(delivered.page->lines.size() == 5 && delivered.page->lines.front().text == "upright",
          "the delivered lines are the upright read");
  require(!delivered.page->preview_png.empty(), "the preview is captured");
  const cv::Mat preview = cv::imdecode(delivered.page->preview_png, cv::IMREAD_UNCHANGED);
  require(preview.cols == 4 && preview.rows == 2, "the preview is the upright raster");

  const auto metrics = scheduler.metrics();
  require(metrics.pages_recognized == 1, "pages_recognized counts pages, not passes");
  require(metrics.pages_rerecognized == 1, "one page was re-read");
  require(metrics.rerecognition_passes == 2, "two extra passes were spent");
  require(metrics.rotations_applied == std::array<uint64_t, 3>{0, 0, 1},
          "the 270 degree turn is the one counted");
}

void verify_upside_down_scan_takes_one_extra_pass() {
  MarkerRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 2, 2, 2, 1, 1, 1},
      [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<TurnedSource>(180); });
  Result result;
  DeliveredPage delivered;
  scheduler.submit(std::make_shared<const std::string>("memory"), false,
                   capturing_callbacks(&result, &delivered));
  wait_until_finished(&result);
  require(!result.failure, "upside-down scan failed");
  require(recognizer.calls.load() == 2, "an upside-down vote costs exactly one extra read");
  std::lock_guard<std::mutex> lock(delivered.mutex);
  require(delivered.page->rotation_degrees == 180 && delivered.page->lines.front().text == "upright",
          "the half turn is kept");
  const auto metrics = scheduler.metrics();
  require(metrics.rotations_applied == std::array<uint64_t, 3>{0, 1, 0} &&
              metrics.rerecognition_passes == 1,
          "the 180 degree turn is counted once");
}

// A page with a digital text layer is never re-read, whatever its raster
// looks like, and neither is any page when recovery is switched off.
void verify_digital_layer_and_disabled_recovery_never_rerecognize() {
  {
    MarkerRecognizer recognizer;
    grparse::PageScheduler scheduler(
        recognizer, {2, 2, 2, 2, 1, 1, 1},
        [](std::shared_ptr<const std::string>, bool, double) {
          return std::make_shared<TurnedSource>(90, true);
        });
    Result result;
    DeliveredPage delivered;
    scheduler.submit(std::make_shared<const std::string>("memory"), true,
                     capturing_callbacks(&result, &delivered));
    wait_until_finished(&result);
    require(!result.failure, "partial digital page failed");
    require(recognizer.calls.load() == 1, "a page with a digital layer is read once");
    std::lock_guard<std::mutex> lock(delivered.mutex);
    require(delivered.page->rotation_degrees == 0, "no turn is applied over a digital layer");
    require(delivered.page->source == grparse::OcrPage::Source::kMerged, "the layer still merges");
    require(scheduler.metrics().pages_rerecognized == 0, "nothing counted as re-read");
  }
  {
    MarkerRecognizer recognizer;
    grparse::PageScheduler::Options options{2, 2, 2, 2, 1, 1, 1};
    options.orientation.enabled = false;
    grparse::PageScheduler scheduler(
        recognizer, options,
        [](std::shared_ptr<const std::string>, bool, double) { return std::make_shared<TurnedSource>(90); });
    Result result;
    DeliveredPage delivered;
    scheduler.submit(std::make_shared<const std::string>("memory"), false,
                     capturing_callbacks(&result, &delivered));
    wait_until_finished(&result);
    require(!result.failure, "recovery-off scan failed");
    require(recognizer.calls.load() == 1, "disabled recovery reads once");
    std::lock_guard<std::mutex> lock(delivered.mutex);
    require(delivered.page->rotation_degrees == 0 && delivered.page->width == 2,
            "disabled recovery delivers the page as fed");
  }
}

int main() {
  return grparse_test::run_test_main("page-scheduler-test", {
      verify_pipeline_and_metrics,
      verify_cancellation_drains_bounded_work,
      verify_digital_pages_bypass_render_and_inference,
      verify_layout_labels_digital_pages_without_ocr,
      verify_table_structure_runs_on_crops,
      verify_figure_classification_runs_on_crops,
      verify_picture_capture_encodes_figure_crops,
      verify_page_capture_encodes_previews,
      verify_barcode_decode_triggers_on_class,
      verify_barcode_gate_and_forced_mode,
      verify_partial_digital_merges_with_ocr,
      verify_inspector_page_set_restricts_ocr,
      verify_recognition_modes_outrank_the_page_set,
      verify_ocr_off_reads_only_the_embedded_layer,
      verify_ocr_off_still_runs_layout,
      verify_force_ocr_replaces_the_embedded_layer,
      verify_render_dpi_reaches_source_factory,
      verify_delivery_cancellation_drains_queued_work,
      verify_page_credits_bound_a_document,
      verify_uncredited_document_survives_later_submissions,
      verify_turned_scan_is_rerecognized_upright,
      verify_upside_down_scan_takes_one_extra_pass,
      verify_digital_layer_and_disabled_recovery_never_rerecognize,
  });
}
