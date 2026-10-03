#include "grparse/vlm_convert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

docv1::CollectorSource vlm_claimant() {
  docv1::CollectorSource source;
  source.set_collector("vlm-convert");
  return source;
}

}  // namespace

void apply_vlm_convert_options(const parsev1::ConvertDocumentOptions& convert,
                               VlmConvertOptions* options) {
  if (options == nullptr) return;
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
    source = open_in_memory_document(std::move(bytes), pdf, dpi);
  } catch (const PdfBackendNotConfigured& ex) {
    report.error = std::string("vlm convert: ") + ex.what();
    report.code = grpc::StatusCode::FAILED_PRECONDITION;
    return report;
  } catch (const std::exception& ex) {
    report.error = std::string("vlm convert: could not open document: ") + ex.what();
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }
  if (!source || source->page_count() <= 0) {
    report.error = "vlm convert: document has no pages";
    report.code = grpc::StatusCode::INVALID_ARGUMENT;
    return report;
  }

  int first = 1;
  int last = source->page_count();
  if (options.page_range.has_value()) {
    first = options.page_range->first;
    last = options.page_range->second;
    if (first < 1 || last < first) {
      report.error = "vlm convert: page_range must be a 1-indexed inclusive span";
      report.code = grpc::StatusCode::INVALID_ARGUMENT;
      return report;
    }
    if (first > source->page_count()) {
      report.error = "vlm convert: page_range start is past the end of the document";
      report.code = grpc::StatusCode::INVALID_ARGUMENT;
      return report;
    }
    if (last > source->page_count()) last = source->page_count();
  }

  // The source's backend calls end with the request: they run no later than
  // its deadline, and a render in flight is cancelled once the caller goes.
  const PageSourceWatch source_watch(source, inbound_deadline, [&cancelled, inbound_deadline] {
    return (cancelled && cancelled()) || std::chrono::system_clock::now() >= inbound_deadline;
  });

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
          docv1::Document fragment = event.page_document().document();
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
  report.success = true;
  return report;
}

}  // namespace grparse
