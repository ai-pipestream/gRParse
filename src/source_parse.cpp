#include "source_parse.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "grparse/base64.h"
#include "grparse/chart_derender.h"
#include "grparse/confidence.h"
#include "grparse/content_sniff.h"
#include "grparse/data_totals.h"
#include "grparse/document_assembly.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"
#include "grparse/heading_hierarchy.h"
#include "grparse/input_format.h"
#include "grparse/page_previews.h"
#include "grparse/schema_version.h"
#include "parse_support.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// Stamps the origin's mimetype and what it rests on: the request's own
// content type first, the bytes next, the name last (content_sniff.h).
void stamp_origin_mimetype(const std::string& declared_content_type,
                           const std::string& bytes, const fs::path& filename,
                           pipestream::document::v1::DocumentOrigin* origin) {
  const MimetypeResolution resolved = resolve_mimetype(declared_content_type, bytes, filename);
  origin->set_mimetype(resolved.mimetype);
  origin->set_mimetype_evidence(resolved.evidence);
  if (resolved.evidence == "magic") {
    data_counters().mimetypes_sniffed.fetch_add(1, std::memory_order_relaxed);
    data_log("origin " + filename.string() + " mimetype " + resolved.mimetype +
             " from the bytes");
  }
}

const char* pdf_class_name(PdfClass pdf_class) {
  switch (pdf_class) {
    case PdfClass::kTextBased: return "text-based";
    case PdfClass::kScanned: return "scanned";
    case PdfClass::kImageBased: return "image-based";
    case PdfClass::kMixed: return "mixed";
    default: return "unknown";
  }
}

std::vector<pipestream::parse::v1::Collector> requested_collectors(
    const google::protobuf::RepeatedField<int>& raw) {
  std::vector<pipestream::parse::v1::Collector> collectors;
  collectors.reserve(raw.size());
  for (const int value : raw) {
    collectors.push_back(static_cast<pipestream::parse::v1::Collector>(value));
  }
  return collectors;
}

// True for the output formats ConvertSource renders: every named value of
// the wire's OutputFormat enum. UNSPECIFIED and values outside the enum are
// rejected up front by validate_options, each by name.
bool renderable(pipestream::parse::v1::OutputFormat format) {
  switch (format) {
    case pipestream::parse::v1::OUTPUT_FORMAT_TEXT:
    case pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN:
    case pipestream::parse::v1::OUTPUT_FORMAT_HTML:
    case pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE:
    case pipestream::parse::v1::OUTPUT_FORMAT_JSON:
    case pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS:
    case pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG:
    case pipestream::parse::v1::OUTPUT_FORMAT_VTT:
    case pipestream::parse::v1::OUTPUT_FORMAT_YAML:
    case pipestream::parse::v1::OUTPUT_FORMAT_CANONICAL_JSON:
    case pipestream::parse::v1::OUTPUT_FORMAT_GDOCS_JSON:
    case pipestream::parse::v1::OUTPUT_FORMAT_LATEX:
      return true;
    default:
      return false;
  }
}

// The options every conversion surface implements. Anything else populated
// on the request is rejected by name (below): an option this server would
// silently ignore is worse than one it turns down.
bool implemented_option(std::string_view name) {
  static constexpr std::string_view kImplemented[] = {
      "from_formats",
      "to_formats",
      "collectors",
      "ebcdic_layout_json",
      "lol_html_options_json",
      "do_ocr",
      "force_ocr",
      "render_scale",
      "pipeline",
      "include_page_images",
      "md_page_break_placeholder",
      "md_compact_tables",
      "do_pdf_heading_hierarchy",
      "pdf_heading_hierarchy_options",
      "document_timeout",
      "page_range",
      "include_images",
      "images_scale",
      "image_export_mode",
  };
  return std::ranges::find(kImplemented, name) != std::end(kImplemented);
}

