#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "grparse/page_previews.h"
#include "support/check.h"
#include "support/fake_pdf_backend.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;

using grparse_test::require;

// The bytes the test sends; the fake PDF backend reads them as two Letter
// pages.
const std::string kTwoPagePdf = "%PDF-two-page-preview-fixture";

// Every page gets a PNG preview sized within the CV path's bound, keyed by
// page number; a page the document already described keeps what it said.
void verify_previews_attach_to_every_page() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf, {grparse_test::text_page({"Hello"}), grparse_test::text_page({"Hello"})});
  docv1::Document document;
  auto& first = (*document.mutable_pages())[1];
  first.set_page_no(1);
  first.mutable_size()->set_width(612);
  first.mutable_size()->set_height(792);
  first.set_unit("pt");
  grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document);
  require(document.pages_size() == 2, "both pages of the PDF carry a preview entry");
  for (const int page_no : {1, 2}) {
    const auto& page = document.pages().at(page_no);
    require(page.page_no() == page_no, "the entry names its page");
    require(page.has_image() && page.image().mimetype() == "image/png" &&
                page.image().uri().starts_with("data:image/png;base64,"),
            "the preview is an embedded PNG");
    const int longest = std::max(page.image().size().width(), page.image().size().height());
    require(longest > 0 && longest <= grparse::kPagePreviewMaxSide,
            "the preview is bounded like the CV path's: " + std::to_string(longest));
    const double ratio = page.image().size().width() / page.image().size().height();
    require(ratio > 0.75 && ratio < 0.79, "the preview keeps the page's aspect ratio");
  }
  require(document.pages().at(1).unit() == "pt" && document.pages().at(1).size().width() == 612,
          "the collector's own page description survives");
}

// Bytes the backend cannot open leave the document exactly as it was.
void verify_unopenable_bytes_change_nothing() {
  grparse_test::ScopedPdfBackend pdf_backend;
  docv1::Document document;
  (*document.mutable_pages())[1].set_page_no(1);
  const std::string before = document.SerializeAsString();
  grparse::attach_page_previews(std::make_shared<const std::string>("not a pdf"), &document);
  require(document.SerializeAsString() == before, "an unopenable source is not a failure");
}

// Previews are decoration: without a PDF backend configured the document is
// left as it was, never failed.
void verify_missing_backend_changes_nothing() {
  unsetenv("GRPARSE_PDF_BACKEND");
  docv1::Document document;
  (*document.mutable_pages())[1].set_page_no(1);
  const std::string before = document.SerializeAsString();
  grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document);
  require(document.SerializeAsString() == before, "a missing backend is not a failure");
}

// A request trimmed to a page span renders only that span: no entry, and no
// render, for a page the returned document does not hold.
void verify_previews_honor_page_range() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf, {grparse_test::text_page({"Hello"}), grparse_test::text_page({"Hello"})});
  docv1::Document document;
  grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document,
                                std::make_pair(2, 5));
  require(document.pages_size() == 1 && document.pages().contains(2) &&
              document.pages().at(2).has_image(),
          "only the page inside the range carries a preview");
}

// A stopped request (cancelled, past its deadline) renders nothing further;
// the pages already rendered stay.
void verify_previews_stop_when_asked() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf, {grparse_test::text_page({"Hello"}), grparse_test::text_page({"Hello"})});
  docv1::Document document;
  // The watch that cancels a render in flight polls the same predicate from
  // its own thread; only the page loop's polls are counted here.
  const auto caller = std::this_thread::get_id();
  std::atomic<int> polls{0};
  grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document,
                                std::nullopt, [&polls, caller] {
                                  return std::this_thread::get_id() == caller && ++polls > 1;
                                });
  require(polls == 2, "the stop predicate is polled before each page");
  require(document.pages_size() == 1 && document.pages().contains(1),
          "the page rendered before the stop keeps its preview, the rest are skipped");
}

// A render stuck on the backend ends with the request: the source is
// cancelled once `stop` answers true, instead of waiting out the backend.
void verify_stop_cancels_a_render_in_flight() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(kTwoPagePdf, {grparse_test::text_page({"Hello"})});
  pdf_backend.backend().stall_renders(std::chrono::seconds(20));
  docv1::Document document;
  const auto started = std::chrono::steady_clock::now();
  grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt,
      [started] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the stalled render is cancelled, not waited out");
  require(document.pages_size() == 0, "the cancelled page keeps no preview");
}


// The opening Probe is tied to the request too: a backend that hangs while
// loading the document is cancelled once `stop` answers true.
void verify_stop_cancels_a_hung_open() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(kTwoPagePdf, {grparse_test::text_page({"Hello"})});
  pdf_backend.backend().block_probe(kTwoPagePdf);
  docv1::Document document;
  const auto started = std::chrono::steady_clock::now();
  grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt,
      [started] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the hung Probe is cancelled, not waited out");
  require(document.pages_size() == 0, "a document that never opened keeps no previews");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("page-previews-test", {
      verify_previews_attach_to_every_page,
      verify_unopenable_bytes_change_nothing,
      verify_missing_backend_changes_nothing,
      verify_previews_honor_page_range,
      verify_previews_stop_when_asked,
      verify_stop_cancels_a_render_in_flight,
      verify_stop_cancels_a_hung_open,
  });
}
