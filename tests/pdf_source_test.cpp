// Exercises the PDF page source the CV path opens: PDFs are read through the
// PdfBackendService GRPARSE_PDF_BACKEND names, here an in-process fake that
// reports the page geometry and text cells each case sets up, so the
// contract-to-pixel mapping (CropBox origin, /Rotate, per-document DPI), the
// OCR-skip gate, error mapping and concurrent page access are covered
// without a PDF engine in the test.
#include <atomic>
#include <cstdlib>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "grparse/in_memory_document.h"
#include "grparse/remote_page_source.h"
#include "grparse/reading_order.h"
#include "grparse/text_geometry.h"
#include "support/check.h"
#include "support/fake_pdf_backend.h"

namespace {

using grparse_test::FakePdfPage;
using grparse_test::FakeTextCell;
using grparse_test::require;
using grparse_test::ScopedPdfBackend;
using grparse_test::text_page;

constexpr double kRenderScale = grparse::kDefaultRenderDpi / 72.0;
constexpr double kMediaWidth = 612.0;
constexpr double kMediaHeight = 792.0;

int expected_pixels(double user_space_units) {
  return static_cast<int>(user_space_units * kRenderScale + 0.5);
}

std::shared_ptr<const std::string> bytes_of(const std::string& value) {
  return std::make_shared<const std::string>(value);
}

// Registers `pages` under fresh bytes and opens them through the backend.
std::shared_ptr<grparse::PageSource> open_pages(ScopedPdfBackend& pdf_backend,
                                                std::vector<FakePdfPage> pages,
                                                double dpi = grparse::kDefaultRenderDpi) {
  static int serial = 0;
  const std::string bytes = "%PDF-fixture-" + std::to_string(++serial);
  pdf_backend.backend().add_document(bytes, std::move(pages));
  return grparse::open_in_memory_document(bytes_of(bytes), true, dpi);
}

std::string page_text(const grparse::OcrPage& page) {
  std::string joined;
  for (const auto& line : page.lines) {
    if (!joined.empty()) joined.push_back(' ');
    joined += line.text;
  }
  return joined;
}

void require_boxes_fit(const grparse::OcrPage& page, const std::string& what) {
  for (const auto& line : page.lines) {
    const auto box = grparse::bounding_box(line);
    require(line.origin == grparse::TextOrigin::kDigitalPdf, what + ": digital line origin");
    require(box.left >= 0 && box.top >= 0 && box.right <= page.width && box.bottom <= page.height,
            what + ": provenance box must fit the advertised page size");
    require(box.width() > 0 && box.height() > 0, what + ": provenance box must be non-degenerate");
  }
}

void verify_digital_text_and_geometry() {
  ScopedPdfBackend pdf_backend;
  const auto source = open_pages(pdf_backend, {text_page({"Hello gRParse"})});
  require(source->page_count() == 1, "single page document");

  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "born-digital text must be extracted");
  require(page->source == grparse::OcrPage::Source::kDigitalPdf, "digital page source tag");
  require(page_text(*page) == "Hello gRParse", "extracted digital text");
  require(page->width == expected_pixels(kMediaWidth), "page width in render pixels");
  require(page->height == expected_pixels(kMediaHeight), "page height in render pixels");
  require_boxes_fit(*page, "upright page");
  const auto& line = page->lines.front();
  require(line.font_name == "Helvetica", "the subset prefix is stripped from the font name");
  require(line.font_size_pt == 24.0, "the font size passes through");

  // One short line is not evidence of a usable text layer: the page must still
  // be offered to OCR so the merge path can fill it in.
  require(!page->skip_ocr, "a sparse text layer must not suppress OCR");
}

void verify_dense_text_layer_skips_ocr() {
  ScopedPdfBackend pdf_backend;
  const auto source = open_pages(
      pdf_backend, {text_page({"The quick brown fox jumps over the lazy dog",
                               "Pack my box with five dozen liquor jugs",
                               "How vexingly quick daft zebras jump",
                               "Sphinx of black quartz judge my vow",
                               "Jackdaws love my big sphinx of quartz"})});
  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "dense digital text must be extracted");
  require(page->lines.size() == 5, "dense page line count");
  require(page->skip_ocr, "a real text layer must skip raster OCR");
}