// The pipelines this server runs: STANDARD is the default path, NATIVE the
// model-free extraction the pdf collector's own Document provides. The
// VLM, ASR and LEGACY pipelines name engines this server does not host.
grpc::Status validate_pipeline(const pipestream::parse::v1::ConvertDocumentOptions& options,
                               const std::string& surface) {
  if (!options.has_pipeline()) return grpc::Status::OK;
  switch (options.pipeline()) {
    case pipestream::parse::v1::PROCESSING_PIPELINE_UNSPECIFIED:
    case pipestream::parse::v1::PROCESSING_PIPELINE_STANDARD:
    case pipestream::parse::v1::PROCESSING_PIPELINE_NATIVE:
      return grpc::Status::OK;
    default: {
      std::string name = pipestream::parse::v1::ProcessingPipeline_Name(options.pipeline());
      if (name.empty()) name = std::to_string(static_cast<int>(options.pipeline()));
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement pipeline '" + name + "'");
    }
  }
}

// The heading pass's two switches must agree when both are given, and the
// numeric tunables must be in range; the rest of the message is accepted as
// documented on HeadingHierarchyOptions.
grpc::Status validate_heading_options(
    const pipestream::parse::v1::ConvertDocumentOptions& options, const std::string& surface) {
  if (!options.has_pdf_heading_hierarchy_options()) return grpc::Status::OK;
  const auto& heading = options.pdf_heading_hierarchy_options();
  if (options.has_do_pdf_heading_hierarchy() && heading.has_enabled() &&
      options.do_pdf_heading_hierarchy() != heading.enabled()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": do_pdf_heading_hierarchy and "
                                  "pdf_heading_hierarchy_options.enabled disagree");
  }
  if (heading.has_max_level() && (heading.max_level() < 1 || heading.max_level() > 6)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": pdf_heading_hierarchy_options.max_level must be in [1, 6]");
  }
  if (heading.has_style_size_tolerance() &&
      (heading.style_size_tolerance() < 0.0 || heading.style_size_tolerance() >= 1.0)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": pdf_heading_hierarchy_options.style_size_tolerance must "
                                  "be in [0, 1)");
  }
  return grpc::Status::OK;
}

// `surface` names the RPC in the rejections so a caller learns which of the
// conversion surfaces turned its request down.
grpc::Status validate_options(const pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface) {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (!implemented_option(field->name())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement option '" + std::string(field->name()) + "'");
    }
  }
  const grpc::Status tuning_status =
      validate_ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                          options.has_render_scale(), options.render_scale());
  if (!tuning_status.ok()) return tuning_status;
  const grpc::Status pipeline_status = validate_pipeline(options, surface);
  if (!pipeline_status.ok()) return pipeline_status;
  const grpc::Status heading_status = validate_heading_options(options, surface);
  if (!heading_status.ok()) return heading_status;
  for (const auto raw : options.to_formats()) {
    const auto format = static_cast<pipestream::parse::v1::OutputFormat>(raw);
    if (!renderable(format)) {
      // A value outside the enum has no name; the rejection still identifies
      // it by number.
      std::string name = pipestream::parse::v1::OutputFormat_Name(format);
      if (name.empty()) name = std::to_string(raw);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement output format '" + name + "'");
    }
  }
  for (const auto raw : options.from_formats()) {
    const auto format = static_cast<pipestream::parse::v1::InputFormat>(raw);
    if (format == pipestream::parse::v1::INPUT_FORMAT_UNSPECIFIED ||
        !pipestream::parse::v1::InputFormat_IsValid(raw)) {
      std::string name = pipestream::parse::v1::InputFormat_Name(format);
      if (name.empty()) name = std::to_string(raw);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " from_formats contains invalid value '" + name + "'");
    }
  }
  // Docling page_range is a (start, end) tuple; on this wire that is exactly
  // two 1-indexed ints. Empty means the whole document.
  if (options.page_range_size() != 0 && options.page_range_size() != 2) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " page_range must be empty or [start, end]");
  }
  if (options.page_range_size() == 2) {
    const int start = options.page_range(0);
    const int end = options.page_range(1);
    if (start < 1 || end < start) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " page_range must be a 1-indexed inclusive span");
    }
  }
  if (options.has_images_scale() && !(options.images_scale() > 0.0)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " images_scale must be positive");
  }
  if (options.has_image_export_mode()) {
    switch (options.image_export_mode()) {
      case pipestream::parse::v1::IMAGE_REF_MODE_UNSPECIFIED:
      case pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED:
      case pipestream::parse::v1::IMAGE_REF_MODE_PLACEHOLDER:
      case pipestream::parse::v1::IMAGE_REF_MODE_REFERENCED:
        break;
      default: {
        std::string name =
            pipestream::parse::v1::ImageRefMode_Name(options.image_export_mode());
        if (name.empty()) name = std::to_string(static_cast<int>(options.image_export_mode()));
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + " does not implement image_export_mode '" + name + "'");
      }
    }
  }
  return validate_document_timeout(options.has_document_timeout(), options.document_timeout(),
                                   surface);
}

