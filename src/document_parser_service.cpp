#include "grparse/document_parser_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chunking/chunker.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_render.h"
#include "parse_support.h"
#include "source_parse.h"
#include "targets/target_step.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// The version every image reports is "grparse-<version>-<flavor>". Both
// halves come from the build: the version from the project() line in
// CMakeLists.txt, so a release bumps one place, and the flavor from the ONNX
// Runtime package, so the OpenVINO and CPU images stop claiming cuda.
#ifndef GRPARSE_VERSION
#define GRPARSE_VERSION "unversioned"
#endif
#ifndef GRPARSE_ORT_PACKAGE_NAME
#define GRPARSE_ORT_PACKAGE_NAME "unknown"
#endif
constexpr const char* kServiceVersion = "grparse-" GRPARSE_VERSION "-" GRPARSE_ORT_PACKAGE_NAME;

// The document's plain text export: its text items in arena order, which is
// each collector's emission order.
std::string document_plain_text(const pipestream::document::v1::Document& document) {
  std::string text;
  for (const auto& item : document.texts()) {
    const pipestream::document::v1::TextItemBase* base = nullptr;
    switch (item.item_case()) {
      case pipestream::document::v1::BaseTextItem::kTitle: base = &item.title().base(); break;
      case pipestream::document::v1::BaseTextItem::kSectionHeader:
        base = &item.section_header().base();
        break;
      case pipestream::document::v1::BaseTextItem::kListItem:
        base = &item.list_item().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFormula: base = &item.formula().base(); break;
      case pipestream::document::v1::BaseTextItem::kText: base = &item.text().base(); break;
      case pipestream::document::v1::BaseTextItem::kCode:
        // CodeItem carries its fields inline instead of a nested base.
        if (!text.empty()) text.push_back('\n');
        text.append(item.code().text());
        break;
      case pipestream::document::v1::BaseTextItem::kFieldHeading:
        base = &item.field_heading().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFieldValue:
        base = &item.field_value().base();
        break;
      case pipestream::document::v1::BaseTextItem::ITEM_NOT_SET: break;
    }
    if (base == nullptr) continue;
    if (!text.empty()) text.push_back('\n');
    text.append(base->text());
  }
  return text;
}

// True when the response must carry this output format. An empty to_formats
// keeps the historical default of the plain-text export alone; every other
// format is opt-in by explicit request.
bool requested(const pipestream::parse::v1::ConvertDocumentOptions& options,
               pipestream::parse::v1::OutputFormat format) {
  if (options.to_formats().empty()) {
    return format == pipestream::parse::v1::OUTPUT_FORMAT_TEXT;
  }
  return std::ranges::find(options.to_formats(), format) !=
         options.to_formats().end();
}

// The collector failures of a parse, as the error list every conversion
// surface reports them under.
void report_failures(const std::vector<CollectorFailureInfo>& failures,
                     google::protobuf::RepeatedPtrField<pipestream::parse::v1::ErrorItem>* errors) {
  for (const auto& failure : failures) {
    auto* error = errors->Add();
    error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
    error->set_module_name(std::string("collector:") + collector_name(failure.id));
    error->set_error_message(failure.error);
    error->set_category(failure_category_for(failure.code));
  }
}

