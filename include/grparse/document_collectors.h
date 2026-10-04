#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/ebcdic/v1/ebcdic.pb.h"
#include "grparse/collector_coordinator.h"
#include "lolhtml/v1/lolhtml_service.pb.h"

namespace grparse {

// Clients for the collectors that project their own typed stream into a
// Document server-side (TranscribeOptions.emit_document and friends). Each
// streams the input bytes up, asks for the Document event, and returns it
// as the outcome; the collector's typed events are drained and dropped
// because the fold already happened where the events were made. None of
// these throw: transport and collector failures land in the outcome so the
// coordinator can degrade instead of failing the parse.
//
// Every client takes `inbound_deadline`, the absolute ceiling of the call
// that asked for the parse. Each leg runs until the sooner of that and its
// own static cap, so a client that gave up or ran out of time is never
// waited on past its own patience; kNoCollectorDeadline (the default) means
// the call carried none and the leg keeps its cap alone. Every client also
// takes `cancelled` (CollectorCancelled), which cancels the leg's own call
// once the inbound call is gone; empty (the default) watches nothing.

// The text a failed collector leg reports: the collector's name, then the
// status message, or the status code's name when the message is empty, so
// a leg that failed with a bare code never reads as "markup collector: ".
std::string collector_status_text(const char* name, const grpc::Status& status);

// grpc-asr. `model` is the whisper model name the collector must have
// loaded; the caller resolves it from configuration, never guesses.
CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& filename,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline =
                                          kNoCollectorDeadline,
                                      CollectorCancelled cancelled = {});

// grpc-email (.eml / .msg bytes). The email fold maps text/plain bodies
// only and leaves HTML to the HTML collector, so a message with no plain
// body has its HTML body parts dialed through `markup` (the HTML hint, one
// shared ceiling) and their items folded into the message body ahead of the
// attachments list, the way an epub chapter folds into its book. A part the
// markup collector cannot parse is reported and skipped. With no markup
// channel (`GRPARSE_MARKUP_TARGET` unset) the fold is the outcome, with a
// warning naming the variable. Attachments stay listed by name: their bytes
// are not requested and nothing recurses into them.
CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::shared_ptr<grpc::Channel>& markup,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes,
                                        CollectorDeadline inbound_deadline =
                                            kNoCollectorDeadline,
                                        CollectorCancelled cancelled = {});

// Gives a document whose collector recorded the source's own title only as
// metadata (an HTML <title>) a TITLE item at the head of the body, so the
// heading tree starts where the source says it does. A body that already
// has a title item, or metadata with no title, is left alone. The item is
// attributed to this service with the model "source-meta-title": it is
// derived from the collector's claim, not claimed by the collector. Returns
// true when an item was added.
bool promote_source_title(ai::pipestream::document::v1::Document* document);

// grpc-xml. The dialect is left unspecified so the collector sniffs it; a
// sniff that fails is that collector's failure, not the parse's.
CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline =
                                          kNoCollectorDeadline,
                                      CollectorCancelled cancelled = {});

// grpc-ebcdic. `options` carries the request's layout in the collector's
// own typed form (or the deprecated JSON form, forwarded verbatim); this
// client asks for the Document event on top. Options with no layout are a
// caller error surfaced as an INVALID_ARGUMENT outcome before anything is
// dialed.
CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const ai::pipestream::ebcdic::v1::ParseOptions& options,
                                         const std::string& bytes,
                                         CollectorDeadline inbound_deadline =
                                             kNoCollectorDeadline,
                                         CollectorCancelled cancelled = {});

// grpc-epub (.epub archive bytes): the collector's own Document, which is
// a skeleton by contract (empty chapter groups, pictures by reference).
CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes,
                                       CollectorDeadline inbound_deadline =
                                           kNoCollectorDeadline,
                                       CollectorCancelled cancelled = {});