// The document every collector's output merges into, additively and in plan
// order. It carries identity and nothing else: the schema name and version
// name the wire schema minor this repo currently mirrors, and must match
// what every other producer stamps on its documents.
pipestream::document::v1::Document base_document(const std::string& bytes,
                                                 const fs::path& requested_name) {
  pipestream::document::v1::Document base;
  base.set_schema_name(kWireSchemaName);
  base.set_version(kUpstreamSchemaVersion);
  base.set_name(requested_name.filename().string());
  auto* origin = base.mutable_origin();
  origin->set_filename(requested_name.filename().string());
  // A FileSource declares no content type; the bytes speak before the name.
  // The resolved type is the origin's and, below, the routing's and every
  // dialed collector's: what the bytes are is decided once.
  stamp_origin_mimetype(std::string(), bytes, requested_name, origin);
  origin->set_binary_hash(content_hash(bytes));
  // The stamp is the service's own claim: attributed like any other, and
  // ranked above every collector's, so no collector's idea of the
  // filename or the hash displaces what the request said.
  pipestream::document::v1::CollectorSource stamp;
  stamp.set_collector("grparse");
  claim_fields(origin, stamp);
  auto* stamp_claim = base.add_claims();
  *stamp_claim->mutable_source() = stamp;
  *stamp_claim->mutable_origin() = *origin;
  stamp_claim->mutable_origin()->clear_field_sources();
  base.mutable_body()->set_self_ref("#/body");
  base.mutable_body()->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
  base.mutable_furniture()->set_self_ref("#/furniture");
  base.mutable_furniture()->set_content_layer(
      pipestream::document::v1::CONTENT_LAYER_FURNITURE);
  return base;
}

// The CV path is the only collector that knows where its text lands in the
// document's text stream. Its offset rows are kept here and only published
// when that collector turns out to be the whole document.
using CvOffsets =
    std::shared_ptr<google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>;
// Likewise the only collector that measured its pages: its read quality is
// kept here and published on the response whenever it read any page.
using CvConfidence = std::shared_ptr<std::optional<pipestream::parse::v1::ConfidenceScores>>;

// The in-process CV collector: the page scheduler's layout, OCR, and model
// pipeline over rendered pages, assembled into a document fragment. Never
// throws; failures become the outcome. The tuning rides as a call parameter
// because the pdf routing leg re-enters this path with the inspector's OCR
// page set applied.
class CvCollector {
 public:
  CvCollector(grpc::CallbackServerContext* context, PageScheduler& scheduler,
              std::shared_ptr<const std::string> bytes, bool pdf, CvOffsets offsets,
              CvConfidence confidence, HeadingOptions heading_options)
      : context_(context),
        scheduler_(scheduler),
        bytes_(std::move(bytes)),
        pdf_(pdf),
        offsets_(std::move(offsets)),
        confidence_(std::move(confidence)),
        heading_options_(std::move(heading_options)) {}

  CollectorOutcome operator()(const PageScheduler::OcrTuning& tuning) const {
    try {
      PageSet pages;
      if (std::optional<CollectorOutcome> failure = collect_pages(tuning, &pages)) {
        return std::move(*failure);
      }
      return assemble(pages);
    } catch (...) {
      CollectorOutcome outcome;
      const grpc::Status status = status_from_exception(std::current_exception());
      outcome.error = status.error_message();
      outcome.code = status.error_code();
      return outcome;
    }
  }

 private:
  // Every page one submission produced, in page order.
  struct PageSet {
    std::map<int, std::shared_ptr<const OcrPage>> pages;
    int total_pages = 0;
  };

