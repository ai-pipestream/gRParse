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
using grparse_test::require_equal;

// The bytes the test sends; the fake PDF backend reads them as two Letter
// pages.
const std::string kTwoPagePdf = "%PDF-two-page-preview-fixture";

// The one warning that says the document could not be opened for previews:
// typed, and about no page, since no page of it was reached.
void require_document_level_failure(const std::vector<grparse::Warning>& warnings,
                                    const std::string& why) {
  require_equal(warnings.size(), size_t{1}, "one warning about the document: " + why);
  require(warnings.front().code == docv1::WARNING_CODE_PAGE_PREVIEW_FAILED,
          "the warning is typed as a preview failure: " + why);
  require(!warnings.front().page_no.has_value(),
          "a document that never opened names no page: " + why);
  require(!warnings.front().message.empty(), "the warning keeps words for a person: " + why);
}

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
  const std::vector<grparse::Warning> warnings =
      grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document);
  require(warnings.empty(), "every preview attached, so nothing is reported");
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

// Bytes the backend cannot open leave the document exactly as it was, and
// the caller is told once that it has no previews: about the document, not
// once for every page it holds.
void verify_unopenable_bytes_change_nothing_and_are_reported_once() {
  grparse_test::ScopedPdfBackend pdf_backend;
  docv1::Document document;
  (*document.mutable_pages())[1].set_page_no(1);
  (*document.mutable_pages())[2].set_page_no(2);
  const std::string before = document.SerializeAsString();
  const std::vector<grparse::Warning> warnings =
      grparse::attach_page_previews(std::make_shared<const std::string>("not a pdf"), &document);
  require(document.SerializeAsString() == before, "an unopenable source is not a failure");
  require_document_level_failure(warnings, "bytes the backend cannot open");
}

// Previews are decoration: without a PDF backend configured the document is
// left as it was, never failed. The caller still asked for images and got
// none, so it is told.
void verify_missing_backend_changes_nothing_and_is_reported() {
  unsetenv("GRPARSE_PDF_BACKEND");
  docv1::Document document;
  (*document.mutable_pages())[1].set_page_no(1);
  const std::string before = document.SerializeAsString();
  const std::vector<grparse::Warning> warnings =
      grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document);
  require(document.SerializeAsString() == before, "a missing backend is not a failure");
  require_document_level_failure(warnings, "no PDF backend configured");
}

// A page the backend cannot draw is named: the other pages keep their
// previews, and the caller learns which page has none and why.
void verify_a_page_that_does_not_render_is_named() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf,
      {grparse_test::text_page({"One"}), grparse_test::text_page({"Two"}),
       grparse_test::text_page({"Three"})});
  pdf_backend.backend().fail_renders_of_page(1);
  docv1::Document document;
  const std::vector<grparse::Warning> warnings =
      grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &document);
  require(document.pages().contains(1) && document.pages().at(1).has_image() &&
              document.pages().contains(3) && document.pages().at(3).has_image(),
          "the pages that rendered keep their previews");
  require(!document.pages().contains(2), "the page that failed gains no entry");
  require_equal(warnings.size(), size_t{1}, "one warning, for the one page without a preview");
  require(warnings.front().code == docv1::WARNING_CODE_PAGE_PREVIEW_FAILED,
          "the warning is typed as a preview failure");
  require(warnings.front().page_no == 2, "the warning names the page");
  require(warnings.front().message.contains("cannot draw page 2"),
          "the backend's reason rides the message: " + warnings.front().message);
}

// An encrypted PDF renders with the request's password: the collector that
// folded the document opened it with one, and the previews need the same.
void verify_previews_open_with_the_request_password() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(kTwoPagePdf, {grparse_test::text_page({"Hello"})});
  pdf_backend.backend().require_password(kTwoPagePdf, "open sesame");
  docv1::Document locked;
  require_document_level_failure(
      grparse::attach_page_previews(std::make_shared<const std::string>(kTwoPagePdf), &locked),
      "an encrypted document and no password");
  grparse::DocumentPasswords passwords;
  passwords.candidates = {"open sesame"};
  docv1::Document document;
  const std::vector<grparse::Warning> warnings = grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt, {},
      std::chrono::system_clock::time_point::max(), passwords);
  require(warnings.empty(), "the password opened the document, so nothing is reported");
  require(document.pages().contains(1) && document.pages().at(1).has_image(),
          "the encrypted document's page carries its preview");
}

// A request trimmed to a page span renders only that span: no entry, and no
// render, for a page the returned document does not hold.
void verify_previews_honor_page_range() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf, {grparse_test::text_page({"Hello"}), grparse_test::text_page({"Hello"})});
  docv1::Document document;
  const std::vector<grparse::Warning> warnings = grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::make_pair(2, 5));
  require(warnings.empty(), "a page past the end of the PDF is not a failed preview");
  require(document.pages_size() == 1 && document.pages().contains(2) &&
              document.pages().at(2).has_image(),
          "only the page inside the range carries a preview");
}