// Renders every output format the options asked for onto the response.
void render_exports(const pipestream::parse::v1::ConvertDocumentOptions& options,
                    const pipestream::document::v1::Document& document,
                    pipestream::parse::v1::DocumentExports* exports) {
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_TEXT)) {
    exports->set_text(document_plain_text(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN)) {
    MarkdownOptions markdown;
    if (options.has_md_page_break_placeholder()) {
      markdown.page_break_placeholder = options.md_page_break_placeholder();
    }
    markdown.compact_tables = options.md_compact_tables();
    if (options.has_caption_placement()) {
      switch (options.caption_placement()) {
        case pipestream::parse::v1::CAPTION_PLACEMENT_LAYOUT:
          markdown.caption_placement = MarkdownOptions::CaptionPlacement::kLayout;
          break;
        case pipestream::parse::v1::CAPTION_PLACEMENT_STANDARD:
          markdown.caption_placement = MarkdownOptions::CaptionPlacement::kStandard;
          break;
        default:
          throw std::invalid_argument("caption_placement is unspecified or unknown");
      }
    }
    if (options.has_image_export_mode()) {
      switch (options.image_export_mode()) {
        case pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kEmbedded;
          break;
        case pipestream::parse::v1::IMAGE_REF_MODE_REFERENCED:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kReferenced;
          break;
        case pipestream::parse::v1::IMAGE_REF_MODE_PLACEHOLDER:
        case pipestream::parse::v1::IMAGE_REF_MODE_UNSPECIFIED:
        default:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kPlaceholder;
          break;
      }
    }
    exports->set_md(render_markdown(document, markdown));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML)) {
    exports->set_html(render_html(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_JSON)) {
    exports->set_json(render_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_CANONICAL_JSON)) {
    exports->set_canonical_json(render_canonical_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_GDOCS_JSON)) {
    exports->set_gdocs_json(render_gdocs_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS)) {
    exports->set_doctags(render_doctags(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG)) {
    exports->set_doclang(render_doclang(document, doclang_options(options)));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_VTT)) {
    exports->set_vtt(render_vtt(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE)) {
    exports->set_html_split_page(render_html_split_page(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_YAML)) {
    exports->set_yaml(render_yaml(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_LATEX)) {
    exports->set_latex(render_latex(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DCLX)) {
    exports->set_dclx(render_dclx(document, doclang_options(options)));
  }
}

// The converted document a chunk response carries when the caller asked for
// it. The chunks themselves never depend on it.
void attach_converted_document(const pipestream::parse::v1::ConvertDocumentOptions& options,
                               const fs::path& filename, CoordinatorResult* result,
                               pipestream::parse::v1::ChunkDocumentResponse* response) {
  auto* entry = response->add_documents();
  entry->set_kind("document");
  auto* content = entry->mutable_content();
  content->set_filename(filename.string());
  *content->mutable_doc() = std::move(result->document);
  render_exports(options, content->doc(), content->mutable_exports());
  entry->set_status(result->failures.empty()
                        ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                        : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  report_failures(result->failures, entry->mutable_errors());
}

// Without the converted document, a parse some collector failed still says
// so: the failures ride a documents entry with no content, the one place a
// chunk response can carry a status and errors.
void attach_failures(const std::vector<CollectorFailureInfo>& failures,
                     pipestream::parse::v1::ChunkDocumentResponse* response) {
  auto* entry = response->add_documents();
  entry->set_kind("document");
  entry->set_status(pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  report_failures(failures, entry->mutable_errors());
}

// The chunk responses have no target result to report a delivery in, so a
// target that asks for one is turned down rather than silently skipped.
grpc::Status refuse_target(const pipestream::parse::v1::Target& target,
                           const std::string& surface) {
  if (!targets::needs_delivery(target)) return grpc::Status::OK;
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      surface + " does not deliver to a target; its chunks return in the "
                                "response body only");
}

// What a copy of the request's sources holds: the chunk surfaces copy them
// into the ConvertDocumentRequest the shared parse reads.
uint64_t sources_bytes(
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources) {
  uint64_t total = 0;
  for (const auto& source : sources) total += source.ByteSizeLong();
  return total;
}

// The reactor the blocking unary surfaces finish through. Construction hands
// `work` to the executor and returns immediately, so the event-manager thread
// that reacted to the call is free the moment the handler returns; the worker
// finishes the call. gRPC queues operations requested before the reactor is
// bound, so finishing from either thread is safe whichever wins the race.
// Cancellation needs no OnCancel here: the work polls the context, which is
// where it can act on the answer.
//
// The request's bytes are charged to the in-flight budget before anything
// queues, and returned when the task that owns the call is gone (run, or
// refused by the executor); a charge past the budget is refused outright.
class ParseUnaryReactor final : public grpc::ServerUnaryReactor {
 public:
  ParseUnaryReactor(grpc::CallbackServerContext* context, CallExecutor& executor,
                    const std::shared_ptr<InflightBytes>& inflight, uint64_t request_bytes,
                    std::function<grpc::Status()> work) {
    if (inflight != nullptr && !inflight->try_acquire(request_bytes)) {
      Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "in-flight document bytes would exceed GRPARSE_MAX_INFLIGHT_BYTES"));
      return;
    }
    // Shared because std::function must stay copyable; the last copy of the
    // task going away is what returns the charge.
    std::shared_ptr<void> charge(nullptr, [inflight, request_bytes](void*) {
      if (inflight != nullptr) inflight->release(request_bytes);
    });
    const bool queued = executor.submit([this, context, work = std::move(work),
                                         charge = std::move(charge)] {
      // A call can wait in the queue behind every conversion ahead of it, so
      // the first thing a worker does is ask whether anyone is still
      // listening. The answer costs one atomic read and saves the whole
      // parse behind it: the base64 decode of up to a few hundred megabytes,
      // and every collector leg after that.
      if (context->IsCancelled()) {
        Finish(grpc::Status(grpc::StatusCode::CANCELLED,
                            "request cancelled before conversion started"));
        return;
      }
      grpc::Status status;
      try {
        status = work();
      } catch (...) {
        status = status_from_exception(std::current_exception());
      }
      // Last statement on purpose: OnDone can delete this reactor from
      // another thread the instant the call completes.
      Finish(std::move(status));
    });
    if (queued) return;
    // The reactor holds the call and is the only thing that can answer it, so
    // a refused submission is a status, not a dropped RPC.
    Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "conversion executor is saturated"));
  }

  void OnDone() override { delete this; }
};

// The trivial surfaces answer on the reaction thread through the context's own
// reactor, which allocates nothing.
grpc::ServerUnaryReactor* finish_inline(grpc::CallbackServerContext* context,
                                        grpc::Status status) {
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}

}  // namespace