  // The scheduler's own state while a submission runs: the pages as they
  // land, the failure that ended it, and whether it has ended at all.
  struct Run {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<int, std::shared_ptr<const OcrPage>> pages;
    std::exception_ptr failure;
    int total_pages = 0;
    bool finished = false;
  };

  // Submits the document and waits for the scheduler to finish with it,
  // cancelling the ticket as soon as the call goes away. Returns the outcome
  // that ended the run, or nothing when every page arrived.
  std::optional<CollectorOutcome> collect_pages(const PageScheduler::OcrTuning& tuning,
                                                PageSet* collected) const {
    Run state;
    auto* context = context_;
    const auto ticket = scheduler_.submit(
        bytes_, pdf_, tuning,
        PageScheduler::Callbacks{
            [&state](int total_pages) {
              std::lock_guard<std::mutex> lock(state.mutex);
              state.total_pages = total_pages;
              state.changed.notify_all();
            },
            [&state, context](int page_number, std::shared_ptr<const OcrPage> page) {
              if (context->IsCancelled()) return PageScheduler::DeliveryResult::kCancelled;
              std::lock_guard<std::mutex> lock(state.mutex);
              state.pages.emplace(page_number, std::move(page));
              return PageScheduler::DeliveryResult::kAcceptedAndRelease;
            },
            [&state](std::exception_ptr failure) {
              std::lock_guard<std::mutex> lock(state.mutex);
              state.failure = std::move(failure);
              state.finished = true;
              state.changed.notify_all();
            }});

    std::unique_lock<std::mutex> lock(state.mutex);
    while (!state.finished) {
      state.changed.wait_for(lock, std::chrono::milliseconds(25));
      if (context_->IsCancelled()) ticket.cancel();
    }
    if (context_->IsCancelled()) return cancelled_outcome();
    const grpc::Status scheduler_status = status_from_exception(state.failure);
    if (!scheduler_status.ok()) {
      CollectorOutcome outcome;
      outcome.error = scheduler_status.error_message();
      outcome.code = scheduler_status.error_code();
      return outcome;
    }
    if (state.total_pages <= 0 || state.pages.size() != static_cast<size_t>(state.total_pages)) {
      CollectorOutcome outcome;
      outcome.error = "scheduler completed before every page was available";
      return outcome;
    }
    collected->pages = std::move(state.pages);
    collected->total_pages = state.total_pages;
    return std::nullopt;
  }

  // Folds the collected pages into one document fragment and keeps the
  // offset rows its text stream produced.
  CollectorOutcome assemble(const PageSet& collected) const {
    CollectorOutcome outcome;
    std::string plain_text;
    AssemblyCursor assembly_cursor;
    google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset> offsets;
    std::vector<const OcrPage*> assembled_pages;
    assembled_pages.reserve(collected.pages.size());
    for (const auto& [page_number, page] : collected.pages) {
      append_page_to_document(*page, page_number, &assembly_cursor,
                              &outcome.document, &plain_text, &offsets,
                              &outcome.warnings);
      assembled_pages.push_back(page.get());
    }
    // A PDF read through several backends claims its vote: one aggregate
    // "protomolt" claim for the document, no-op when nothing voted.
    append_consensus_claim(assembled_pages, &outcome.document);
    // Heading depth clusters over the whole document's heights, so it can
    // only run after every page is in.
    assign_section_header_levels(&outcome.document, heading_options_);
    std::vector<PageConfidence> page_scores;
    page_scores.reserve(assembled_pages.size());
    for (const OcrPage* page : assembled_pages) page_scores.push_back(page_confidence(*page));
    *confidence_ = document_confidence(page_scores);
    *offsets_ = std::move(offsets);
    outcome.success = true;
    return outcome;
  }

  grpc::CallbackServerContext* context_;
  PageScheduler& scheduler_;
  std::shared_ptr<const std::string> bytes_;
  bool pdf_;
  CvOffsets offsets_;
  CvConfidence confidence_;
  HeadingOptions heading_options_;
};

