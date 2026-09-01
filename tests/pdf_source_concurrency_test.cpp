// Regression test for the poppler thread-safety crash: concurrent
// substitute-font lookups (the fallback path a broken embedded font takes on
// every text op) corrupt poppler's shared state even across fully
// independent documents, landing as std::system_error or SIGSEGV out of
// XRef::fetch. PdfPageSource serialises every poppler entry point on one
// process-wide mutex; this test hammers extraction and rendering from two
// threads over a PDF built to force the fallback constantly - an AcroForm
// sheet whose /DA fonts are missing from /DR and whose content names
// undefined font tags - both against one shared source (the page-worker
// shape) and against independent sources (the probe that reproduced the
// upstream crash). The PDF is assembled in memory; no binary fixture.
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/in_memory_document.h"
#include "support/check.h"

namespace {

using grparse_test::require;

std::string pdf_object(int number, const std::string& body) {
  return std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
}

// A one-page PDF with the substitute-font pathology: NeedAppearances form
// fields whose /DA names fonts absent from /DR (poppler regenerates their
// appearance streams through the fallback), plus page content that shows
// text with font tags its resources never define ("Unknown font tag" /
// "No font in show" on stderr is this document working as intended).
std::shared_ptr<const std::string> bad_font_form_pdf() {
  const std::string content =
      "BT /F1 12 Tf 72 700 Td (Strength) Tj ET\n"
      "BT /Helvetica 10 Tf 72 650 Td (Dexterity) Tj ET\n";
  const std::string appearance = "/Tx BMC BT /Helv 12 Tf 2 2 Td (Bard) Tj ET EMC";
  std::map<int, std::string> objects;
  objects[1] =
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R 7 0 R] "
      "/NeedAppearances true /DA (/Helv 0 Tf 0 g) /DR << >> >> >>";
  objects[2] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
  objects[3] =
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
      "/Annots [5 0 R 7 0 R] /Resources << >> >>";
  objects[4] = "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
               content + "\nendstream";
  objects[5] =
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [72 600 300 620] "
      "/DA (/Helv 12 Tf 0 g) /V (Justin's Character) /F 4 /P 3 0 R >>";
  objects[6] = "<< /Length " + std::to_string(appearance.size()) +
               " /Type /XObject /Subtype /Form /BBox [0 0 228 20] /Resources << >> "
               ">>\nstream\n" +
               appearance + "\nendstream";
  objects[7] =
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (class) /Rect [72 560 300 580] "
      "/DA (/NoSuchFont 12 Tf 0 g) /V (Bard) /F 4 /P 3 0 R /AP << /N 6 0 R >> >>";

  std::string out = "%PDF-1.7\n";
  std::map<int, size_t> offsets;
  for (const auto& [number, body] : objects) {
    offsets[number] = out.size();
    out += pdf_object(number, body);
  }
  const size_t xref_at = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
  out += "0000000000 65535 f \n";
  char line[32];
  for (const auto& [number, offset] : offsets) {
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_at) + "\n%%EOF\n";
  return std::make_shared<const std::string>(std::move(out));
}

constexpr int kIterations = 12;
constexpr double kTestDpi = 36.0;  // tiny rasters; the race is in font lookup

void hammer(const std::shared_ptr<grparse::PageSource>& source,
            std::exception_ptr& failure) {
  try {
    for (int i = 0; i < kIterations; ++i) {
      const auto digital = source->extract_digital_page(1);
      require(digital.has_value() && !digital->lines.empty(),
              "the generated appearances carry extractable text");
      const cv::Mat raster = source->render_page(1);
      require(raster.cols > 0 && raster.rows > 0, "the page renders");
    }
  } catch (...) {
    failure = std::current_exception();
  }
}

void rethrow(const std::exception_ptr& first, const std::exception_ptr& second) {
  if (first) std::rethrow_exception(first);
  if (second) std::rethrow_exception(second);
}

// The page-worker shape: one document, two threads extracting and rendering.
void verify_shared_source_survives_concurrency() {
  const auto source =
      grparse::open_in_memory_document(bad_font_form_pdf(), /*pdf=*/true,
                                       /*pdf_parser_slots=*/2, kTestDpi);
  std::exception_ptr first, second;
  std::thread a([&] { hammer(source, first); });
  std::thread b([&] { hammer(source, second); });
  a.join();
  b.join();
  rethrow(first, second);
}

// The upstream repro shape: fully independent documents over the same bytes,
// which still crashed pre-serialisation because the corrupted state is
// poppler's, not the document's.
void verify_independent_sources_survive_concurrency() {
  const auto bytes = bad_font_form_pdf();
  std::exception_ptr first, second;
  std::thread a([&] {
    const auto source =
        grparse::open_in_memory_document(bytes, /*pdf=*/true, /*pdf_parser_slots=*/1, kTestDpi);
    hammer(source, first);
  });
  std::thread b([&] {
    const auto source =
        grparse::open_in_memory_document(bytes, /*pdf=*/true, /*pdf_parser_slots=*/1, kTestDpi);
    hammer(source, second);
  });
  a.join();
  b.join();
  rethrow(first, second);
}

}  // namespace

int main() {
  return grparse_test::run_test_main({.on_failure = "FAILED", .on_success = "pdf source concurrency test passed"}, {
      verify_shared_source_survives_concurrency,
      verify_independent_sources_survive_concurrency,
  });
}
