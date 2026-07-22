// Exercises the real Poppler-backed page source with PDFs built in memory, so
// the digital-text path, provenance geometry, and parser fan-out are covered
// without checking binary fixtures into the repository.
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "grparse/in_memory_document.h"
#include "grparse/text_geometry.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr double kRenderScale = 200.0 / 72.0;  // must track kRenderDpi in the source
constexpr int kMediaWidth = 612;
constexpr int kMediaHeight = 792;

int expected_pixels(double user_space_units) {
  return static_cast<int>(user_space_units * kRenderScale + 0.5);
}

// Assembles a single-page PDF with a base-14 font and explicit widths, so text
// extraction never depends on system fonts being installed.
std::string build_pdf(const std::vector<std::string>& lines, int rotate) {
  std::string content;
  int baseline = kMediaHeight - 72;
  for (const auto& line : lines) {
    content += "BT /F1 24 Tf 72 " + std::to_string(baseline) + " Td (" + line + ") Tj ET\n";
    baseline -= 60;
  }

  std::string widths = "[";
  for (int code = 32; code < 127; ++code) widths += "600 ";
  widths += "]";

  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(kMediaWidth) + " " +
          std::to_string(kMediaHeight) + "] /Rotate " + std::to_string(rotate) +
          " /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 32 /LastChar 126 /Widths " +
          widths + " >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
  };

  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  offsets.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
  }

  const size_t xref_offset = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
  pdf += "0000000000 65535 f \n";
  for (const size_t offset : offsets) {
    std::string entry = std::to_string(offset);
    entry.insert(entry.begin(), 10 - entry.size(), '0');
    pdf += entry + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref_offset) + "\n%%EOF\n";
  return pdf;
}

std::shared_ptr<const std::string> bytes_of(const std::string& value) {
  return std::make_shared<const std::string>(value);
}

std::string page_text(const grparse::OcrPage& page) {
  std::string joined;
  for (const auto& line : page.lines) {
    if (!joined.empty()) joined.push_back(' ');
    joined += line.text;
  }
  return joined;
}

void verify_digital_text_and_geometry() {
  const auto source = grparse::open_in_memory_document(bytes_of(build_pdf({"Hello gRParse"}, 0)), true);
  require(source->page_count() == 1, "single page document");

  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "born-digital text must be extracted");
  require(page->source == grparse::OcrPage::Source::kDigitalPdf, "digital page source tag");
  require(page_text(*page).find("Hello") != std::string::npos, "extracted digital text");
  require(page->width == expected_pixels(kMediaWidth), "page width in render pixels");
  require(page->height == expected_pixels(kMediaHeight), "page height in render pixels");

  for (const auto& line : page->lines) {
    const auto box = grparse::bounding_box(line);
    require(line.origin == grparse::TextOrigin::kDigitalPdf, "digital line origin");
    require(box.left >= 0 && box.top >= 0, "provenance box inside the page");
    require(box.right <= page->width && box.bottom <= page->height,
            "provenance box must fit the advertised page size");
    require(box.width() > 0 && box.height() > 0, "provenance box must be non-degenerate");
  }

  // One short line is not evidence of a usable text layer: the page must still
  // be offered to OCR so the merge path can fill it in.
  require(!page->skip_ocr, "a sparse text layer must not suppress OCR");
}

void verify_dense_text_layer_skips_ocr() {
  const auto source = grparse::open_in_memory_document(
      bytes_of(build_pdf({"The quick brown fox jumps over the lazy dog",
                          "Pack my box with five dozen liquor jugs",
                          "How vexingly quick daft zebras jump",
                          "Sphinx of black quartz judge my vow",
                          "Jackdaws love my big sphinx of quartz"},
                         0)),
      true);
  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "dense digital text must be extracted");
  require(page->lines.size() >= 4, "dense page line count");
  require(page->skip_ocr, "a real text layer must skip raster OCR");
}

// Poppler applies /Rotate to both the raster and the text list; page_rect()
// does not.  Without the swap, boxes on a rotated page fall outside the
// advertised page size.
void verify_rotated_page_geometry() {
  const auto source =
      grparse::open_in_memory_document(bytes_of(build_pdf({"Hello gRParse"}, 90)), true);
  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "rotated digital text must be extracted");
  require(page->width == expected_pixels(kMediaHeight), "rotated page width follows /Rotate");
  require(page->height == expected_pixels(kMediaWidth), "rotated page height follows /Rotate");

  for (const auto& line : page->lines) {
    const auto box = grparse::bounding_box(line);
    require(box.right <= page->width && box.bottom <= page->height,
            "rotated provenance box must fit the advertised page size");
  }

  const cv::Mat raster = source->render_page(1);
  require(!raster.empty(), "rotated page must rasterize");
  require(raster.cols > raster.rows, "rotated raster must be landscape");
}

void verify_render_matches_page_size() {
  const auto source = grparse::open_in_memory_document(bytes_of(build_pdf({"Hello gRParse"}, 0)), true);
  const cv::Mat raster = source->render_page(1);
  require(!raster.empty(), "page must rasterize in memory");
  require(raster.type() == CV_8UC3, "renderer must produce three-channel BGR");
  require(std::abs(raster.cols - expected_pixels(kMediaWidth)) <= 1 &&
              std::abs(raster.rows - expected_pixels(kMediaHeight)) <= 1,
          "raster size must match the advertised page size");
}

void verify_invalid_input_is_rejected() {
  bool threw = false;
  try {
    grparse::open_in_memory_document(bytes_of("not a pdf at all"), true);
  } catch (const grparse::InvalidDocument&) {
    threw = true;
  }
  require(threw, "unreadable PDF bytes must fail as InvalidDocument");

  const auto source = grparse::open_in_memory_document(bytes_of(build_pdf({"Hello"}, 0)), true);
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

// Render workers share one PageSource.  With a pool of parsers they must be
// able to run concurrently and still return identical results.
void verify_concurrent_page_access() {
  constexpr size_t kParsers = 4;
  const auto source =
      grparse::open_in_memory_document(bytes_of(build_pdf({"Hello gRParse"}, 0)), true, kParsers);
  const auto reference = source->extract_digital_page(1);
  require(reference.has_value(), "reference digital page");
  const std::string reference_text = page_text(*reference);

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (size_t index = 0; index < kParsers * 2; ++index) {
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
            if (raster.empty() || raster.cols != source->render_page(1).cols) {
              failures.fetch_add(1);
            }
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

void verify_raster_source() {
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

}  // namespace

int main() {
  try {
    verify_digital_text_and_geometry();
    verify_dense_text_layer_skips_ocr();
    verify_rotated_page_geometry();
    verify_render_matches_page_size();
    verify_invalid_input_is_rejected();
    verify_concurrent_page_access();
    verify_raster_source();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "pdf-source-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
