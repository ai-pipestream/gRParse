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
#include "grparse/content_sniff.h"
#include "grparse/data_totals.h"
#include "grparse/document_assembly.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"
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

// `surface` names the RPC in the rejections so a caller learns which of the
// conversion surfaces turned its request down.
grpc::Status validate_options(const pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface) {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (field->name() != "to_formats" && field->name() != "collectors" &&
        field->name() != "ebcdic_layout_json" &&
        field->name() != "lol_html_options_json" && field->name() != "do_ocr" &&
        field->name() != "force_ocr" && field->name() != "render_scale") {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement option '" + std::string(field->name()) + "'");
    }
  }
  const grpc::Status tuning_status =
      validate_ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                          options.has_render_scale(), options.render_scale());
  if (!tuning_status.ok()) return tuning_status;
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
  return grpc::Status::OK;
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

// The in-process CV collector: the page scheduler's layout, OCR, and model
// pipeline over rendered pages, assembled into a document fragment. Never
// throws; failures become the outcome. The tuning rides as a call parameter
// because the pdf routing leg re-enters this path with the inspector's OCR
// page set applied.
class CvCollector {
 public:
  CvCollector(grpc::CallbackServerContext* context, PageScheduler& scheduler,
              std::shared_ptr<const std::string> bytes, bool pdf, CvOffsets offsets)
      : context_(context),
        scheduler_(scheduler),
        bytes_(std::move(bytes)),
        pdf_(pdf),
        offsets_(std::move(offsets)) {}

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
    for (int page_number = 1; page_number <= collected.total_pages; ++page_number) {
      const auto page = collected.pages.find(page_number);
      if (page == collected.pages.end()) {
        outcome.error = "scheduler omitted a document page";
        return outcome;
      }
      append_page_to_document(*page->second, page_number, &assembly_cursor,
                              &outcome.document, &plain_text, &offsets);
    }
    // Heading depth clusters over the whole document's heights, so it can
    // only run after every page is in.
    assign_section_header_levels(&outcome.document);
    *offsets_ = std::move(offsets);
    outcome.success = true;
    return outcome;
  }

  grpc::CallbackServerContext* context_;
  PageScheduler& scheduler_;
  std::shared_ptr<const std::string> bytes_;
  bool pdf_;
  CvOffsets offsets_;
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
};

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
  // Every dialed leg inherits this call's own ceiling, so no collector is
  // waited on past the patience of the client that asked for the parse. A
  // call with no deadline yields time_point::max(), which leaves each leg on
  // its own static cap exactly as before.
  inputs.inbound_deadline = context->deadline();
  // A collector-folded PDF never rasterized; when previews are on, it gets
  // them rendered so the shell has a page to paint the boxes on.
  inputs.previews = scheduler.captures_page_images();
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
                  *inputs.bytes, inputs.inbound_deadline);
  const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
  if (parsed.outcome.success && route.fast_path) {
    PdfParseResult fast = parsed;
    if (inputs.previews) attach_page_previews(inputs.bytes, &fast.outcome.document);
    return fast.outcome;
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
    const ParseInputs& inputs, const CvCollector& run_cv) {
  std::vector<PlannedCollector> plan;
  for (const auto id : plan_ids) {
    PlannedCollector collector;
    collector.id = id;
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

    const ParseInputs inputs =
        parse_inputs(context, request, scheduler, collectors, bytes, requested_name,
                     base.origin().mimetype());

    const auto cv_offsets = std::make_shared<
        google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>();
    const bool pdf = is_pdf(*bytes, requested_name);
    const CvCollector run_cv(context, scheduler, bytes, pdf, cv_offsets);

    const RoutedPlan routed = route_plan(request.options().collectors(), pdf, inputs);
    CoordinatorResult result = run_collectors(
        build_plan(routed.ids, routed.pdf_routing, inputs, run_cv), std::move(base));
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
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

}  // namespace grparse
