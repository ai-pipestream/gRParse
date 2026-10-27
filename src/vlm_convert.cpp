#include "grparse/vlm_convert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/vlm/v1/vlm_convert.grpc.pb.h"
#include "collectors/collector_support.h"
#include "grparse/document_merge.h"
#include "grparse/in_memory_document.h"
#include "grparse/page_previews.h"
#include "grparse/page_source_watch.h"

namespace grparse {
namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;
namespace parsev1 = ai::pipestream::parse::v1;

void type_stop_reasons_in(google::protobuf::Message* message) {
  if (auto* generation = dynamic_cast<docv1::GenerationSource*>(message)) {
    if (generation->stop_reason() == docv1::VLM_STOP_REASON_UNSPECIFIED &&
        generation->has_finish_reason()) {
      generation->set_stop_reason(vlm_stop_reason(generation->finish_reason()));
    }
    return;
  }
  const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
  const google::protobuf::Reflection* reflection = message->GetReflection();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(index);
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE || field->is_map()) {
      continue;
    }
    if (field->is_repeated()) {
      const int size = reflection->FieldSize(*message, field);
      for (int item = 0; item < size; ++item) {
        type_stop_reasons_in(reflection->MutableRepeatedMessage(message, field, item));
      }
    } else if (reflection->HasField(*message, field)) {
      type_stop_reasons_in(reflection->MutableMessage(message, field));
    }
  }
}

vlmv1::VlmPreset preset_for(parsev1::VlmModelType model) {
  switch (model) {
    case parsev1::VLM_MODEL_TYPE_SMOLDOCLING:
      return vlmv1::VLM_PRESET_SMOLDOCLING;
    case parsev1::VLM_MODEL_TYPE_GRANITEDOCLING:
      return vlmv1::VLM_PRESET_GRANITE_DOCLING;
    case parsev1::VLM_MODEL_TYPE_GOT_OCR_2:
      return vlmv1::VLM_PRESET_GOT_OCR_2;
    case parsev1::VLM_MODEL_TYPE_GRANITE_VISION:
      return vlmv1::VLM_PRESET_GRANITE_VISION;
    case parsev1::VLM_MODEL_TYPE_DEEPSEEKOCR_OLLAMA:
      return vlmv1::VLM_PRESET_DEEPSEEK_OCR;
    case parsev1::VLM_MODEL_TYPE_NANONETS_OCR2:
      return vlmv1::VLM_PRESET_NANONETS_OCR2;
    case parsev1::VLM_MODEL_TYPE_GLMOCR:
      return vlmv1::VLM_PRESET_GLM_OCR;
    case parsev1::VLM_MODEL_TYPE_LIGHTONOCR:
      return vlmv1::VLM_PRESET_LIGHTON_OCR;
    case parsev1::VLM_MODEL_TYPE_UNSPECIFIED:
      return vlmv1::VLM_PRESET_UNSPECIFIED;
    default:
      // vLLM / Ollama / LM Studio variants and anything else: open vocabulary.
      return vlmv1::VLM_PRESET_RAW;
  }
}

std::string raw_name_for(parsev1::VlmModelType model) {
  const std::string name = parsev1::VlmModelType_Name(model);
  return name.empty() ? std::to_string(static_cast<int>(model)) : name;
}

// "vlm convert: page 3: PAGE_WARNING_CODE_TABLE_TRUNCATED: <detail> (#/tables/0)".
// A code this build does not know yet prints as its number. The ref is in
// the page fragment's ref space, before the merge renumbers it.
std::string page_warning_text(uint32_t page_no, const vlmv1::PageWarning& warning) {
  std::string code = vlmv1::PageWarningCode_Name(warning.code());
  if (code.empty()) code = "page warning " + std::to_string(static_cast<int>(warning.code()));
  std::string text = "vlm convert: page " + std::to_string(page_no) + ": " + code;
  if (!warning.message().empty()) text += ": " + warning.message();
  if (!warning.ref().empty()) text += " (" + warning.ref() + ")";
  return text;
}

docv1::CollectorSource vlm_claimant() {
  docv1::CollectorSource source;
  source.set_collector("vlm-convert");
  return source;
}

}  // namespace

