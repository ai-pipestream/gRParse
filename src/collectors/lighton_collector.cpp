// grpc-lighton-ocr: LightOnOCR-3 over one bidirectional ParseDocument
// stream. Pages render here, one at a time, and go up on their own thread
// while the page documents come back; the fragments merge in page order once
// the trailer is in.

#include "grparse/lighton_ocr.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/lightonocr/v1/lighton_ocr.grpc.pb.h"
#include "collector_support.h"
#include "grparse/document_merge.h"
#include "grparse/in_memory_document.h"
#include "grparse/page_previews.h"
#include "grparse/page_source_watch.h"

namespace grparse {
namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace lightonv1 = ai::pipestream::lightonocr::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;

// Adds `offset` to every page number a fragment carries (provenance, page
// entries, generation traces, outline entries: the int32 fields named
// page_no) and re-keys its page map, so a page the wire numbered n lands on
// the document page it was rendered from.
void shift_page_numbers_in(google::protobuf::Message* message, int32_t offset) {
  const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
  const google::protobuf::Reflection* reflection = message->GetReflection();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(index);
    if (field->is_map()) continue;
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32 &&
        !field->is_repeated() && field->name() == "page_no") {
      const int32_t value = reflection->GetInt32(*message, field);
      const bool set = field->has_presence() ? reflection->HasField(*message, field) : value != 0;
      if (set) reflection->SetInt32(message, field, value + offset);
      continue;
    }
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) continue;
    if (field->is_repeated()) {
      const int size = reflection->FieldSize(*message, field);
      for (int item = 0; item < size; ++item) {
        shift_page_numbers_in(reflection->MutableRepeatedMessage(message, field, item), offset);
      }
    } else if (reflection->HasField(*message, field)) {
      shift_page_numbers_in(reflection->MutableMessage(message, field), offset);
    }
  }
}

void shift_page_numbers(docv1::Document* document, int32_t offset) {
  if (offset == 0) return;
  shift_page_numbers_in(document, offset);
  google::protobuf::Map<int32_t, docv1::PageItem> shifted;
  for (auto& [page_no, page] : *document->mutable_pages()) {
    docv1::PageItem moved = std::move(page);
    shift_page_numbers_in(&moved, offset);
    shifted[page_no + offset] = std::move(moved);
  }
  document->mutable_pages()->swap(shifted);
}

std::string page_label(int page_no) { return "lighton ocr: page " + std::to_string(page_no); }

// "lighton ocr: page 3: PAGE_FAILURE_CODE_INFERENCE: <message>". A code this
// build does not know yet prints as its number.
std::string page_failure_text(int page_no, const lightonv1::PageFailure& failure) {
  std::string code = lightonv1::PageFailureCode_Name(failure.code());
  if (code.empty()) code = "page failure " + std::to_string(static_cast<int>(failure.code()));
  std::string text = page_label(page_no) + ": " + code;
  if (!failure.message().empty()) text += ": " + failure.message();
  return text;
}

// A page whose generation stopped short of the end token is kept, with the
// reason beside it.
std::optional<std::string> finish_reason_text(int page_no, lightonv1::FinishReason reason) {
  if (reason == lightonv1::FINISH_REASON_UNSPECIFIED ||
      reason == lightonv1::FINISH_REASON_END_TOKEN) {
    return std::nullopt;
  }
  std::string name = lightonv1::FinishReason_Name(reason);
  if (name.empty()) name = "finish reason " + std::to_string(static_cast<int>(reason));
  return page_label(page_no) + ": " + name;
}

}  // namespace

VlmBodyEngine choose_vlm_body_engine(const VlmConvertOptions& resolved,
                                     bool lighton_configured) {
  return lighton_configured && resolved.preset == vlmv1::VLM_PRESET_LIGHTON_OCR
             ? VlmBodyEngine::kLightonOcr
             : VlmBodyEngine::kVlmConvert;
}