// Everything one parse's collector legs read: the request's bytes and
// identity, the endpoints they dial, and the ceilings they run under. Copied
// into each leg, which is why every member is a value or a shared handle.
struct ParseInputs {
  grpc::CallbackServerContext* context = nullptr;
  std::shared_ptr<CollectorEndpoints> endpoints;
  std::shared_ptr<const std::string> bytes;
  std::shared_ptr<const std::string> ebcdic_layout_json;
  std::shared_ptr<const std::string> lol_html_options_json;
  fs::path filename;
  std::string content_type;
  PageScheduler::OcrTuning tuning;
  CollectorDeadline inbound_deadline = kNoCollectorDeadline;
  bool previews = false;
  // PROCESSING_PIPELINE_NATIVE: the pdf collector's own model-free Document
  // is the answer whatever its classification said.
  bool native_pipeline = false;
  HeadingOptions heading;
};

// The heading pass's tuning as the request states it; every unset field
// keeps the pass's own default (HeadingOptions).
HeadingOptions heading_options_from(const pipestream::parse::v1::ConvertDocumentOptions& options) {
  HeadingOptions heading;
  if (options.has_do_pdf_heading_hierarchy()) heading.enabled = options.do_pdf_heading_hierarchy();
  if (!options.has_pdf_heading_hierarchy_options()) return heading;
  const auto& requested = options.pdf_heading_hierarchy_options();
  if (requested.has_enabled()) heading.enabled = requested.enabled();
  if (requested.has_use_numbering()) heading.use_numbering = requested.use_numbering();
  if (requested.has_use_style()) heading.use_style = requested.use_style();
  if (requested.has_max_level()) heading.max_level = requested.max_level();
  if (requested.has_style_size_tolerance()) {
    heading.style_size_tolerance = requested.style_size_tolerance();
  }
  return heading;
}

// One parse's inputs, read off the request once. The mimetype is the
// origin's own resolved type, so the routing and every dialed collector see
// what the bytes were decided to be.
ParseInputs parse_inputs(grpc::CallbackServerContext* context,
                         const pipestream::parse::v1::ConvertDocumentRequest& request,
                         const PageScheduler& scheduler,
                         const std::shared_ptr<CollectorEndpoints>& collectors,
                         std::shared_ptr<const std::string> bytes,
                         const fs::path& requested_name, std::string content_type) {
  const auto& options = request.options();
  ParseInputs inputs;
  inputs.context = context;
  inputs.endpoints = collectors;
  inputs.bytes = std::move(bytes);
  inputs.ebcdic_layout_json = std::make_shared<const std::string>(options.ebcdic_layout_json());
  inputs.lol_html_options_json =
      std::make_shared<const std::string>(options.lol_html_options_json());
  inputs.filename = requested_name;
  inputs.content_type = std::move(content_type);
  inputs.tuning = ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                             options.has_render_scale(), options.render_scale());
  if (options.page_range_size() == 2) {
    inputs.tuning.page_range = std::make_pair(options.page_range(0), options.page_range(1));
  }
  if (options.has_include_images()) {
    inputs.tuning.capture_picture_images = options.include_images();
  }
  if (options.has_images_scale()) {
    inputs.tuning.images_scale = options.images_scale();
  }
  // Every dialed leg inherits this call's own ceiling, so no collector is
  // waited on past the patience of the client that asked for the parse. A
  // call with no deadline yields time_point::max(), which leaves each leg on
  // its own static cap exactly as before. document_timeout (seconds) further
  // caps that ceiling when set — parity with Docling Convert options.
  inputs.inbound_deadline = deadline_with_document_timeout(
      context->deadline(), options.has_document_timeout(), options.document_timeout());
  // A collector-folded PDF never rasterized; when previews are on, it gets
  // them rendered so the shell has a page to paint the boxes on. The request
  // decides when it says; the server setting otherwise.
  if (options.has_include_page_images()) {
    inputs.tuning.capture_page_images = options.include_page_images();
    inputs.previews = options.include_page_images();
  } else {
    inputs.previews = scheduler.captures_page_images();
  }
  inputs.native_pipeline =
      options.has_pipeline() &&
      options.pipeline() == pipestream::parse::v1::PROCESSING_PIPELINE_NATIVE;
  inputs.heading = heading_options_from(options);
  return inputs;
}