// grpc-epub, then grpc-markup once per chapter: the whole book. The epub
// stream's chapter XHTML and image bytes are kept instead of dropped, each
// chapter is dialed through `markup` with the HTML hint, and the chapters'
// Documents and the images plug into the skeleton (see epub_book.h). A
// chapter the markup collector cannot parse leaves its group empty and
// says so in a warning; the book never fails for one chapter. The chapter
// dials share one ceiling (the leg's cap from when they start), not a cap
// each; chapters past it stay empty, with a warning. With no
// markup channel (`GRPARSE_MARKUP_TARGET` unset) the skeleton is the
// outcome, with a warning naming the variable, so the degradation is
// visible rather than silent. An image spine item (a cover or plate in the
// spine) needs no markup dial: its chapter group gets one picture of the
// bytes its chapter event carried.
//
// The chapters and images the stream carries are decompressed archive
// entries, so a small book can inflate far past its upload: once they
// pass `stream_byte_cap` together the call is cancelled and the leg fails
// with RESOURCE_EXHAUSTED.
inline constexpr size_t kEpubStreamByteCap = 512U * 1024U * 1024U;
CollectorOutcome collect_epub_book(const std::shared_ptr<grpc::Channel>& epub,
                                   const std::shared_ptr<grpc::Channel>& markup,
                                   const std::string& bytes,
                                   CollectorDeadline inbound_deadline =
                                       kNoCollectorDeadline,
                                   CollectorCancelled cancelled = {},
                                   size_t stream_byte_cap = kEpubStreamByteCap);

// grpc-markup (Markdown, HTML, AsciiDoc, LaTeX, WebVTT, BoxNote, Docling
// JSON). The format is hinted from the filename and content type via
// markup_format_for; an unresolved hint asks the collector to sniff.
CollectorOutcome collect_markup_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& filename,
                                         const std::string& content_type,
                                         const std::string& bytes,
                                         CollectorDeadline inbound_deadline =
                                             kNoCollectorDeadline,
                                         CollectorCancelled cancelled = {});

// grpc-lol-html, the one collector whose stream carries no document event:
// it reports CSS selector matches as they happen, so this client folds the
// match events into a Document here — a group per rule, its matches and
// text as source-tagged text items in arrival order. `options` is the
// request's typed lolhtml.v1.ExtractOptions; absent is a caller error
// surfaced as an INVALID_ARGUMENT outcome before anything is dialed,
// because this client never invents selector rules.
CollectorOutcome collect_lol_html_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::optional<lolhtml::v1::ExtractOptions>& options,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline =
                                               kNoCollectorDeadline,
                                           CollectorCancelled cancelled = {});

// fastwarc-grpc, the second collector whose stream carries no document
// event: it reports WARC records as they parse, so this client folds the
// record stream into a Document here — one group per record in stream
// order, the record's metadata and (when it reads as text) its payload as
// source-tagged text items. A non-recoverable framing error ends the fold
// but keeps the records already collected: a clipped archive is a partial
// success, not a failure. A leg cut off by its deadline or cancelled is a
// failure: nothing says where its archive really ended.
CollectorOutcome collect_fastwarc_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline =
                                               kNoCollectorDeadline,
                                           CollectorCancelled cancelled = {});

// grpc-calamine (workbooks: xls/xlsx/xlsm/xlsb/ods). The wire is
// handle-based and carries no document event: OpenWorkbook uploads the
// bytes, one StreamWorksheetRange per sheet streams the cells, and the fold
// happens here — one sheet group and one TableItem per sheet. CloseWorkbook
// runs on every path, success or failure.
CollectorOutcome collect_calamine_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline =
                                               kNoCollectorDeadline,
                                           CollectorCancelled cancelled = {});

// grpc-pdf-inspector's classification of one document, mapped off the wire
// enum so the routing decision stays proto-free and unit-testable.
// kUnknown also covers "the stream carried no info event at all".
enum class PdfClass { kUnknown, kTextBased, kScanned, kImageBased, kMixed };