// A stopped request (cancelled, past its deadline) renders nothing further;
// the pages already rendered stay. The pages it left are not failed
// previews: the request's own status says what happened to it.
void verify_previews_stop_when_asked() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf, {grparse_test::text_page({"Hello"}), grparse_test::text_page({"Hello"})});
  docv1::Document document;
  // The watch that cancels a render in flight polls the same predicate from
  // its own thread; only the page loop's polls are counted here.
  const auto caller = std::this_thread::get_id();
  std::atomic<int> polls{0};
  const std::vector<grparse::Warning> warnings = grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt,
      [&polls, caller] { return std::this_thread::get_id() == caller && ++polls > 1; });
  require(polls == 2, "the stop predicate is polled before each page");
  require(warnings.empty(), "a stopped request reports no failed previews");
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
  const std::vector<grparse::Warning> warnings = grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt,
      [started] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the stalled render is cancelled, not waited out");
  require(document.pages_size() == 0, "the cancelled page keeps no preview");
  require(warnings.empty(), "a render the request's end cut short is not a failed preview");
}


// The opening Probe is tied to the request too: a backend that hangs while
// loading the document is cancelled once `stop` answers true.
void verify_stop_cancels_a_hung_open() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(kTwoPagePdf, {grparse_test::text_page({"Hello"})});
  pdf_backend.backend().block_probe(kTwoPagePdf);
  docv1::Document document;
  const auto started = std::chrono::steady_clock::now();
  const std::vector<grparse::Warning> warnings = grparse::attach_page_previews(
      std::make_shared<const std::string>(kTwoPagePdf), &document, std::nullopt,
      [started] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the hung Probe is cancelled, not waited out");
  require(document.pages_size() == 0, "a document that never opened keeps no previews");
  require(warnings.empty(), "an open the request's end cut short is not a failed preview");
}

// A PagePreviewSession reused across individual page slices opens and probes
// the backend once for the whole stream instead of once per page.
void verify_session_probes_once_across_streamed_pages() {
  grparse_test::ScopedPdfBackend pdf_backend;
  pdf_backend.backend().add_document(
      kTwoPagePdf,
      {grparse_test::text_page({"One"}), grparse_test::text_page({"Two"}),
       grparse_test::text_page({"Three"})});
  grparse::PagePreviewSession session(std::make_shared<const std::string>(kTwoPagePdf));
  require(pdf_backend.backend().probe_calls() == 0,
          "the session is lazy until the first page slice arrives");
  for (int page_no = 1; page_no <= 3; ++page_no) {
    docv1::Document slice;
    const std::vector<grparse::Warning> warnings =
        session.attach(&slice, std::make_pair(page_no, page_no));
    require(warnings.empty() && slice.pages_size() == 1 && slice.pages().at(page_no).has_image(),
            "each streamed page slice receives its preview");
  }
  require(pdf_backend.backend().probe_calls() == 2,
          "one content-addressed open handshake (miss + upload) serves all three pages");
  require(pdf_backend.backend().render_calls() == 3,
          "each page renders once");
}

// A session that could not open its document says so on the first slice and
// not again: the stream carries one warning about the document, not one for
// every page that follows.
void verify_session_reports_a_failed_open_once_across_streamed_pages() {
  grparse_test::ScopedPdfBackend pdf_backend;
  grparse::PagePreviewSession session(std::make_shared<const std::string>(kTwoPagePdf));
  std::vector<grparse::Warning> warnings;
  for (int page_no = 1; page_no <= 3; ++page_no) {
    docv1::Document slice;
    for (grparse::Warning& warning : session.attach(&slice, std::make_pair(page_no, page_no))) {
      warnings.push_back(std::move(warning));
    }
    require(slice.pages_size() == 0, "a slice of a document that never opened gains nothing");
  }
  require_document_level_failure(warnings, "three slices of a document that never opened");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("page-previews-test", {
      verify_previews_attach_to_every_page,
      verify_unopenable_bytes_change_nothing_and_are_reported_once,
      verify_missing_backend_changes_nothing_and_is_reported,
      verify_a_page_that_does_not_render_is_named,
      verify_previews_open_with_the_request_password,
      verify_previews_honor_page_range,
      verify_previews_stop_when_asked,
      verify_stop_cancels_a_render_in_flight,
      verify_stop_cancels_a_hung_open,
      verify_session_probes_once_across_streamed_pages,
      verify_session_reports_a_failed_open_once_across_streamed_pages,
  });
}