// The pdf routing leg: the inspector's classification decides between the
// collector's own fast-path Document and a CV run restricted to the pages it
// named as needing OCR. A failed classification degrades to the unrouted CV
// path, never to a failed parse.
CollectorOutcome route_pdf_leg(const ParseInputs& inputs, const CvCollector& run_cv) {
  // Same pre-dial cancellation check as the plain collector legs: a call
  // that died after the dequeue check must not dial the inspector either.
  if (inputs.context->IsCancelled()) return cancelled_outcome();
  const PdfParseResult parsed =
      collect_pdf(inputs.endpoints->channel(pipestream::parse::v1::COLLECTOR_PDF),
                  *inputs.bytes, inputs.inbound_deadline, inputs.tuning.page_range);
  const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
  if (parsed.outcome.success && (route.fast_path || inputs.native_pipeline)) {
    PdfParseResult fast = parsed;
    if (inputs.previews) attach_page_previews(inputs.bytes, &fast.outcome.document);
    if (!route.fast_path) {
      // NATIVE asked for the text layer as it is; say what the models would
      // have been run for, so a caller can tell a thin result from a thin
      // document.
      fast.outcome.warnings.push_back(
          "pipeline NATIVE took the pdf collector's extraction although the inspector "
          "classified the document as " +
          std::string(pdf_class_name(parsed.classification.pdf_class)) +
          (parsed.classification.encoding_issues ? " with encoding issues in the text layer"
                                                 : "") +
          "; no layout, OCR, or table-structure model ran");
    }
    return fast.outcome;
  }
  if (!parsed.outcome.success && inputs.native_pipeline) {
    // Degrading to the CV path would run the models NATIVE excludes.
    CollectorOutcome outcome;
    outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    outcome.error = "pipeline NATIVE needs the pdf collector's extraction and it failed: " +
                    parsed.outcome.error;
    return outcome;
  }
  if (!parsed.outcome.success) {
    // Degrade, don't sink: an unreachable inspector leaves the parse on
    // exactly the path it would have taken without the collector.
    CollectorOutcome outcome = run_cv(inputs.tuning);
    outcome.warnings.push_back("pdf collector failed (" + parsed.outcome.error +
                               "); fell back to the in-process CV path");
    return outcome;
  }
  PageScheduler::OcrTuning routed_tuning = inputs.tuning;
  routed_tuning.ocr_pages.insert(route.ocr_pages.begin(), route.ocr_pages.end());
  const bool forced =
      route.force_ocr && routed_tuning.mode == PageScheduler::OcrTuning::Mode::kSelective;
  if (forced) routed_tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
  CollectorOutcome outcome = run_cv(routed_tuning);
  outcome.warnings.push_back(
      "pdf inspector classified the document as " +
      std::string(pdf_class_name(parsed.classification.pdf_class)) +
      (parsed.classification.encoding_issues
           ? " with encoding issues in the text layer, so its extraction was not taken"
           : "") +
      (forced
           ? "; recognition was forced on every page in place of the "
             "untrustworthy embedded layer"
       : route.ocr_pages.empty()
           ? "; the CV path's own per-page heuristic decided recognition"
           : "; recognition restricted to the " +
                 std::to_string(route.ocr_pages.size()) + " page(s) needing OCR"));
  return outcome;
}

// The collectors one request runs, and whether the pdf collector owns the
// plan whole, which is what turns classification routing on.
struct RoutedPlan {
  std::vector<pipestream::parse::v1::Collector> ids;
  bool pdf_routing = false;
  // True when the routed (not explicitly selected) plan fanned out to the
  // secondary office collectors; the legs append_office_fanout adds are
  // always poi or calamine, never the routed primary itself.
  bool office_fanout = false;
};