// The contract reports boxes before /Rotate; the rendered page has it
// applied, so every quarter turn must land the box where the raster shows
// the text. One cell at user space (72, 700)-(172, 712) on a Letter page:
// in the upright top-left frame it spans x 72..172, y 80..92.
void verify_rotated_page_geometry() {
  struct Case {
    int rotation;
    double left, top, right, bottom;  // expected, in points
  };
  const std::vector<Case> cases = {
      {0, 72, 80, 172, 92},
      {90, 700, 72, 712, 172},
      {180, 440, 700, 540, 712},
      {270, 80, 440, 92, 540},
  };
  for (const Case& expected : cases) {
    ScopedPdfBackend pdf_backend;
    FakePdfPage rotated;
    rotated.rotation_degrees = expected.rotation;
    rotated.cells.push_back(FakeTextCell{"Turned", 72, 700, 172, 712});
    const auto source = open_pages(pdf_backend, {rotated});
    const std::string what = "/Rotate " + std::to_string(expected.rotation);
    const auto page = source->extract_digital_page(1);
    require(page.has_value(), what + ": rotated digital text must be extracted");
    const bool quarter_turn = expected.rotation % 180 != 0;
    require(page->width == expected_pixels(quarter_turn ? kMediaHeight : kMediaWidth) &&
                page->height == expected_pixels(quarter_turn ? kMediaWidth : kMediaHeight),
            what + ": page size follows /Rotate");
    require_boxes_fit(*page, what);
    const auto box = grparse::bounding_box(page->lines.front());
    require(box.left == expected_pixels(expected.left) && box.top == expected_pixels(expected.top) &&
                box.right == expected_pixels(expected.right) &&
                box.bottom == expected_pixels(expected.bottom),
            what + ": the box lands where the rendered page shows it");

    const cv::Mat raster = source->render_page(1);
    require(raster.cols == page->width && raster.rows == page->height,
            what + ": raster and advertised page size agree");
  }
}

// Boxes are user space and the rendered page is the CropBox, so a CropBox
// that does not start at the origin shifts every box by its corner.
void verify_crop_box_origin() {
  ScopedPdfBackend pdf_backend;
  FakePdfPage cropped;
  cropped.crop_box = std::vector<double>{36, 36, 576, 756};
  cropped.cells.push_back(FakeTextCell{"Cropped", 72, 700, 172, 712});
  const auto source = open_pages(pdf_backend, {cropped});
  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "cropped digital text must be extracted");
  require(page->width == expected_pixels(540) && page->height == expected_pixels(720),
          "the page is the CropBox");
  const auto box = grparse::bounding_box(page->lines.front());
  require(box.left == expected_pixels(36) && box.top == expected_pixels(44),
          "boxes are measured from the CropBox corner");
}

// A CropBox reaching past the MediaBox is legal; renderers draw only the
// overlap, so boxes are measured from the clipped corner and the page is the
// MediaBox, matching the raster.
void verify_crop_box_past_media_box_is_clipped() {
  ScopedPdfBackend pdf_backend;
  FakePdfPage oversized;
  oversized.crop_box = std::vector<double>{-18, -18, 630, 810};
  oversized.cells.push_back(FakeTextCell{"Bleed", 72, 700, 172, 712});
  const auto source = open_pages(pdf_backend, {oversized});
  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "digital text on an oversized CropBox must be extracted");
  require(page->width == expected_pixels(kMediaWidth) && page->height == expected_pixels(kMediaHeight),
          "the page is the CropBox clipped to the MediaBox");
  const auto box = grparse::bounding_box(page->lines.front());
  require(box.left == expected_pixels(72) && box.top == expected_pixels(80),
          "boxes are measured from the clipped corner");
}

void verify_render_matches_page_size() {
  ScopedPdfBackend pdf_backend;
  const auto source = open_pages(pdf_backend, {text_page({"Hello gRParse"})});
  const cv::Mat raster = source->render_page(1);
  require(!raster.empty(), "page must rasterize in memory");
  require(raster.type() == CV_8UC3, "renderer must produce three-channel BGR");
  require(raster.cols == expected_pixels(kMediaWidth) && raster.rows == expected_pixels(kMediaHeight),
          "raster size must match the advertised page size");
}