DocumentParserService::DocumentParserService(PageScheduler& scheduler,
                                             std::shared_ptr<CollectorEndpoints> endpoints,
                                             CallExecutor::Options executor_options,
                                             std::optional<RepairOptions> repair,
                                             std::shared_ptr<EmbeddingEngine> embedding_engine,
                                             EmbeddingConfig embedding_config,
                                             std::shared_ptr<InflightBytes> inflight)
    : scheduler_(scheduler),
      endpoints_(std::move(endpoints)),
      repair_(std::move(repair)),
      embedder_(std::move(embedding_engine), std::move(embedding_config)),
      inflight_(std::move(inflight)),
      executor_(executor_options) {}

grpc::ServerUnaryReactor* DocumentParserService::ConvertSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ConvertSourceRequest* request,
    pipestream::parse::v1::ConvertSourceResponse* response) {
  // The wire message and the copy its base64 source decodes to.
  const uint64_t charge =
      request->ByteSizeLong() + decoded_source_bytes(request->request().sources());
  return new ParseUnaryReactor(context, executor_, inflight_, charge,
                               [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    SourceParse parsed;
    const grpc::Status parse_status = parse_source(context, request->request(), scheduler_,
                                                   endpoints_, repair_, "ConvertSource", &parsed);
    if (!parse_status.ok()) return parse_status;
    auto& result = parsed.result;
    auto* converted = response->mutable_response();
    auto* document_response = converted->mutable_document();
    document_response->set_filename(parsed.filename.string());
    auto* document = document_response->mutable_doc();
    *document = std::move(result.document);
    report_failures(result.failures, converted->mutable_errors());
    if (parsed.confidence.has_value()) *converted->mutable_confidence() = *parsed.confidence;
    converted->mutable_structure_findings()->Swap(&parsed.structure_findings);
    *converted->mutable_text_offsets() = chunking::offset_rows(parsed.offsets);
    // Every requested output format renders from the same merged document;
    // TEXT keeps its arena-order line export, the rest fold the body tree.
    const auto& options = request->request().options();
    render_exports(options, *document, document_response->mutable_exports());
    const bool wants_chunks =
        std::ranges::find(options.to_formats(), pipestream::parse::v1::OUTPUT_FORMAT_CHUNKS) !=
        options.to_formats().end();
    if (wants_chunks) {
      std::vector<pipestream::parse::v1::Chunk> chunks;
      if (options.chunking_options_case() ==
          pipestream::parse::v1::ConvertDocumentOptions::kHybridChunking) {
        const grpc::Status chunked = chunking::chunk_hybrid(
            *document, parsed.offsets, options.hybrid_chunking(), parsed.filename.string(),
            &chunks);
        if (!chunked.ok()) return chunked;
      } else {
        // hierarchical_chunking, chunking_preset, or CHUNKS with no config:
        // hierarchical defaults (validate_options admits no other preset).
        pipestream::parse::v1::HierarchicalChunkerOptions hierarchical;
        if (options.chunking_options_case() ==
            pipestream::parse::v1::ConvertDocumentOptions::kHierarchicalChunking) {
          hierarchical = options.hierarchical_chunking();
        }
        const chunking::ChunkOptions chunk_opts = chunking::chunk_options_from(hierarchical);
        chunks = chunking::chunk_hierarchical(*document, parsed.offsets, chunk_opts,
                                              parsed.filename.string());
      }
      for (auto& chunk : chunks) {
        *converted->add_chunks() = std::move(chunk);
      }
    }
    // The target delivers the same conversion somewhere else; the response
    // body above keeps everything it already carries either way. It runs
    // here on the conversion's own worker because it compresses and uploads,
    // which is not work a gRPC event thread may be handed.
    const auto& target = request->request().target();
    bool delivery_failed = false;
    if (targets::needs_delivery(target)) {
      const grpc::Status delivered =
          targets::deliver(target, *document, document_response->exports(),
                           converted->mutable_target_result());
      if (!delivered.ok()) {
        // Delivery is additive, never a replacement: the conversion the
        // response already carries survives a store that would not take it.
        // The failure lands as an error item and a partial status, except a
        // misconfigured target itself, which the caller must fix and gets
        // told about at the RPC layer.
        if (delivered.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
            delivered.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
          return delivered;
        }
        delivery_failed = true;
        auto* error = converted->add_errors();
        error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
        error->set_module_name("target-delivery");
        error->set_error_message(delivered.error_message());
        error->set_category(pipestream::parse::v1::FAILURE_CATEGORY_TARGET_UNAVAILABLE);
      }
    }
    converted->set_status(result.failures.empty() && !delivery_failed
                              ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                              : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
    converted->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    (*converted->mutable_timings())["total"] = converted->processing_time();
    // Full ProfilingItem sample matching serve's additive profiling map; totals
    // stay on timings for older clients.
    auto& profile = (*converted->mutable_profiling())["total"];
    profile.set_scope(pipestream::parse::v1::PROFILING_SCOPE_DOCUMENT);
    profile.set_count(1);
    profile.add_times(converted->processing_time());
    return grpc::Status::OK;
  });
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHierarchicalSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
    pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) {
  // The wire message, the sources copied into the parse request, and the
  // copy their base64 decodes to.
  const uint64_t charge = request->ByteSizeLong() +
                          sources_bytes(request->request().sources()) +
                          decoded_source_bytes(request->request().sources());
  return new ParseUnaryReactor(context, executor_, inflight_, charge,
                               [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    const auto& chunk_request = request->request();
    const grpc::Status target_status =
        refuse_target(chunk_request.target(), "ChunkHierarchicalSource");
    if (!target_status.ok()) return target_status;
    const auto embedding_status = embedder_.validate(chunk_request.embedding_options());
    if (!embedding_status.ok()) return embedding_status;
    SourceParse parsed;
    pipestream::parse::v1::ConvertDocumentRequest convert;
    *convert.mutable_sources() = chunk_request.sources();
    *convert.mutable_options() = chunk_request.convert_options();
    const grpc::Status parse_status =
        parse_source(context, convert, scheduler_, endpoints_, repair_, "ChunkHierarchicalSource",
                     &parsed);
    if (!parse_status.ok()) return parse_status;
    auto* chunked = response->mutable_response();
    const chunking::ChunkOptions options =
        chunking::chunk_options_from(chunk_request.chunking_options());
    auto chunks = chunking::chunk_hierarchical(parsed.result.document, parsed.offsets,
                                              options, parsed.filename.string());
    const auto embedded = embedder_.embed(chunk_request.embedding_options(),
                                          [context] { return context->IsCancelled(); }, &chunks);
    if (!embedded.ok()) return embedded;
    for (auto& chunk : chunks) {
      *chunked->add_chunks() = std::move(chunk);
    }
    if (chunk_request.include_converted_doc()) {
      attach_converted_document(chunk_request.convert_options(), parsed.filename,
                                &parsed.result, chunked);
    } else if (!parsed.result.failures.empty()) {
      attach_failures(parsed.result.failures, chunked);
    }
    chunking::fill_chunking_info(chunk_request.chunking_options(),
                                 chunked->mutable_chunking_info());
    chunked->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return grpc::Status::OK;
  });
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHybridSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHybridSourceRequest* request,
    pipestream::parse::v1::ChunkHybridSourceResponse* response) {
  // The wire message, the sources copied into the parse request, and the
  // copy their base64 decodes to.
  const uint64_t charge = request->ByteSizeLong() +
                          sources_bytes(request->request().sources()) +
                          decoded_source_bytes(request->request().sources());
  return new ParseUnaryReactor(context, executor_, inflight_, charge,
                               [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    const auto& chunk_request = request->request();
    const grpc::Status target_status = refuse_target(chunk_request.target(), "ChunkHybridSource");
    if (!target_status.ok()) return target_status;
    // The budget decides every boundary, so it is validated before any work
    // starts rather than defaulted to a number nobody asked for.
    const grpc::Status option_status =
        chunking::validate_hybrid_options(chunk_request.chunking_options());
    if (!option_status.ok()) return option_status;
    const auto embedding_status = embedder_.validate(chunk_request.embedding_options());
    if (!embedding_status.ok()) return embedding_status;
    SourceParse parsed;
    pipestream::parse::v1::ConvertDocumentRequest convert;
    *convert.mutable_sources() = chunk_request.sources();
    *convert.mutable_options() = chunk_request.convert_options();
    const grpc::Status parse_status =
        parse_source(context, convert, scheduler_, endpoints_, repair_, "ChunkHybridSource",
                     &parsed);
    if (!parse_status.ok()) return parse_status;
    auto* chunked = response->mutable_response();
    std::vector<pipestream::parse::v1::Chunk> chunks;
    const grpc::Status chunk_status =
        chunking::chunk_hybrid(parsed.result.document, parsed.offsets,
                               chunk_request.chunking_options(), parsed.filename.string(),
                               &chunks);
    if (!chunk_status.ok()) return chunk_status;
    const auto embedded = embedder_.embed(chunk_request.embedding_options(),
                                          [context] { return context->IsCancelled(); }, &chunks);
    if (!embedded.ok()) return embedded;
    for (auto& chunk : chunks) {
      *chunked->add_chunks() = std::move(chunk);
    }
    if (chunk_request.include_converted_doc()) {
      attach_converted_document(chunk_request.convert_options(), parsed.filename,
                                &parsed.result, chunked);
    } else if (!parsed.result.failures.empty()) {
      attach_failures(parsed.result.failures, chunked);
    }
    chunking::fill_chunking_info(chunk_request.chunking_options(),
                                 chunked->mutable_chunking_info());
    chunked->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return grpc::Status::OK;
  });
}

grpc::ServerUnaryReactor* DocumentParserService::Health(
    grpc::CallbackServerContext* context, const pipestream::parse::v1::HealthRequest*,
    pipestream::parse::v1::HealthResponse* response) {
  response->set_status("ready");
  response->set_version(kServiceVersion);
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::GetServiceInfo(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::GetServiceInfoRequest*,
    pipestream::parse::v1::GetServiceInfoResponse* response) {
  response->set_name("gRParse");
  response->set_version(kServiceVersion);
  auto* ui = response->mutable_ui();
  ui->set_title("gRParse");
  ui->set_path("/ui/grparse");
  ui->set_description("Diskless PDF/image to page-streamed protobuf with OCR and layout");
  const auto embeddings = embedder_.capabilities();
  if (embeddings.available()) *response->mutable_embeddings() = embeddings;
  return finish_inline(context, grpc::Status::OK);
}

}  // namespace grparse