// The default PDF route becomes the pdf inspector when one is configured:
// its classification decides between the collector's own fast-path Document
// and a CV run restricted to the pages needing OCR. Unconfigured, PDF stays
// on the CV path exactly as before.
RoutedPlan route_plan(const google::protobuf::RepeatedField<int>& requested, bool pdf,
                      const ParseInputs& inputs) {
  const auto selected = requested_collectors(requested);
  auto routed = route_document(inputs.filename.string(), std::string(), *inputs.bytes);
  const bool inspector =
      inputs.endpoints != nullptr && inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_PDF);
  if (selected.empty() && pdf && routed == pipestream::parse::v1::COLLECTOR_GRPARSE_CV &&
      inspector) {
    routed = pipestream::parse::v1::COLLECTOR_PDF;
  }
  RoutedPlan plan;
  plan.ids = resolve_collectors(selected, routed);
  // A routed office default fans out to the secondary office collectors when
  // their endpoints are configured: a poi leg for the OOXML/OLE2 formats, a
  // calamine leg for workbooks. An explicit selection stays verbatim.
  if (selected.empty() && inputs.endpoints != nullptr) {
    plan.office_fanout = true;
    append_office_fanout(&plan.ids, inputs.filename.string(), inputs.content_type,
                         inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_POI),
                         inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_CALAMINE));
  }
  // Classification routing applies when the pdf collector is the whole plan:
  // by the swap above or by explicit sole selection. Shared with other
  // collectors it is a plain Document-emitting leg.
  plan.pdf_routing = pdf && plan.ids.size() == 1 &&
                     plan.ids[0] == pipestream::parse::v1::COLLECTOR_PDF && inspector;
  return plan;
}

// One planned leg per collector, in plan order: the CV path, an in-process
// fold, the classification-routed pdf leg, or a dialed collector.
std::vector<PlannedCollector> build_plan(
    const std::vector<pipestream::parse::v1::Collector>& plan_ids, bool pdf_routing,
    bool office_fanout, const ParseInputs& inputs, const CvCollector& run_cv) {
  std::vector<PlannedCollector> plan;
  for (const auto id : plan_ids) {
    PlannedCollector collector;
    collector.id = id;
    // The fan-out legs read the same bytes as the routed libreoffice
    // default: beside a live primary their body readings drop and only
    // their claims merge. An explicit selection stays verbatim, readings
    // and all.
    collector.office_fanout =
        office_fanout && (id == pipestream::parse::v1::COLLECTOR_POI ||
                          id == pipestream::parse::v1::COLLECTOR_CALAMINE);
    if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
      collector.run = [run_cv, tuning = inputs.tuning] { return run_cv(tuning); };
    } else if (local_collector(id)) {
      collector.run = [id, bytes = inputs.bytes] { return run_local_collector(id, *bytes); };
    } else if (pdf_routing) {
      collector.run = [inputs, run_cv] { return route_pdf_leg(inputs, run_cv); };
    } else {
      collector.run = [id, inputs] {
        // The dequeue check answered "still listening" before this parse
        // started; a cancel can land any time after. Ask again before
        // dialing so a dead call costs no collector leg.
        if (inputs.context->IsCancelled()) return cancelled_outcome();
        return run_remote_collector(id, inputs.endpoints, inputs.filename.string(),
                                    inputs.filename.string(), inputs.content_type,
                                    *inputs.bytes, *inputs.ebcdic_layout_json,
                                    *inputs.lol_html_options_json, inputs.inbound_deadline);
      };
    }
    plan.push_back(std::move(collector));
  }
  return plan;
}

// The status a parse whose every collector failed reports: the first
// failure's code, and every failure's message keyed by its collector.
grpc::Status all_failed_status(const CoordinatorResult& result) {
  const auto& first = result.failures.front();
  std::string message;
  for (const auto& failure : result.failures) {
    if (!message.empty()) message += "; ";
    message += std::string(collector_name(failure.id)) + ": " + failure.error;
  }
  return grpc::Status(first.code, message);
}

// The chart derender leg runs on the finished document, after the merge and
// the repair pass, so it sees every raster chart the CV path classified and
// none of the office charts (those carry their typed table already).
// Advisory and bounded: it edits picture annotations only, never the text or
// the arenas, and its failures are warnings.
void derender_charts_if_configured(const std::shared_ptr<CollectorEndpoints>& collectors,
                                   grpc::CallbackServerContext* context,
                                   CollectorDeadline inbound_deadline,
                                   CoordinatorResult* result) {
  if (collectors == nullptr || !collectors->has_derender() || context->IsCancelled()) return;
  const ChartDerenderReport derendered =
      derender_charts(collectors->enrich_channel(), collectors->derender(), &result->document,
                      inbound_deadline);
  for (const std::string& warning : derendered.warnings) {
    result->warnings.emplace_back(pipestream::parse::v1::COLLECTOR_GRPARSE_CV, warning);
  }
}