// A per-document render DPI scales the raster and the digital-line geometry
// together, so provenance boxes fit the advertised page size at any scale.
void verify_per_document_render_dpi() {
  constexpr double kDpi = 100.0;
  ScopedPdfBackend pdf_backend;
  const auto source = open_pages(pdf_backend, {text_page({"Hello gRParse"})}, kDpi);
  const auto scaled = [](double user_space_units) {
    return static_cast<int>(user_space_units * kDpi / 72.0 + 0.5);
  };

  const cv::Mat raster = source->render_page(1);
  require(raster.cols == scaled(kMediaWidth) && raster.rows == scaled(kMediaHeight),
          "raster size must follow the per-document DPI");

  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "digital text must extract at any DPI");
  require(page->width == scaled(kMediaWidth) && page->height == scaled(kMediaHeight),
          "advertised page size must follow the per-document DPI");
  require_boxes_fit(*page, "rescaled page");
}

void verify_invalid_input_is_rejected() {
  ScopedPdfBackend pdf_backend;
  bool threw = false;
  try {
    grparse::open_in_memory_document(bytes_of("not a pdf at all"), true);
  } catch (const grparse::InvalidDocument& error) {
    threw = std::string(error.what()).find("CORRUPT") != std::string::npos;
  }
  require(threw, "bytes the backend cannot load fail as InvalidDocument naming the verdict");

  const auto source = open_pages(pdf_backend, {text_page({"Hello"})});
  for (const int page_number : {0, 2, -1}) {
    threw = false;
    try {
      source->render_page(page_number);
    } catch (const grparse::InvalidDocument&) {
      threw = true;
    }
    require(threw, "out-of-range page number must fail as InvalidDocument");
  }

  threw = false;
  try {
    grparse::open_in_memory_document(bytes_of(""), true);
  } catch (const grparse::InvalidDocument&) {
    threw = true;
  }
  require(threw, "empty document bytes must fail as InvalidDocument");
}

// gRParse links no PDF engine: without a backend a PDF is a deployment gap,
// raised as its own type so the service answers FAILED_PRECONDITION.
void verify_missing_backend_is_a_precondition() {
  unsetenv("GRPARSE_PDF_BACKEND");
  bool threw = false;
  try {
    grparse::open_in_memory_document(bytes_of("%PDF-1.7\n"), true);
  } catch (const grparse::PdfBackendNotConfigured& error) {
    threw = std::string(error.what()).find("GRPARSE_PDF_BACKEND") != std::string::npos;
  }
  require(threw, "a PDF without a backend raises PdfBackendNotConfigured naming the variable");

  setenv("GRPARSE_PDF_BACKEND", "", 1);
  threw = false;
  try {
    grparse::open_in_memory_document(bytes_of("%PDF-1.7\n"), true);
  } catch (const grparse::PdfBackendNotConfigured&) {
    threw = true;
  }
  require(threw, "an empty GRPARSE_PDF_BACKEND configures no backend");
  unsetenv("GRPARSE_PDF_BACKEND");
}

// The in-process engine is gone; naming it must fail at configuration time,
// in a single target or inside a consensus list, not dial a host by that name.
void verify_inprocess_target_is_refused() {
  for (const char* value : {"inprocess", " inprocess ", "pdfium:50069, inprocess"}) {
    setenv("GRPARSE_PDF_BACKEND", value, 1);
    bool threw = false;
    try {
      grparse::remote_pdf_backend_target();
    } catch (const std::invalid_argument& error) {
      threw = std::string(error.what()).find("inprocess") != std::string::npos;
    }
    require(threw, std::string("GRPARSE_PDF_BACKEND=") + value + " is refused by name");
  }
  setenv("GRPARSE_PDF_BACKEND", "pdfium:50069,qparse:50070", 1);
  require(grparse::remote_pdf_backend_target().has_value(), "a real consensus list still parses");
  unsetenv("GRPARSE_PDF_BACKEND");
}

