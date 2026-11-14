// Proves the second, sharper view of a document used to OCR image
// rectangles: the source opens once, on first use, and never after a cancel;
// a failed open or render gives an empty raster (the caller then reads the
// page raster); cancel reaches an open source; rectangles and OCR polygons
// map between the base raster and the sharper one.

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include <opencv2/core.hpp>

#include "grparse/ocr_rect_render.h"
#include "support/check.h"

namespace {

using grparse_test::require;

class FakeSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 3; }
  cv::Mat render_page(int page_number) const override {
    ++renders;
    if (page_number == 3) throw std::runtime_error("render failed");
    return cv::Mat(30, 20, CV_8UC3, cv::Scalar(page_number, 0, 0));
  }
  void cancel() override { cancelled = true; }
  mutable std::atomic<int> renders{0};
  bool cancelled = false;
};

void verify_the_source_opens_once_and_renders_pages() {
  int opens = 0;
  auto fake = std::make_shared<FakeSource>();
  grparse::OcrRectRenderer renderer([&] { ++opens; return fake; }, 200.0, 300.0);
  require(opens == 0, "nothing opens before the first render");
  const cv::Mat first = renderer.render(1);
  const cv::Mat second = renderer.render(2);
  require(opens == 1, "the second source opens once");
  require(first.rows == 30 && second.at<cv::Vec3b>(0, 0)[0] == 2, "each page renders from it");
  require(renderer.render(3).empty(), "a failed render gives an empty raster");
  require(renderer.scale() == 1.5, "300 over 200 DPI is a 1.5 scale");
  renderer.cancel();
  require(fake->cancelled, "cancel reaches the open source");
  require(renderer.render(1).empty(), "a cancelled renderer renders nothing");
}

void verify_open_failures_and_early_cancel() {
  grparse::OcrRectRenderer none([] { return std::shared_ptr<grparse::PageSource>(); }, 200.0, 300.0);
  require(none.render(1).empty(), "a source that does not open gives an empty raster");
  grparse::OcrRectRenderer throwing(
      []() -> std::shared_ptr<grparse::PageSource> { throw std::runtime_error("backend down"); }, 200.0, 300.0);
  require(throwing.render(1).empty(), "an open that throws gives an empty raster");
  int opens = 0;
  grparse::OcrRectRenderer early([&] { ++opens; return std::make_shared<FakeSource>(); }, 200.0, 300.0);
  early.cancel();
  require(early.render(1).empty() && opens == 0, "a cancel before first use never opens the source");
}

void verify_rectangles_scale_and_clamp() {
  require(grparse::scaled_rect(cv::Rect(100, 200, 40, 20), 1.5, cv::Size(2550, 3300)) ==
              cv::Rect(150, 300, 60, 30),
          "a rectangle scales with the raster");
  require(grparse::scaled_rect(cv::Rect(1650, 2150, 100, 100), 1.5, cv::Size(2550, 3300)) ==
              cv::Rect(2475, 3225, 75, 75),
          "and is clamped to the sharper raster");
}

void verify_reads_come_back_to_the_base_raster() {
  grparse::OcrPage read;
  grparse::OcrLine line;
  line.text = "Sensor signals";
  line.polygon = {{30, 15}, {90, 15}, {90, 45}, {30, 45}};
  read.lines.push_back(line);
  grparse::scale_read(&read, 1.5);
  require(read.lines[0].polygon[0] == cv::Point(20, 10) && read.lines[0].polygon[2] == cv::Point(60, 30),
          "polygon points divide by the scale");
  grparse::scale_read(nullptr, 1.5);
}

void verify_the_factory_only_builds_for_a_sharper_pdf_view() {
  double seen_dpi = 0.0;
  const grparse::PageSourceFactory factory =
      [&](std::shared_ptr<const std::string>, bool, double dpi, const std::string&) {
        seen_dpi = dpi;
        return std::shared_ptr<grparse::PageSource>(std::make_shared<FakeSource>());
      };
  const auto bytes = std::make_shared<const std::string>("%PDF-1.7");
  const auto never = std::chrono::system_clock::time_point::max();
  require(grparse::make_ocr_rect_renderer(factory, bytes, false, 200.0, 300.0, "", never, {}) == nullptr,
          "a raster input reads the page raster");
  require(grparse::make_ocr_rect_renderer(factory, bytes, true, 200.0, 200.0, "", never, {}) == nullptr,
          "an OCR DPI at the render DPI reads the page raster");
  const auto renderer = grparse::make_ocr_rect_renderer(factory, bytes, true, 200.0, 300.0, "", never, {});
  require(renderer != nullptr && seen_dpi == 0.0, "a PDF gets a renderer, opened lazily");
  require(!renderer->render(1).empty() && seen_dpi == 300.0, "which opens the PDF at the OCR DPI");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-rect-render-test", "all checks passed", {
      verify_the_source_opens_once_and_renders_pages,
      verify_open_failures_and_early_cancel,
      verify_rectangles_scale_and_clamp,
      verify_reads_come_back_to_the_base_raster,
      verify_the_factory_only_builds_for_a_sharper_pdf_view,
  });
}