struct PdfClassification {
  PdfClass pdf_class = PdfClass::kUnknown;
  // 1-indexed pages the inspector reported as needing OCR — the same
  // indexing the wire and the page scheduler both use, so the numbers pass
  // straight through. Sorted and unique: the sampling detection's set on
  // info, widened by what the extraction pass found (the trailer's
  // extraction_ocr_reasons, each page event's needs_ocr) and by every page
  // that extracted no markdown although it drew a picture or the trailer
  // reported invisible text, which is how a scanned page reads when the
  // detection's sample missed it or an OCR layer sits behind it.
  std::vector<int> pages_needing_ocr;
  // The status trailer's has_encoding_issues flag: broken font encodings in
  // the text layer, whose extraction the wire contract says not to trust.
  // A classification can carry this with an empty OCR page set (the
  // inspector knows the layer is garbled without knowing which pages), so
  // the routing consults it independently.
  bool encoding_issues = false;
  // The info event's ocr_recommended flag: detection judged that OCR reads
  // this document better than its text layer does (images carry essential
  // context, or a dense newspaper layout whose reading order the layer
  // cannot be trusted to keep). It is a document-wide answer and can be set
  // on a TEXT_BASED document that names no OCR page, so the routing
  // consults it independently of pages_needing_ocr.
  bool ocr_recommended = false;
  // The trailer's has_invisible_text flag: some page drew text in rendering
  // mode 3 (typically an OCR layer behind a scan), which never reaches the
  // markdown or the folded body.
  bool invisible_text = false;
  // The folded Document of a document with pages carries no body text or
  // table at all: whatever the classification, the extraction produced
  // nothing, so it is not a parse result to take as is.
  bool empty_body = false;
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
  // True when the classification carried encoding issues or recommended
  // OCR: the embedded text layer is not to be read document-wide, so the CV
  // path should recognize every page and let the recognized text replace
  // that layer (kForce) instead of reading it. Reading a layer the contract
  // says to distrust at best folds mojibake into the result, and reading one
  // the detection judged OCR to beat keeps the reading it judged worse. An
  // explicit kOff request still outranks this, as it outranks every
  // classification hint.
  bool force_ocr = false;
};

PdfRouteDecision route_pdf_by_classification(const PdfClassification& classification);

// One page's share of the inspector's fold (its page_document event): the
// items folding that page made, under the refs they carry in the whole
// Document, plus the page count the info event reported.
struct PdfPageSlice {
  int page_no = 0;
  int page_count = 0;
  ai::pipestream::document::v1::Document document;
};

// Receives a page slice the moment it is known to be final: the document is
// still on course for the fast path and the page itself is clean. Answers
// false once nobody is listening (the call is gone), which stops the
// streaming for the rest of the parse.
using PdfPageSink = std::function<bool(PdfPageSlice slice)>;

// The pdf client's full return: the collector outcome (its Document, which
// the server folds whenever emit_document is set) plus the classification
// the stream opened with.
struct PdfParseResult {
  CollectorOutcome outcome;
  PdfClassification classification;
  // With a page sink: the pages handed to it, a run from the first page
  // the collector extracted, in order.
  int streamed_pages = 0;
  // With a page sink: the slices of the pages after that run, held while
  // the trailer could still route them to recognition. Kept only for a
  // document the info event left on course for the fast path, since no
  // other route reads them.
  std::vector<PdfPageSlice> held_pages;
};

// Whether an extracted page's own text layer is final as it arrives: it
// carries text, its reading pass did not ask for recognition, and the
// encoding backstop did not convict its rendering. A page that fails any
// of these may still be routed to recognition by the trailer.
bool pdf_page_is_clean(bool needs_ocr, bool encoding_issues, const std::string& markdown);

// grpc-pdf-inspector, dialed for routing: FULL mode with emit_document, so
// a text-based document's own Document is the fast-path result while every
// classification reports its OCR page set in the info event. page_range
// (inclusive 1-indexed) selects which pages the collector extracts when set.
//
// With a page sink, the call also asks for page documents and streams the
// fast path page by page: while the info event's classification allows the
// fast path and every page so far was clean, each page's slice goes to the
// sink as it arrives. The first page that is not clean ends the run; the
// slices after it are held for the caller to route once the trailer is in.
// An inspector that predates page documents sends none, and the parse is
// exactly what it was without a sink.
PdfParseResult collect_pdf(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& bytes,
    CollectorDeadline inbound_deadline = kNoCollectorDeadline,
    std::optional<std::pair<int, int>> page_range = std::nullopt,
    CollectorCancelled cancelled = {}, PdfPageSink page_sink = {});

// The plain collector leg for a selection the pdf collector shares with
// other collectors: the collector's Document is the contribution, whatever
// the classification was.
CollectorOutcome collect_pdf_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline =
                                          kNoCollectorDeadline,
                                      CollectorCancelled cancelled = {});

}  // namespace grparse
