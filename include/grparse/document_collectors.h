#pragma once

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace grparse {

// Clients for the collectors that project their own typed stream into a
// Document server-side (TranscribeOptions.emit_document and friends). Each
// streams the input bytes up, asks for the Document event, and returns it
// as the outcome; the collector's typed events are drained and dropped
// because the fold already happened where the events were made. None of
// these throw: transport and collector failures land in the outcome so the
// coordinator can degrade instead of failing the parse.

// grpc-asr. `model` is the whisper model name the collector must have
// loaded; the caller resolves it from configuration, never guesses.
CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& bytes);

// grpc-email (.eml / .msg bytes).
CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes);

// grpc-xml. The dialect is left unspecified so the collector sniffs it; a
// sniff that fails is that collector's failure, not the parse's.
CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes);

// grpc-ebcdic. `layout_json` is the JSON serialization of Docling's
// EbcdicLayout, forwarded verbatim; empty is a caller error surfaced as an
// INVALID_ARGUMENT outcome before anything is dialed.
CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& layout_json,
                                         const std::string& bytes);

// grpc-epub (.epub archive bytes).
CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes);

// grpc-markup (Markdown, HTML, AsciiDoc, LaTeX, WebVTT, BoxNote, Docling
// JSON). The format is hinted from the filename and content type via
// markup_format_for; an unresolved hint asks the collector to sniff.
CollectorOutcome collect_markup_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& filename,
                                         const std::string& content_type,
                                         const std::string& bytes);

// grpc-lol-html, the one collector whose stream carries no document event:
// it reports CSS selector matches as they happen, so this client folds the
// match events into a Document here — a group per rule, its matches and
// text as source-tagged text items in arrival order. `options_json` is the
// protobuf JSON serialization of lolhtml.v1.ExtractOptions; empty is a
// caller error surfaced as an INVALID_ARGUMENT outcome before anything is
// dialed, because this client never invents selector rules.
CollectorOutcome collect_lol_html_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& options_json,
                                           const std::string& bytes);

// fastwarc-grpc, the second collector whose stream carries no document
// event: it reports WARC records as they parse, so this client folds the
// record stream into a Document here — one group per record in stream
// order, the record's metadata and (when it reads as text) its payload as
// source-tagged text items. A non-recoverable framing error ends the fold
// but keeps the records already collected: a clipped archive is a partial
// success, not a failure.
CollectorOutcome collect_fastwarc_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes);

// grpc-pdf-inspector's classification of one document, mapped off the wire
// enum so the routing decision stays proto-free and unit-testable.
// kUnknown also covers "the stream carried no info event at all".
enum class PdfClass { kUnknown, kTextBased, kScanned, kImageBased, kMixed };

struct PdfClassification {
  PdfClass pdf_class = PdfClass::kUnknown;
  // 1-indexed pages the inspector reported as needing OCR — the same
  // indexing the wire and the page scheduler both use, so the numbers pass
  // straight through.
  std::vector<int> pages_needing_ocr;
};

// The routing answer for one classified PDF. The fast path takes the
// collector's own Document as the parse result and skips the in-process CV
// pipeline entirely; otherwise the CV path runs with recognition restricted
// to ocr_pages. An empty ocr_pages means the classification carried no
// usable hint (or none was seen), and the CV path's own embedded-layer
// heuristic decides, exactly as it does without the collector.
struct PdfRouteDecision {
  bool fast_path = false;
  std::vector<int> ocr_pages;
};

PdfRouteDecision route_pdf_by_classification(const PdfClassification& classification);

// The pdf client's full return: the collector outcome (its Document, which
// the server folds whenever emit_document is set) plus the classification
// the stream opened with.
struct PdfParseResult {
  CollectorOutcome outcome;
  PdfClassification classification;
};

// grpc-pdf-inspector, dialed for routing: FULL mode with emit_document, so
// a text-based document's own Document is the fast-path result while every
// classification reports its OCR page set in the info event.
PdfParseResult collect_pdf(const std::shared_ptr<grpc::Channel>& channel,
                           const std::string& bytes);

// The plain collector leg for a selection the pdf collector shares with
// other collectors: the collector's Document is the contribution, whatever
// the classification was.
CollectorOutcome collect_pdf_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes);

}  // namespace grparse
