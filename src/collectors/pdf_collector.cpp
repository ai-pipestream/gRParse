#include "grparse/document_collectors.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "collector_support.h"

namespace pdfv1 = ai::pipestream::pdf::v1;

namespace grparse {

PdfRouteDecision route_pdf_by_classification(const PdfClassification& classification) {
  PdfRouteDecision decision;
  // Document-wide encoding issues make the embedded layer untrustworthy for
  // every class, so the CV run recognizes all pages rather than reading it.
  decision.force_ocr = classification.encoding_issues;
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
      decision.fast_path =
          classification.pages_needing_ocr.empty() && !classification.encoding_issues;
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
                           CollectorDeadline inbound_deadline) {
  PdfParseResult result;
  if (channel == nullptr) {
    result.outcome.error = "pdf collector is not configured (GRPARSE_PDF_TARGET)";
    result.outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    return result;
  }
  auto stub = pdfv1::PdfParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParsePdf(&context);

  pdfv1::ParsePdfRequest request;
  // Mode stays absent, which the wire defines as FULL: the routing decision
  // needs only the info event, but a text-based document's fast path needs
  // the fold, and the fold is built from the page stream.
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](pdfv1::ParsePdfRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  bool trailer_seen = false;
  bool document_seen = false;
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
      // Page 0 is never a page (the wire rejects it in requests); drop it
      // defensively so a buggy server cannot inject it into the scheduler.
      for (const uint32_t page : info.pages_needing_ocr()) {
        if (page >= 1 && page <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
          result.classification.pages_needing_ocr.push_back(static_cast<int>(page));
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
      trailer_seen = true;
    }
    event.Clear();
  }
  result.outcome = finish_outcome("pdf", stream->Finish(), trailer_seen, document_seen,
                                  std::move(result.outcome));
  return result;
}

CollectorOutcome collect_pdf_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline) {
  return collect_pdf(channel, bytes, inbound_deadline).outcome;
}
}  // namespace grparse