VlmConvertReport convert_lighton_pages(const std::shared_ptr<grpc::Channel>& channel,
                                       const LightonOcrOptions& options,
                                       const VlmConvertOptions& vlm,
                                       std::shared_ptr<const std::string> bytes, bool pdf,
                                       docv1::Document* document,
                                       CollectorDeadline inbound_deadline,
                                       const std::function<bool()>& cancelled) {
  VlmConvertReport report;
  report.engine = VlmBodyEngine::kLightonOcr;
  const auto fail = [&report](grpc::StatusCode code, std::string error) {
    report.code = code;
    report.error = std::move(error);
    return report;
  };
  const auto gone = [&cancelled] { return cancelled && cancelled(); };
  const auto expired = [inbound_deadline] {
    return std::chrono::system_clock::now() >= inbound_deadline;
  };
  if (document == nullptr || bytes == nullptr) {
    return fail(grpc::StatusCode::INVALID_ARGUMENT, "lighton ocr: missing document or bytes");
  }
  if (channel == nullptr || !options.enabled()) {
    return fail(grpc::StatusCode::FAILED_PRECONDITION,
                "lighton ocr: grpc-lighton-ocr is not configured (GRPARSE_LIGHTON_TARGET)");
  }

  const double dpi = vlm.render_dpi > 0.0 ? vlm.render_dpi : kDefaultRenderDpi;
  std::shared_ptr<PageSource> source;
  try {
    source = open_in_memory_document(std::move(bytes), pdf, dpi, SourceOpening::kOnFirstUse);
  } catch (const PdfBackendNotConfigured& ex) {
    return fail(grpc::StatusCode::FAILED_PRECONDITION, std::string("lighton ocr: ") + ex.what());
  } catch (const std::exception& ex) {
    return fail(grpc::StatusCode::INVALID_ARGUMENT,
                std::string("lighton ocr: could not open document: ") + ex.what());
  }
  if (!source) return fail(grpc::StatusCode::INVALID_ARGUMENT, "lighton ocr: document has no pages");
  // The source's backend calls end with the request, the opening Probe
  // included, as on the vlm-convert leg.
  const PageSourceWatch source_watch(source, inbound_deadline,
                                     [&gone, &expired] { return gone() || expired(); });

  int page_count = 0;
  try {
    page_count = source->page_count();
  } catch (const std::exception& ex) {
    if (gone()) return fail(grpc::StatusCode::CANCELLED, "lighton ocr: request cancelled");
    if (expired()) {
      return fail(grpc::StatusCode::DEADLINE_EXCEEDED,
                  "lighton ocr: deadline exceeded while opening the document");
    }
    return fail(grpc::StatusCode::INVALID_ARGUMENT,
                std::string("lighton ocr: could not open document: ") + ex.what());
  }
  if (page_count <= 0) {
    return fail(grpc::StatusCode::INVALID_ARGUMENT, "lighton ocr: document has no pages");
  }
  int first = 1;
  int last = page_count;
  if (vlm.page_range.has_value()) {
    first = vlm.page_range->first;
    last = vlm.page_range->second;
    if (first < 1 || last < first) {
      return fail(grpc::StatusCode::INVALID_ARGUMENT,
                  "lighton ocr: page_range must be a 1-indexed inclusive span");
    }
    if (first > page_count) {
      return fail(grpc::StatusCode::INVALID_ARGUMENT,
                  "lighton ocr: page_range start is past the end of the document");
    }
    if (last > page_count) last = page_count;
  }
  // The wire numbers the pages it is sent 1..pages; document page = wire
  // page + offset.
  const int pages = last - first + 1;
  const int32_t offset = first - 1;
  if (gone()) return fail(grpc::StatusCode::CANCELLED, "lighton ocr: request cancelled");
  if (expired()) {
    return fail(grpc::StatusCode::DEADLINE_EXCEEDED,
                "lighton ocr: deadline exceeded before the first page");
  }

  auto stub = lightonv1::LightOnOcrService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, options.timeout));
  context.set_wait_for_ready(false);
  auto stream = stub->ParseDocument(&context);
  const CancelWatch watch(context, cancelled);

  // Written by the upload thread, read once it is joined.
  std::atomic<int> pages_sent{0};
  std::optional<std::string> upload_error;
  lightonv1::ParseDocumentRequest frame;
  ConcurrentUpload upload(context, [&] {
    lightonv1::ParseOptions* parse_options = frame.mutable_options();
    parse_options->set_model(options.model);
    parse_options->set_page_count(static_cast<uint32_t>(pages));
    parse_options->set_emit_page_documents(true);
    parse_options->set_emit_document(false);
    if (!stream->Write(frame)) return;
    for (int wire_page = 1; wire_page <= pages; ++wire_page) {
      const int page_no = wire_page + offset;
      // A page the peer is not sent leaves the document short of its
      // page_count, so a page that cannot be rendered ends the call.
      const auto stop = [&](std::string why) {
        upload_error = page_label(page_no) + ": " + std::move(why);
        context.TryCancel();
      };
      cv::Mat raster;
      try {
        raster = source->render_page(page_no);
      } catch (const std::exception& ex) {
        stop(std::string("render failed: ") + ex.what());
        return;
      }
      if (raster.empty()) {
        stop("rendered empty");
        return;
      }
      std::vector<unsigned char> png;
      if (!cv::imencode(".png", raster, png, kPngEncodeParams)) {
        stop("PNG encode failed");
        return;
      }
      frame.Clear();
      lightonv1::PageImage* image = frame.mutable_page();
      image->set_page_no(static_cast<uint32_t>(wire_page));
      image->set_png(reinterpret_cast<const char*>(png.data()), png.size());
      image->set_width_px(static_cast<uint32_t>(raster.cols));
      image->set_height_px(static_cast<uint32_t>(raster.rows));
      if (!stream->Write(frame)) return;
      pages_sent.fetch_add(1);
    }
    stream->WritesDone();
  });

  // Keyed by wire page, so the merge runs in page order whatever order the
  // peer finished the pages in.
  std::map<uint32_t, docv1::Document> fragments;
  std::map<uint32_t, lightonv1::PageFailure> failures;
  std::optional<std::string> protocol_error;
  std::optional<lightonv1::ParseComplete> complete;
  const auto refuse = [&](std::string why) {
    if (!protocol_error.has_value()) protocol_error = "lighton ocr: " + std::move(why);
    context.TryCancel();
  };
  const auto claim_page = [&](uint32_t wire_page) {
    if (wire_page < 1 || wire_page > static_cast<uint32_t>(pages)) {
      refuse("page " + std::to_string(wire_page) + " is outside the " +
             std::to_string(pages) + " pages sent");
      return false;
    }
    if (fragments.contains(wire_page) || failures.contains(wire_page)) {
      refuse("page " + std::to_string(wire_page) + " was answered twice");
      return false;
    }
    return true;
  };
  lightonv1::ParseDocumentResponse event;
  while (stream->Read(&event)) {
    if (event.has_page_document()) {
      lightonv1::PageDocument* page = event.mutable_page_document();
      if (!claim_page(page->page_no())) break;
      const int page_no = static_cast<int>(page->page_no()) + offset;
      if (auto reason = finish_reason_text(page_no, page->finish_reason())) {
        report.warnings.push_back(std::move(*reason));
      }
      fragments.emplace(page->page_no(), std::move(*page->mutable_document()));
    } else if (event.has_page_failure()) {
      lightonv1::PageFailure* failure = event.mutable_page_failure();
      if (!claim_page(failure->page_no())) break;
      const int page_no = static_cast<int>(failure->page_no()) + offset;
      report.warnings.push_back(page_failure_text(page_no, *failure));
      if (vlm.abort_on_error) {
        refuse(report.warnings.back().substr(std::string("lighton ocr: ").size()));
        break;
      }
      failures.emplace(failure->page_no(), std::move(*failure));
    } else if (event.has_complete()) {
      complete = event.complete();
    }
    event.Clear();
  }
  upload.join();
  const grpc::Status status = stream->Finish();
  report.pages_sent = pages_sent.load();
  report.pages_ok = static_cast<int>(fragments.size());
  report.pages_failed = static_cast<int>(failures.size());

  if (gone()) return fail(grpc::StatusCode::CANCELLED, "lighton ocr: request cancelled");
  if (upload_error.has_value()) {
    if (expired()) {
      return fail(grpc::StatusCode::DEADLINE_EXCEEDED,
                  "lighton ocr: deadline exceeded while rendering: " + *upload_error);
    }
    return fail(grpc::StatusCode::INTERNAL, std::move(*upload_error));
  }
  if (protocol_error.has_value()) {
    return fail(grpc::StatusCode::INTERNAL, std::move(*protocol_error));
  }
  if (!status.ok()) {
    return fail(status.error_code(),
                "lighton ocr: ParseDocument failed: " +
                    (status.error_message().empty()
                         ? std::string("status ") +
                               std::to_string(static_cast<int>(status.error_code()))
                         : status.error_message()));
  }
  if (!complete.has_value()) {
    return fail(grpc::StatusCode::INTERNAL,
                "lighton ocr: ParseDocument ended without its ParseComplete trailer");
  }
  if (fragments.empty()) {
    return fail(grpc::StatusCode::INTERNAL, report.warnings.empty()
                                                ? "lighton ocr: peer returned no page documents"
                                                : report.warnings.front());
  }
  docv1::CollectorSource claimant;
  claimant.set_collector(kLightonCollectorName);
  for (uint32_t wire_page = 1; wire_page <= static_cast<uint32_t>(pages); ++wire_page) {
    if (auto found = fragments.find(wire_page); found != fragments.end()) {
      docv1::Document fragment = std::move(found->second);
      shift_page_numbers(&fragment, offset);
      type_generation_stop_reasons(&fragment);
      merge_documents(std::move(fragment), document, claimant);
    } else if (auto failed = failures.find(wire_page); failed != failures.end()) {
      // What the engine could ground before it stopped is kept beside the
      // failure warning, the way a fragment cut to a cap is.
      if (failed->second.has_partial_document()) {
        docv1::Document partial = std::move(*failed->second.mutable_partial_document());
        shift_page_numbers(&partial, offset);
        type_generation_stop_reasons(&partial);
        merge_documents(std::move(partial), document, claimant);
      }
    } else {
      report.warnings.push_back(page_label(static_cast<int>(wire_page) + offset) +
                                ": not answered");
    }
  }
  report.success = true;
  report.code = grpc::StatusCode::OK;
  return report;
}

}  // namespace grparse