docv1::VlmStopReason vlm_stop_reason(std::string_view finish_reason) {
  if (finish_reason == "length") return docv1::VLM_STOP_REASON_LENGTH;
  if (finish_reason == "stop_sequence") return docv1::VLM_STOP_REASON_STOP_SEQUENCE;
  if (finish_reason == "end_of_sequence" || finish_reason == "stop") {
    return docv1::VLM_STOP_REASON_END_OF_SEQUENCE;
  }
  if (finish_reason == "content_filter") return docv1::VLM_STOP_REASON_CONTENT_FILTERED;
  if (finish_reason == "inference_error") return docv1::VLM_STOP_REASON_INFERENCE_ERROR;
  return docv1::VLM_STOP_REASON_UNSPECIFIED;
}

void type_generation_stop_reasons(docv1::Document* document) { type_stop_reasons_in(document); }

namespace {

struct ResponseFormatName {
  parsev1::ResponseFormat format;
  std::string_view name;
};

// docling's ResponseFormat values (docling 2.142.1), in its declaration order.
constexpr ResponseFormatName kResponseFormatNames[] = {
    {parsev1::RESPONSE_FORMAT_DOCTAGS, "doctags"},
    {parsev1::RESPONSE_FORMAT_DOCLANG, "doclang"},
    {parsev1::RESPONSE_FORMAT_MARKDOWN, "markdown"},
    {parsev1::RESPONSE_FORMAT_DEEPSEEKOCR_MARKDOWN, "deepseekocr_markdown"},
    {parsev1::RESPONSE_FORMAT_UNLIMITED_OCR_MARKDOWN, "unlimited_ocr_markdown"},
    {parsev1::RESPONSE_FORMAT_HTML, "html"},
    {parsev1::RESPONSE_FORMAT_OTSL, "otsl"},
    {parsev1::RESPONSE_FORMAT_PLAINTEXT, "plaintext"},
    {parsev1::RESPONSE_FORMAT_CHANDRA_HTML, "chandra_html"},
    {parsev1::RESPONSE_FORMAT_DOTS_JSON, "dots_json"},
    {parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V2, "nemotron_parse_v2"},
    {parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V3_5, "nemotron_parse_v3_5"},
    {parsev1::RESPONSE_FORMAT_MINERU2, "mineru2"},
};

}  // namespace

std::optional<std::string_view> response_format_name(parsev1::ResponseFormat format) {
  for (const ResponseFormatName& entry : kResponseFormatNames) {
    if (entry.format == format) return entry.name;
  }
  return std::nullopt;
}

std::optional<parsev1::ResponseFormat> response_format_from_name(std::string_view name) {
  for (const ResponseFormatName& entry : kResponseFormatNames) {
    if (entry.name == name) return entry.format;
  }
  return std::nullopt;
}

std::optional<vlmv1::ResponseFormat> vlm_collector_response_format(
    parsev1::ResponseFormat format) {
  switch (format) {
    case parsev1::RESPONSE_FORMAT_DOCTAGS: return vlmv1::RESPONSE_FORMAT_DOCTAGS;
    case parsev1::RESPONSE_FORMAT_MARKDOWN: return vlmv1::RESPONSE_FORMAT_MARKDOWN;
    case parsev1::RESPONSE_FORMAT_HTML: return vlmv1::RESPONSE_FORMAT_HTML;
    case parsev1::RESPONSE_FORMAT_OTSL: return vlmv1::RESPONSE_FORMAT_OTSL;
    case parsev1::RESPONSE_FORMAT_PLAINTEXT: return vlmv1::RESPONSE_FORMAT_PLAINTEXT;
    case parsev1::RESPONSE_FORMAT_DOCLANG: return vlmv1::RESPONSE_FORMAT_DOCLANG;
    case parsev1::RESPONSE_FORMAT_DEEPSEEKOCR_MARKDOWN:
      return vlmv1::RESPONSE_FORMAT_DEEPSEEKOCR_MARKDOWN;
    case parsev1::RESPONSE_FORMAT_UNLIMITED_OCR_MARKDOWN:
      return vlmv1::RESPONSE_FORMAT_UNLIMITED_OCR_MARKDOWN;
    case parsev1::RESPONSE_FORMAT_CHANDRA_HTML: return vlmv1::RESPONSE_FORMAT_CHANDRA_HTML;
    case parsev1::RESPONSE_FORMAT_DOTS_JSON: return vlmv1::RESPONSE_FORMAT_DOTS_JSON;
    case parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V2:
      return vlmv1::RESPONSE_FORMAT_NEMOTRON_PARSE_V2;
    case parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V3_5:
      return vlmv1::RESPONSE_FORMAT_NEMOTRON_PARSE_V3_5;
    // MinerU2 is a two-pass protocol the collector does not drive; the
    // option is refused NOT_IMPLEMENTED by field before any page is sent.
    default: return std::nullopt;
  }
}

