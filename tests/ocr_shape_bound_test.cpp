// Proves, with the real OCR models on CPU, what the input buckets promise
// (compat/grparse_ocr_shapes.h):
//   - a document read as a full page plus picture regions of many sizes runs
//     the detector and the recognizer at a bounded number of distinct input
//     shapes, each one a member of the bucket lists, while the same reads
//     without buckets run a new shape for nearly every region and line;
//   - the bucketed read of tests/data/report_page.png finds the same lines
//     as the unbucketed read, with their text inside a small stated allowance.
// On a provider that compiles per input shape every distinct shape is a
// compile, so the first bound is the compile bound for a document.
//
// Model tier: skips (77) without the OCR model files, fails under
// GRPARSE_TEST_REQUIRE_MODELS=1.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/ocr_engine.h"
#include "grparse_session_ep.h"
#include "support/check.h"

namespace fs = std::filesystem;

namespace {

using grparse::OcrNet;
using grparse_test::require;

// The compile bound for the fixture document below: the six detector sizes
// its reads land on, and every listed recognizer width up to kWidestLine.
constexpr size_t kMaxDetShapes = 6;
constexpr size_t kMaxRecShapes = 14;
// No line of the fixture, scaled to the recognizer's height of 48, is wider.
constexpr int kWidestLine = 2560;

void set_buckets(const char* mode) {
  setenv("GRPARSE_OCR_SHAPE_BUCKETS", mode, 1);
  grparse::reset_ocr_shape_buckets();
}

// The reads one digital page makes in selective mode, plus the page itself:
// picture regions of every aspect and size, cut from the fixture page on a
// fixed grid so the test is deterministic.
std::vector<cv::Mat> document_reads(const cv::Mat& page) {
  std::vector<cv::Mat> reads{page};
  int index = 0;
  for (int width = 150; width <= page.cols; width += 109) {
    const int height = 120 + (index * 197) % (page.rows - 120);
    const int x = (index * 61) % (page.cols - width + 1);
    const int y = (index * 113) % (page.rows - height + 1);
    reads.push_back(page(cv::Rect(x, y, width, height)));
    ++index;
  }
  return reads;
}

struct ShapeCounts {
  size_t det = 0;
  size_t cls = 0;
  size_t rec = 0;
};

struct DocumentRead {
  ShapeCounts shapes;
  // The read of the full page, the first of the document's reads.
  grparse::OcrPage page;
};

DocumentRead read_document(grparse::OcrEngine& engine, const std::vector<cv::Mat>& reads) {
  grparse::reset_ocr_input_shapes();
  DocumentRead document;
  document.page = engine.extract_page(reads.front());
  for (size_t index = 1; index < reads.size(); ++index) engine.extract_page(reads[index]);
  document.shapes = {grparse::ocr_input_shape_count(OcrNet::kDetection),
                     grparse::ocr_input_shape_count(OcrNet::kDirection),
                     grparse::ocr_input_shape_count(OcrNet::kRecognition)};
  return document;
}

void verify_bucketed_shapes_are_bounded(const ShapeCounts& free_shapes,
                                        const ShapeCounts& bucketed) {
  require(free_shapes.det > kMaxDetShapes && free_shapes.rec >= 2 * kMaxRecShapes,
          std::format("without buckets the fixture runs many shapes (det {}, rec {}); a fixture "
                      "that does not would prove nothing",
                      free_shapes.det, free_shapes.rec));
  require(bucketed.det <= kMaxDetShapes,
          std::format("detector shapes for one document: {}, bound {}", bucketed.det, kMaxDetShapes));
  require(bucketed.rec <= kMaxRecShapes,
          std::format("recognizer shapes for one document: {}, bound {}", bucketed.rec,
                      kMaxRecShapes));
  require(kMaxRecShapes == grparse::ocr_rec_bucket_count(kWidestLine),
          "the recognizer bound is the bucket list's own count");
  require(bucketed.det <= grparse::ocr_det_bucket_count(),
          "the detector bound is within the bucket list");
  require(bucketed.cls == 1, std::format("the angle net runs one shape, saw {}", bucketed.cls));

  // Every shape that ran is a bucket: bucketing it again changes nothing.
  for (const grparse::OcrInputShape& shape : grparse::ocr_input_shapes()) {
    require(shape.batch == 1, "the CPU provider runs one item per call");
    if (shape.net == OcrNet::kDetection) {
      const grparse::OcrDetSize size{shape.width, shape.height};
      require(grparse::ocr_det_bucket(shape.width, shape.height) == size,
              std::format("detector ran {}x{}, not a bucket", shape.width, shape.height));
    } else if (shape.net == OcrNet::kRecognition) {
      require(grparse::ocr_rec_bucket(shape.width) == shape.width && shape.width <= kWidestLine,
              std::format("recognizer ran width {}, not a listed bucket", shape.width));
    }
  }
}

size_t edit_distance(std::string_view left, std::string_view right) {
  std::vector<size_t> row(right.size() + 1);
  for (size_t column = 0; column <= right.size(); ++column) row[column] = column;
  for (size_t i = 1; i <= left.size(); ++i) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= right.size(); ++j) {
      const size_t above = row[j];
      row[j] = std::min({above + 1, row[j - 1] + 1, diagonal + (left[i - 1] == right[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[right.size()];
}

// The bucketed read against the exact one, line by line: the same lines, at
// most kMaxChangedLines of them reading differently, and at most
// kMaxCharacterEdits byte edits in all.  Padding moves the detector's output
// a little (its squeeze-and-excitation layers pool over the whole input) and
// gives the recognizer a wider context, so a borderline character can read
// differently: with the default padding 2 of the page's 36 lines change by
// one byte each (a table header cell gaining or losing its space).  The
// limits leave room for another runtime build to move a different cell and
// still fail the padding this replaced, which changed 12 to 14 lines.
constexpr size_t kMaxChangedLines = 4;
constexpr size_t kMaxCharacterEdits = 8;

void verify_same_text(const char* mode, const grparse::OcrPage& exact,
                      const grparse::OcrPage& bucketed) {
  require(exact.lines.size() >= 20, "the fixture page has at least twenty text lines");
  require(bucketed.lines.size() == exact.lines.size(),
          std::format("GRPARSE_OCR_SHAPE_BUCKETS={}: {} lines, the unbucketed read has {}", mode,
                      bucketed.lines.size(), exact.lines.size()));
  size_t changed = 0;
  size_t edits = 0;
  size_t characters = 0;
  std::string first_change;
  for (size_t index = 0; index < exact.lines.size(); ++index) {
    const std::string& expected = exact.lines[index].text;
    const std::string& actual = bucketed.lines[index].text;
    characters += expected.size();
    if (actual == expected) continue;
    if (changed++ == 0) first_change = std::format("{:?} for {:?}", actual, expected);
    edits += edit_distance(actual, expected);
  }
  std::println("ocr-shape-bound-test: GRPARSE_OCR_SHAPE_BUCKETS={}: {} of {} lines changed, {} "
               "of {} bytes{}",
               mode, changed, exact.lines.size(), edits, characters,
               changed == 0 ? "" : " (first: " + first_change + ")");
  require(changed <= kMaxChangedLines && edits <= kMaxCharacterEdits,
          std::format("GRPARSE_OCR_SHAPE_BUCKETS={}: {} lines changed ({} byte edits), first {}; "
                      "limits {} and {}",
                      mode, changed, edits, first_change, kMaxChangedLines, kMaxCharacterEdits));
}

void verify_buckets(grparse::OcrEngine& engine, const cv::Mat& page) {
  const std::vector<cv::Mat> reads = document_reads(page);
  require(reads.size() >= 10, "the fixture document has at least ten reads");

  set_buckets("off");
  const DocumentRead exact = read_document(engine, reads);
  set_buckets("on");
  const DocumentRead bucketed = read_document(engine, reads);
  std::println("ocr-shape-bound-test: {} reads; distinct shapes det {} -> {}, rec {} -> {}, cls {}",
               reads.size(), exact.shapes.det, bucketed.shapes.det, exact.shapes.rec,
               bucketed.shapes.rec, bucketed.shapes.cls);
  verify_bucketed_shapes_are_bounded(exact.shapes, bucketed.shapes);

  verify_same_text("on", exact.page, bucketed.page);
  for (const char* mode : {"det", "rec"}) {
    set_buckets(mode);
    verify_same_text(mode, exact.page, engine.extract_page(page));
  }
  set_buckets("off");
}

}  // namespace

int main() {
  const char* models_dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
  const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
  const fs::path models = models_dir == nullptr ? "models" : models_dir;
  const fs::path image_path =
      fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "report_page.png";
  if (!fs::exists(models / "ch_PP-OCRv3_det_infer.onnx")) {
    std::println(stderr, "ocr-shape-bound-test: skipped, OCR models not present: {:?}",
                 models.string());
    return grparse_test::missing_model_exit_code();
  }
  return grparse_test::run_test_main("ocr-shape-bound-test", "all checks passed", {
      [&] {
        const cv::Mat page = cv::imread(image_path.string(), cv::IMREAD_COLOR);
        require(!page.empty(), "test image missing: " + image_path.string());
        grparse::OcrEngine engine(models, -1);
        verify_buckets(engine, page);
      },
  });
}