// Render workers share one PageSource and reach the backend concurrently;
// every caller must get the same answer.
void verify_concurrent_page_access() {
  constexpr int kWorkers = 8;
  ScopedPdfBackend pdf_backend;
  const auto source = open_pages(pdf_backend, {text_page({"Hello gRParse"})});
  const auto reference = source->extract_digital_page(1);
  require(reference.has_value(), "reference digital page");
  const std::string reference_text = page_text(*reference);

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < kWorkers; ++index) {
    workers.emplace_back([&, index] {
      try {
        for (int round = 0; round < 4; ++round) {
          if (index % 2 == 0) {
            const auto page = source->extract_digital_page(1);
            if (!page.has_value() || page_text(*page) != reference_text ||
                page->width != reference->width) {
              failures.fetch_add(1);
            }
          } else {
            const cv::Mat raster = source->render_page(1);
            if (raster.empty() || raster.cols != reference->width) failures.fetch_add(1);
          }
        }
      } catch (...) {
        failures.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  require(failures.load() == 0, "concurrent page access must be consistent and exception free");
}

// Rasters never need a backend: they decode in process.
void verify_raster_source() {
  unsetenv("GRPARSE_PDF_BACKEND");
  // A tiny valid PNG (1x1, white) exercised through the non-PDF branch.
  static const unsigned char kPng[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
      0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xFF, 0xFF, 0x3F,
      0x00, 0x05, 0xFE, 0x02, 0xFE, 0xDC, 0xCC, 0x59, 0xE7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
      0x44, 0xAE, 0x42, 0x60, 0x82};
  const std::string png(reinterpret_cast<const char*>(kPng), sizeof(kPng));
  const auto source = grparse::open_in_memory_document(bytes_of(png), false);
  require(source->page_count() == 1, "raster sources are single page");
  require(!source->extract_digital_page(1).has_value(), "raster sources carry no digital text");
  const cv::Mat image = source->render_page(1);
  require(image.rows == 1 && image.cols == 1, "raster decoded from memory");

  bool threw = false;
  try {
    source->render_page(2);
  } catch (const grparse::InvalidDocument&) {
    threw = true;
  }
  require(threw, "raster page number must be validated");
}

// C5 fixture: a two-column digital page must read in column order, not the
// row-interleaved order the backend emits the cells in.
void verify_two_column_digital_pdf_reads_in_column_order() {
  ScopedPdfBackend pdf_backend;
  FakePdfPage columns;
  const std::vector<std::string> left = {"Lone", "Ltwo", "Lthree"};
  const std::vector<std::string> right = {"Rone", "Rtwo", "Rthree"};
  double baseline = kMediaHeight - 92.0;
  for (size_t row = 0; row < left.size(); ++row) {
    columns.cells.push_back(FakeTextCell{left[row], 60, baseline - 4,
                                         60 + 10.8 * static_cast<double>(left[row].size()),
                                         baseline + 14, "Helvetica", 18});
    columns.cells.push_back(FakeTextCell{right[row], 340, baseline - 4,
                                         340 + 10.8 * static_cast<double>(right[row].size()),
                                         baseline + 14, "Helvetica", 18});
    baseline -= 60.0;
  }
  const auto source = open_pages(pdf_backend, {columns});
  const auto page = source->extract_digital_page(1);
  require(page.has_value() && page->lines.size() == 6, "two-column digital extraction");

  std::vector<std::string> texts;
  for (const size_t index : grparse::reading_order(*page)) {
    texts.push_back(page->lines[index].text);
  }
  const std::vector<std::string> expected = {"Lone", "Ltwo", "Lthree", "Rone", "Rtwo", "Rthree"};
  require(texts == expected, "two-column PDF must read column-by-column");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("pdf-source-test", {
      verify_digital_text_and_geometry,
      verify_dense_text_layer_skips_ocr,
      verify_rotated_page_geometry,
      verify_crop_box_origin,
      verify_crop_box_past_media_box_is_clipped,
      verify_render_matches_page_size,
      verify_per_document_render_dpi,
      verify_invalid_input_is_rejected,
      verify_missing_backend_is_a_precondition,
      verify_inprocess_target_is_refused,
      verify_concurrent_page_access,
      verify_raster_source,
      verify_two_column_digital_pdf_reads_in_column_order,
  });
}