void apply_vlm_convert_options(const parsev1::ConvertDocumentOptions& convert,
                               VlmConvertOptions* options) {
  if (options == nullptr) return;
  // docling's VlmConvertOptions as a custom config: the spec's repo is the
  // model (VlmModelSpec.get_repo_id with no engine override), its prompt the
  // prompt, and scale the image scale. Any explicit selection below (model,
  // local, api, preset) still wins.
  if (convert.has_vlm_pipeline_custom_config()) {
    const auto& custom = convert.vlm_pipeline_custom_config();
    if (!custom.model_spec().default_repo_id().empty()) {
      options->preset = vlmv1::VLM_PRESET_RAW;
      options->preset_raw = custom.model_spec().default_repo_id();
    }
    if (custom.model_spec().has_prompt() && !custom.model_spec().prompt().empty()) {
      options->prompt = custom.model_spec().prompt();
    }
    if (custom.has_scale() && custom.scale() > 0.0) options->scale = custom.scale();
  }
  if (convert.has_vlm_pipeline_model() &&
      convert.vlm_pipeline_model() != parsev1::VLM_MODEL_TYPE_UNSPECIFIED) {
    options->preset = preset_for(convert.vlm_pipeline_model());
    if (options->preset == vlmv1::VLM_PRESET_RAW) {
      options->preset_raw = raw_name_for(convert.vlm_pipeline_model());
    }
  }
  if (convert.has_vlm_pipeline_model_local()) {
    const auto& local = convert.vlm_pipeline_model_local();
    if (local.has_repo_id() && !local.repo_id().empty()) {
      options->preset = vlmv1::VLM_PRESET_RAW;
      options->preset_raw = local.repo_id();
    }
    if (local.has_scale() && local.scale() > 0.0) {
      options->scale = local.scale();
    }
  }
  if (convert.has_vlm_pipeline_model_api()) {
    const auto& api = convert.vlm_pipeline_model_api();
    if (api.has_url() && !api.url().empty()) {
      if (api.url().starts_with("http://") || api.url().starts_with("https://")) {
        options->endpoint = api.url();
      } else {
        options->preset = vlmv1::VLM_PRESET_RAW;
        options->preset_raw = api.url();
      }
    }
    if (api.has_scale() && api.scale() > 0.0 && options->scale <= 0.0) {
      options->scale = api.scale();
    }
  }
  std::optional<parsev1::ResponseFormat> format;
  if (convert.has_vlm_pipeline_model_local()) {
    format = named_response_format(convert.vlm_pipeline_model_local());
  } else if (convert.has_vlm_pipeline_model_api()) {
    format = named_response_format(convert.vlm_pipeline_model_api());
  }
  if (!format.has_value() && convert.has_vlm_pipeline_custom_config()) {
    format = named_response_format(convert.vlm_pipeline_custom_config().model_spec());
  }
  if (format.has_value()) {
    if (const auto collector = vlm_collector_response_format(*format); collector.has_value()) {
      options->response_format = *collector;
      // docling's spelling rides along for the collector to check against
      // the typed value (ConvertOptions.response_format_raw).
      options->response_format_raw = std::string(response_format_name(*format).value_or(""));
    }
  }
  if (convert.has_vlm_pipeline_preset() && !convert.vlm_pipeline_preset().empty()) {
    options->preset = vlmv1::VLM_PRESET_RAW;
    options->preset_raw = convert.vlm_pipeline_preset();
  }
  if (convert.has_images_scale() && convert.images_scale() > 0.0) {
    options->scale = convert.images_scale();
  }
  if (convert.has_render_scale() && convert.render_scale() > 0.0) {
    options->render_dpi = convert.render_scale() * 72.0;
    if (options->scale <= 0.0) options->scale = convert.render_scale();
  }
  if (convert.has_abort_on_error()) options->abort_on_error = convert.abort_on_error();
  if (convert.has_page_range()) {
    options->page_range =
        std::make_pair(convert.page_range().start(), convert.page_range().end());
  }
}