// Collector warnings are not failures; they stay on the document, keyed by
// collector, so nothing the collectors reported is dropped.
void stamp_collector_warnings(CoordinatorResult* result) {
  for (const auto& [collector, text] : result->warnings) {
    auto& fields =
        *result->document.mutable_body()->mutable_meta()->mutable_custom_fields();
    *fields[std::string("collector_warnings:") + collector_name(collector)]
         .mutable_list_value()
         ->add_values()
         ->mutable_string_value() = text;
  }
}

}  // namespace

grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& repair,
                          const std::string& surface, SourceParse* parsed) {
  const auto& sources = request.sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one FileSource containing base64_string");
  }
  const grpc::Status option_status = validate_options(request.options(), surface);
  if (!option_status.ok()) return option_status;
  try {
    const auto& source = sources.Get(0).file();
    auto bytes = std::make_shared<const std::string>(decode_base64(source.base64_string()));
    const fs::path requested_name = source.filename().empty() ? "document.pdf" : fs::path(source.filename()).filename();
    pipestream::document::v1::Document base = base_document(*bytes, requested_name);

    if (!request.options().from_formats().empty()) {
      const auto detected =
          input_format_for(base.origin().mimetype(), requested_name);
      if (!detected.has_value()) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            surface + ": from_formats is set but the input type '" +
                base.origin().mimetype() + "' does not map to a known InputFormat");
      }
      if (!from_formats_allows(request.options().from_formats(), *detected)) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            surface + ": from_formats does not allow " +
                pipestream::parse::v1::InputFormat_Name(*detected));
      }
    }

    const ParseInputs inputs =
        parse_inputs(context, request, scheduler, collectors, bytes, requested_name,
                     base.origin().mimetype());

    const auto cv_offsets = std::make_shared<
        google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>();
    const auto cv_confidence =
        std::make_shared<std::optional<pipestream::parse::v1::ConfidenceScores>>();
    const bool pdf = is_pdf(*bytes, requested_name);
    const CvCollector run_cv(context, scheduler, bytes, pdf, cv_offsets, cv_confidence,
                             inputs.heading);

    const RoutedPlan routed = route_plan(request.options().collectors(), pdf, inputs);
    // NATIVE is model-free by definition. A plan that would put the bytes
    // through the CV models (a PDF with no inspector to read its text layer,
    // a raster image) cannot honour it, and the caller must know rather
    // than get a modelled document under a model-free label.
    if (inputs.native_pipeline &&
        std::ranges::find(routed.ids, pipestream::parse::v1::COLLECTOR_GRPARSE_CV) !=
            routed.ids.end()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          surface + ": pipeline NATIVE needs the pdf collector "
                                    "(GRPARSE_COLLECTOR_PDF) for PDF input and does not "
                                    "apply to raster input");
    }
    CoordinatorResult result = run_collectors(
        build_plan(routed.ids, routed.pdf_routing, routed.office_fanout, inputs, run_cv),
        std::move(base));
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
    }
    if (result.succeeded == 0) return all_failed_status(result);

    // The merged Document is complete here, and this is the one place every
    // unary surface renders from, so the post-merge repair pass runs here:
    // running headers and footers demoted to furniture, line-break
    // hyphenation rejoined, paragraphs a page break split merged.
    const bool repaired_text =
        repair.has_value() &&
        run_repair_pass(&result.document, *repair).changed_text_or_arenas();
    derender_charts_if_configured(collectors, context, inputs.inbound_deadline, &result);
    // The offset table describes the CV collector's own text stream. It is
    // published only when that collector is the entire document and the
    // repair pass left its text and arena alone: a merge renumbers arena
    // references, a repair that retires items or rewrites text moves them,
    // and a table that no longer names the items it describes is worse than
    // no table at all.
    if (result.succeeded == 1 && !cv_offsets->empty() && !repaired_text) {
      chunking::add_offsets(*cv_offsets, &parsed->offsets);
    }
    stamp_collector_warnings(&result);
    parsed->filename = requested_name;
    parsed->result = std::move(result);
    parsed->confidence = std::move(*cv_confidence);
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

}  // namespace grparse
