#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/document_geometry.h"

namespace docv1 = ai::pipestream::document::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;

namespace grparse {

namespace {

void add_ocr_page(uint32_t page, std::set<int>* pages) {
  // Page 0 is never a page (the wire rejects it in requests, and it names
  // the whole-document password fallback on a page event); drop it so a
  // buggy server cannot inject it into the scheduler.
  if (page >= 1 && page <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    pages->insert(static_cast<int>(page));
  }
}

bool blank(const std::string& text) {
  return std::ranges::all_of(text, [](unsigned char c) { return std::isspace(c) != 0; });
}

bool in_body(docv1::ContentLayer layer) {
  return layer == docv1::CONTENT_LAYER_BODY || layer == docv1::CONTENT_LAYER_UNSPECIFIED;
}

// Whether the fold carries anything a reader would call content: a body
// text that is not blank, or a body table. Pictures do not count, because a
// page image with nothing read off it is exactly the scan this guards.
bool has_body_content(const docv1::Document& document) {
  for (const auto& item : document.texts()) {
    const docv1::TextItemBase* base = text_base_of(item);
    if (base != nullptr && in_body(base->content_layer()) && !blank(base->text())) return true;
  }
  return std::ranges::any_of(document.tables(), [](const docv1::TableItem& table) {
    return in_body(table.content_layer());
  });
}

}  // namespace

bool pdf_page_is_clean(bool needs_ocr, bool encoding_issues, const std::string& markdown) {
  return !needs_ocr && !encoding_issues && !blank(markdown);
}

PdfRouteDecision route_pdf_by_classification(const PdfClassification& classification) {
  PdfRouteDecision decision;
  // Document-wide encoding issues make the embedded layer untrustworthy for
  // every class, and a detection that recommends OCR judged recognition the
  // better reading of the whole document; either way the CV run recognizes
  // all pages rather than reading the layer.
  decision.force_ocr = classification.encoding_issues || classification.ocr_recommended;
  switch (classification.pdf_class) {
    case PdfClass::kTextBased:
      // The whole text layer is usable: the collector's own Document is the
      // parse result and the CV pipeline never runs for this document. A
      // text-based document that still names OCR pages is not the fast
      // path; its named pages route to recognition like any other
      // classification's. Neither is one whose trailer flagged encoding
      // issues: the wire contract says that text layer is untrustworthy
      // however confident the classification, and when no pages are named
      // the CV path's own heuristic decides recognition (custom-encoded
      // vector fonts routinely classify TEXT_BASED at full confidence while
      // extracting mojibake or nothing).
      // Nor is one whose fold carried no body at all: a searchable scan
      // (a page image behind an invisible OCR layer) classifies TEXT_BASED
      // and extracts nothing, and an empty Document is not a parse.
      // Nor is one the detection recommended OCR for: the newspaper case
      // names no page, because every page has a usable layer and reading
      // it in order is the hard part.
      decision.fast_path = classification.pages_needing_ocr.empty() &&
                           !classification.encoding_issues && !classification.empty_body &&
                           !classification.ocr_recommended;
      decision.ocr_pages = classification.pages_needing_ocr;
      break;
    case PdfClass::kScanned:
    case PdfClass::kImageBased:
    case PdfClass::kMixed:
      decision.ocr_pages = classification.pages_needing_ocr;
      break;
    case PdfClass::kUnknown:
      // No routing answer (a failed or pre-info stream): leave the CV
      // path's own heuristic in charge.
      break;
  }
  return decision;
}

PdfParseResult collect_pdf(const std::shared_ptr<grpc::Channel>& channel,
                           const std::string& bytes,
                           CollectorDeadline inbound_deadline,
                           std::optional<std::pair<int, int>> page_range,
                           CollectorCancelled cancelled, PdfPageSink page_sink) {
  PdfParseResult result;
  if (channel == nullptr) {
    result.outcome.error = "pdf collector is not configured (GRPARSE_PDF_TARGET)";
    result.outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    return result;
  }
  auto stub = pdfv1::PdfParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->ParsePdf(&context);

  pdfv1::ParsePdfRequest request;
  // Mode stays absent, which the wire defines as FULL: the routing decision
  // needs only the info event, but a text-based document's fast path needs
  // the fold, and the fold is built from the page stream.
  request.mutable_options()->set_emit_document(true);
  // With a sink, the fold also comes page by page, so the fast path can
  // stream instead of waiting for the whole Document.
  const bool streaming = static_cast<bool>(page_sink);
  if (streaming) request.mutable_options()->set_emit_page_documents(true);
  // Docling page_range → collector options first_page/last_page, a
  // 1-indexed inclusive span that costs two numbers however long it is.
  // Docling spells "to the end" as (start, sys.maxsize), which reaches this
  // wire as INT32_MAX: an absent last_page already means the last page, and
  // so does any value past it, so the sentinel is left out. A span from
  // page 1 to the end is the whole document, which the wire spells as no
  // selection at all.
  if (page_range.has_value()) {
    const int first = std::max(page_range->first, 1);
    if (first > 1) request.mutable_options()->set_first_page(static_cast<uint32_t>(first));
    if (page_range->second < std::numeric_limits<int>::max()) {
      request.mutable_options()->set_last_page(
          static_cast<uint32_t>(std::max(page_range->second, first)));
    }
  }
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](pdfv1::ParsePdfRequest& frame, size_t offset, size_t length, bool /*last*/) {
        frame.set_chunk(bytes.data() + offset, length);
      });

  bool trailer_seen = false;
  bool document_seen = false;
  uint32_t page_count = 0;
  std::set<int> ocr_pages;
  // Pages whose markdown came back empty; whether that means "scanned"
  // needs the fold's pictures and the trailer's invisible-text flag, which
  // arrive after the pages.
  std::vector<uint32_t> empty_pages;
  // The streaming state: whether the info event left the fast path open,
  // whether every page so far went to the sink, and the verdict on the page
  // whose slice comes next (its page event precedes it).
  bool fast_path_open = false;
  bool run_open = false;
  std::optional<std::pair<uint32_t, bool>> pending_page;
  pdfv1::ParsePdfResponse event;
  while (stream->Read(&event)) {
    if (event.has_info()) {
      const pdfv1::PdfInfo& info = event.info();
      switch (info.pdf_type()) {
        case pdfv1::PDF_TYPE_TEXT_BASED:
          result.classification.pdf_class = PdfClass::kTextBased;
          break;
        case pdfv1::PDF_TYPE_SCANNED:
          result.classification.pdf_class = PdfClass::kScanned;
          break;
        case pdfv1::PDF_TYPE_IMAGE_BASED:
          result.classification.pdf_class = PdfClass::kImageBased;
          break;
        case pdfv1::PDF_TYPE_MIXED:
          result.classification.pdf_class = PdfClass::kMixed;
          break;
        default:
          break;
      }
      page_count = info.page_count();
      result.classification.ocr_recommended = info.ocr_recommended();
      for (const uint32_t page : info.pages_needing_ocr()) add_ocr_page(page, &ocr_pages);
      // The same document-wide conditions route_pdf_by_classification
      // reads off the info event; anything the pages or the trailer add is
      // judged page by page below.
      fast_path_open = streaming && result.classification.pdf_class == PdfClass::kTextBased &&
                       !result.classification.ocr_recommended && ocr_pages.empty();
      run_open = fast_path_open;
    } else if (event.has_page()) {
      // The pass that decoded the page can convict it where the sampling
      // detection on info did not look.
      if (event.page().needs_ocr()) add_ocr_page(event.page().page_no(), &ocr_pages);
      if (blank(event.page().markdown())) empty_pages.push_back(event.page().page_no());
      if (streaming) {
        pending_page.emplace(event.page().page_no(),
                             pdf_page_is_clean(event.page().needs_ocr(),
                                               event.page().encoding_issues(),
                                               event.page().markdown()));
      }
    } else if (event.has_page_document()) {
      // A page's slice is final the moment it arrives; whether it may go out
      // now is the page's own verdict and the run before it. A blank page
      // ends the run because the trailer can still send it to recognition
      // (a picture on it, or invisible text anywhere), and an unclean one
      // ends it because recognition is where it is going.
      const uint32_t page_no = event.page_document().page_no();
      const bool clean = pending_page.has_value() && pending_page->first == page_no &&
                         pending_page->second;
      pending_page.reset();
      if (fast_path_open && page_no >= 1 &&
          page_no <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        PdfPageSlice slice{static_cast<int>(page_no),
                           static_cast<int>(std::min<uint32_t>(
                               page_count, static_cast<uint32_t>(std::numeric_limits<int>::max()))),
                           std::move(*event.mutable_page_document()->mutable_document())};
        if (run_open && clean) {
          if (page_sink(std::move(slice))) {
            ++result.streamed_pages;
          } else {
            run_open = false;
          }
        } else {
          run_open = false;
          result.held_pages.push_back(std::move(slice));
        }
      }
    } else if (event.has_document()) {
      result.outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      for (const auto& warning : event.status().warnings()) {
        result.outcome.warnings.push_back(
            pdfv1::ParseWarningCode_Name(warning.code()) + ": " + warning.message());
      }
      if (event.status().has_encoding_issues()) {
        // The text layer decoded to mojibake somewhere; whatever the
        // classification said, the folded text is not fully trustworthy.
        // The flag rides the classification too so the routing can refuse
        // the fast path for it.
        result.classification.encoding_issues = true;
        result.outcome.warnings.push_back(
            "encoding issues detected in the text layer; extracted text may be untrustworthy");
      }
      for (const auto& reasons : event.status().extraction_ocr_reasons()) {
        add_ocr_page(reasons.page(), &ocr_pages);
      }
      result.classification.invisible_text = event.status().has_invisible_text();
      trailer_seen = true;
    }
    event.Clear();
  }
  upload.join();
  if (!empty_pages.empty()) {
    // An empty page is a scan the detection missed when it drew a picture,
    // or when the document drew invisible text: the page image with its
    // OCR layer held out of the markdown. A blank page that is neither
    // stays a blank page.
    std::set<int> pictured;
    for (const auto& picture : result.outcome.document.pictures()) {
      for (const auto& prov : picture.prov()) pictured.insert(prov.page_no());
    }
    for (const uint32_t page : empty_pages) {
      if (result.classification.invisible_text ||
          (page <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
           pictured.contains(static_cast<int>(page)))) {
        add_ocr_page(page, &ocr_pages);
      }
    }
  }
  result.classification.pages_needing_ocr.assign(ocr_pages.begin(), ocr_pages.end());
  result.classification.empty_body =
      document_seen && page_count > 0 && !has_body_content(result.outcome.document);
  if (result.classification.invisible_text) {
    result.outcome.warnings.push_back(
        "the text layer drew invisible text (an OCR layer behind a scan, or hidden text), "
        "which the extraction leaves out");
  }
  result.outcome = finish_outcome("pdf", stream->Finish(), trailer_seen, document_seen,
                                  std::move(result.outcome));
  return result;
}

CollectorOutcome collect_pdf_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline,
                                      CollectorCancelled cancelled) {
  return collect_pdf(channel, bytes, inbound_deadline, std::nullopt, std::move(cancelled))
      .outcome;
}
}  // namespace grparse