VlmConvertReport convert_vlm_pages(const std::shared_ptr<grpc::Channel>& channel,
                                   const VlmConvertOptions& options,
                                   std::shared_ptr<const std::string> bytes, bool pdf,
                                   docv1::Document* document,
                                   CollectorDeadline inbound_deadline,
                                   const std::function<bool()>& cancelled) {
  VlmConvertReport report;
  if (document == nullptr || bytes == nullptr) {
    report.error = "vlm convert: missing document or bytes";
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }
  if (channel == nullptr || !options.enabled()) {
    report.error = "vlm convert: grpc-vlm-convert is not configured "
                   "(GRPARSE_VLM_CONVERT_TARGET)";
    report.code = grpc::StatusCode::FAILED_PRECONDITION;
    return report;
  }

  const double dpi = options.render_dpi > 0.0 ? options.render_dpi : kDefaultRenderDpi;
  std::shared_ptr<PageSource> source;
  try {
    // A PDF makes its opening backend call on first use, after the watch
    // below has tied the source to this request.
    source = open_in_memory_document(std::move(bytes), pdf, dpi, SourceOpening::kOnFirstUse);
  } catch (const PdfBackendNotConfigured& ex) {
    report.error = std::string("vlm convert: ") + ex.what();
    report.code = grpc::StatusCode::FAILED_PRECONDITION;
    return report;
  } catch (const std::exception& ex) {
    report.error = std::string("vlm convert: could not open document: ") + ex.what();
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }
  if (!source) {
    report.error = "vlm convert: document has no pages";
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }

  // The source's backend calls end with the request: they run no later than
  // its deadline, and a call in flight (the opening Probe included) is
  // cancelled once the caller goes.
  const PageSourceWatch source_watch(source, inbound_deadline, [&cancelled, inbound_deadline] {
    return (cancelled && cancelled()) || std::chrono::system_clock::now() >= inbound_deadline;
  });

  int page_count = 0;
  try {
    page_count = source->page_count();
  } catch (const std::exception& ex) {
    if (cancelled && cancelled()) {
      report.error = "vlm convert: request cancelled";
      report.code = grpc::StatusCode::CANCELLED;
    } else if (std::chrono::system_clock::now() >= inbound_deadline) {
      report.error = "vlm convert: deadline exceeded while opening the document";
      report.code = grpc::StatusCode::DEADLINE_EXCEEDED;
    } else {
      report.error = std::string("vlm convert: could not open document: ") + ex.what();
      report.code = grpc::StatusCode::INVALID_ARGUMENT;
    }
    return report;
  }
  if (page_count <= 0) {
    report.error = "vlm convert: document has no pages";
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }

  int first = 1;
  int last = page_count;
  if (options.page_range.has_value()) {
    first = options.page_range->first;
    last = options.page_range->second;
    if (first < 1 || last < first) {
      report.error = "vlm convert: page_range must be a 1-indexed inclusive span";
      report.code = grpc::StatusCode::INVALID_ARGUMENT;
      return report;
    }
    if (first > page_count) {
      report.error = "vlm convert: page_range start is past the end of the document";
      report.code = grpc::StatusCode::INVALID_ARGUMENT;
      return report;
    }
    if (last > page_count) last = page_count;
  }

  // Pages render, encode and go out one at a time, so memory holds one page's
  // raster and PNG, not the whole document's. The stream opens on the first
  // page that encodes: a document with nothing to send never dials the peer.
  auto stub = vlmv1::VlmConvertService::NewStub(channel);
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<vlmv1::ConvertPagesRequest,
                                           vlmv1::ConvertPagesResponse>>
      stream;
  // Cancels the open stream once the caller is gone, through the write
  // phase and the read loop alike, like every other collector's call.
  std::optional<CancelWatch> watch;
  // The responses are read on their own thread while pages still go out: a
  // peer that answers as it converts stops reading once its response channel
  // and this client's receive window are full, and an upload that sent every
  // page before reading would stall against it until the deadline. Only the
  // reader touches `document` and `answers` until it is joined.
  struct Answers {
    int pages_ok = 0;
    int pages_failed = 0;
    std::vector<std::string> warnings;
    std::optional<std::string> abort_error;
  } answers;
  std::atomic<bool> reader_aborted{false};
  std::thread reader;
  const auto join_reader = [&] {
    if (reader.joinable()) reader.join();
  };
  // A return that leaves the reader running cancels the call first, so the
  // reader cannot stay blocked.
  struct ReaderGuard {
    grpc::ClientContext& context;
    std::thread& reader;
    ~ReaderGuard() {
      if (!reader.joinable()) return;
      context.TryCancel();
      reader.join();
    }
  } reader_guard{context, reader};
  const auto start_reader = [&] {
    reader = std::thread([&] {
      const auto claimant = vlm_claimant();
      vlmv1::ConvertPagesResponse event;
      while (stream->Read(&event)) {
        if (event.has_page_document()) {
          const vlmv1::PageDocument& page = event.page_document();
          // A fragment cut to a server cap still merges; the cut is
          // reported beside it, by code name, with the fragment's own ref.
          for (const vlmv1::PageWarning& warning : page.warnings()) {
            answers.warnings.push_back(page_warning_text(page.page_no(), warning));
          }
          docv1::Document fragment = page.document();
          type_generation_stop_reasons(&fragment);
          merge_documents(std::move(fragment), document, claimant);
          ++answers.pages_ok;
        } else if (event.has_page_raw()) {
          ++answers.pages_failed;
          std::string detail = "vlm convert: page " +
                               std::to_string(event.page_raw().page_no()) + " raw";
          if (!event.page_raw().error().empty()) detail += ": " + event.page_raw().error();
          else if (!event.page_raw().text().empty()) detail += " (unmapped text)";
          answers.warnings.push_back(std::move(detail));
          if (options.abort_on_error && !answers.abort_error.has_value()) {
            answers.abort_error = answers.warnings.back();
            reader_aborted.store(true);
            context.TryCancel();
          }
        }
        event.Clear();
      }
    });
  };
  vlmv1::ConvertPagesRequest frame;
  bool written = true;
  // Ends the dial early: the peer is told to stop before Finish, which would
  // otherwise wait for it to finish streaming.
  const auto abandon = [&](grpc::StatusCode code, std::string error) {
    report.error = std::move(error);
    report.code = code;
    if (stream) {
      context.TryCancel();
      join_reader();
      const grpc::Status ignored = stream->Finish();
      (void)ignored;
    }
    return report;
  };
  for (int page_no = first; page_no <= last && written && !reader_aborted.load(); ++page_no) {
    if (cancelled && cancelled()) {
      return abandon(grpc::StatusCode::CANCELLED, "vlm convert: request cancelled");
    }
    if (std::chrono::system_clock::now() >= inbound_deadline) {
      return abandon(grpc::StatusCode::DEADLINE_EXCEEDED,
                     "vlm convert: deadline exceeded before page " + std::to_string(page_no));
    }
    cv::Mat raster;
    try {
      raster = source->render_page(page_no);
    } catch (const std::exception& ex) {
      report.warnings.push_back("vlm convert: page " + std::to_string(page_no) +
                                " render failed: " + ex.what());
      if (options.abort_on_error) {
        return abandon(grpc::StatusCode::INTERNAL, report.warnings.back());
      }
      continue;
    }
    if (raster.empty()) {
      report.warnings.push_back("vlm convert: page " + std::to_string(page_no) +
                                " rendered empty");
      if (options.abort_on_error) {
        return abandon(grpc::StatusCode::INTERNAL, report.warnings.back());
      }
      continue;
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", raster, png, kPngEncodeParams)) {
      report.warnings.push_back("vlm convert: page " + std::to_string(page_no) +
                                " PNG encode failed");
      if (options.abort_on_error) {
        return abandon(grpc::StatusCode::INTERNAL, report.warnings.back());
      }
      continue;
    }
    if (!stream) {
      context.set_deadline(capped_collector_deadline(inbound_deadline, options.timeout));
      context.set_wait_for_ready(false);
      stream = stub->ConvertPages(&context);
      watch.emplace(context, cancelled);
      vlmv1::ConvertOptions* request_options = frame.mutable_options();
      request_options->set_preset(options.preset);
      if (!options.preset_raw.empty()) request_options->set_preset_raw(options.preset_raw);
      request_options->set_response_format(options.response_format);
      if (!options.response_format_raw.empty()) {
        request_options->set_response_format_raw(options.response_format_raw);
      }
      if (!options.prompt.empty()) request_options->set_prompt(options.prompt);
      if (options.scale > 0.0) request_options->set_scale(options.scale);
      if (!options.endpoint.empty()) request_options->set_endpoint(options.endpoint);
      if (options.concurrency != 0) request_options->set_concurrency(options.concurrency);
      request_options->set_abort_on_error(options.abort_on_error);
      written = stream->Write(frame);
      if (!written) break;
      start_reader();
    }
    frame.Clear();
    vlmv1::PageImage* image = frame.mutable_page_image();
    image->set_png(reinterpret_cast<const char*>(png.data()), png.size());
    image->set_page_no(static_cast<uint32_t>(page_no));
    image->set_width(static_cast<uint32_t>(raster.cols));
    image->set_height(static_cast<uint32_t>(raster.rows));
    written = stream->Write(frame);
    if (written) ++report.pages_sent;
  }
  if (!stream) {
    report.error = "vlm convert: no pages could be rasterized";
    report.code = grpc::StatusCode::INTERNAL;
    return report;
  }
  stream->WritesDone();
  join_reader();
  const grpc::Status status = stream->Finish();
  report.pages_ok = answers.pages_ok;
  report.pages_failed = answers.pages_failed;
  for (std::string& warning : answers.warnings) report.warnings.push_back(std::move(warning));
  if (answers.abort_error.has_value()) {
    report.error = std::move(*answers.abort_error);
    report.code = grpc::StatusCode::INTERNAL;
    return report;
  }
  if (!written) {
    report.error = "vlm convert: failed to write ConvertPages stream: " + status.error_message();
    report.code = status.error_code() == grpc::StatusCode::OK ? grpc::StatusCode::UNAVAILABLE
                                                              : status.error_code();
    return report;
  }
  if (!status.ok()) {
    report.error = "vlm convert: ConvertPages failed: " + status.error_message();
    report.code = status.error_code();
    return report;
  }
  if (report.pages_ok == 0) {
    report.error = report.warnings.empty()
                       ? "vlm convert: peer returned no page documents"
                       : report.warnings.front();
    report.code = grpc::StatusCode::INTERNAL;
    return report;
  }
  if (raw_heading_depth_format(options.response_format)) {
    normalize_section_header_levels(document);
  }
  report.success = true;
  return report;
}

bool raw_heading_depth_format(vlmv1::ResponseFormat format) {
  switch (format) {
    case vlmv1::RESPONSE_FORMAT_CHANDRA_HTML:
    case vlmv1::RESPONSE_FORMAT_DEEPSEEKOCR_MARKDOWN:
    case vlmv1::RESPONSE_FORMAT_DOTS_JSON:
    case vlmv1::RESPONSE_FORMAT_NEMOTRON_PARSE_V2:
    case vlmv1::RESPONSE_FORMAT_NEMOTRON_PARSE_V3_5:
    case vlmv1::RESPONSE_FORMAT_UNLIMITED_OCR_MARKDOWN:
      return true;
    default:
      return false;
  }
}

void normalize_section_header_levels(docv1::Document* document) {
  if (document == nullptr) return;
  for (const auto& item : document->texts()) {
    if (item.has_section_header() && item.section_header().level() <= 1) return;
  }
  for (auto& item : *document->mutable_texts()) {
    if (item.has_section_header()) {
      auto* header = item.mutable_section_header();
      header->set_level(header->level() - 1);
    }
  }
}

}  // namespace grparse
