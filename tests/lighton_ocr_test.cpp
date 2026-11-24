// The LightOnOCR leg against fake grpc-lighton-ocr servers: the page stream
// and its merge (pages answered out of order, a span past page 1, a page
// failure), the leg's failures (a peer error, a missing trailer, a page
// answered twice, a cancelled caller), and the VLM pipeline's choice
// between it and grpc-vlm-convert through a running DocumentParserService.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/lightonocr/v1/lighton_ocr.grpc.pb.h"
#include "ai/pipestream/vlm/v1/vlm_convert.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/lighton_ocr.h"
#include "grparse/page_previews.h"
#include "grparse/vlm_convert.h"
#include "support/check.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace lightonv1 = ai::pipestream::lightonocr::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;
using namespace std::chrono_literals;

// What the fake does once it has read the whole upload.
enum class Answer {
  // Every page, last page first, then the trailer.
  kReversed,
  // Page 2 fails with a partial document; the rest convert.
  kSecondPageFails,
  // FAILED_PRECONDITION, the way a checkpoint mismatch answers.
  kError,
  // Every page, but no trailer.
  kNoTrailer,
  // Page 1 twice.
  kDuplicate,
  // Nothing until the call is cancelled.
  kStall,
};

docv1::Document page_fragment(uint32_t page_no, const std::string& text) {
  docv1::Document fragment;
  auto* base = fragment.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->set_text(text);
  auto* prov = base->add_prov();
  prov->set_page_no(static_cast<int32_t>(page_no));
  auto* generation = base->add_source()->mutable_generation();
  generation->set_model("lightonai/LightOnOCR-3-0.8B");
  generation->set_finish_reason("stop");
  (*fragment.mutable_pages())[static_cast<int32_t>(page_no)].set_page_no(
      static_cast<int32_t>(page_no));
  return fragment;
}

class FakeLightonService final : public lightonv1::LightOnOcrService::Service {
 public:
  explicit FakeLightonService(Answer answer) : answer_(answer) {}

  grpc::Status ParseDocument(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<lightonv1::ParseDocumentResponse,
                               lightonv1::ParseDocumentRequest>* stream) override {
    ++calls_;
    Seen seen;
    lightonv1::ParseDocumentRequest request;
    bool first = true;
    while (stream->Read(&request)) {
      if (first) seen.options_first = request.has_options();
      first = false;
      if (request.has_options()) seen.options = request.options();
      if (request.has_page()) {
        seen.page_nos.push_back(request.page().page_no());
        seen.png_ok.push_back(!request.page().png().empty() && request.page().width_px() > 0 &&
                              request.page().height_px() > 0);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_ = seen;
    }
    if (answer_ == Answer::kError) {
      return {grpc::StatusCode::FAILED_PRECONDITION, "this process serves LIGHTON_MODEL_0_8B"};
    }
    if (answer_ == Answer::kStall) {
      while (!context->IsCancelled()) std::this_thread::sleep_for(10ms);
      return grpc::Status::CANCELLED;
    }
    std::vector<uint32_t> order(seen.page_nos.rbegin(), seen.page_nos.rend());
    if (answer_ == Answer::kDuplicate) order = {1, 1};
    uint32_t ok = 0;
    uint32_t failed = 0;
    for (const uint32_t page_no : order) {
      lightonv1::ParseDocumentResponse event;
      if (answer_ == Answer::kSecondPageFails && page_no == 2) {
        auto* failure = event.mutable_page_failure();
        failure->set_page_no(page_no);
        failure->set_code(lightonv1::PAGE_FAILURE_CODE_RESOURCE_LIMIT);
        failure->set_message("decode cap reached");
        failure->set_finish_reason(lightonv1::FINISH_REASON_TOKEN_LIMIT);
        *failure->mutable_partial_document() = page_fragment(page_no, "partial-2");
        ++failed;
      } else {
        auto* page = event.mutable_page_document();
        page->set_page_no(page_no);
        page->set_finish_reason(page_no == 1 ? lightonv1::FINISH_REASON_TOKEN_LIMIT
                                             : lightonv1::FINISH_REASON_END_TOKEN);
        *page->mutable_document() = page_fragment(page_no, "wire-" + std::to_string(page_no));
        ++ok;
      }
      stream->Write(event);
    }
    if (answer_ != Answer::kNoTrailer) {
      lightonv1::ParseDocumentResponse done;
      done.mutable_complete()->set_pages(static_cast<uint32_t>(seen.page_nos.size()));
      done.mutable_complete()->set_pages_ok(ok);
      done.mutable_complete()->set_pages_failed(failed);
      stream->Write(done);
    }
    return grpc::Status::OK;
  }

  struct Seen {
    bool options_first = false;
    lightonv1::ParseOptions options;
    std::vector<uint32_t> page_nos;
    std::vector<bool> png_ok;
  };
  Seen seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }
  int calls() const { return calls_.load(); }

 private:
  Answer answer_;
  mutable std::mutex mutex_;
  Seen seen_;
  std::atomic<int> calls_{0};
};

// grpc-vlm-convert answering every page with one text item, so a routing
// test can tell which peer produced the body.
class FakeVlmConvertService final : public vlmv1::VlmConvertService::Service {
 public:
  grpc::Status ConvertPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<vlmv1::ConvertPagesResponse, vlmv1::ConvertPagesRequest>*
          stream) override {
    ++calls_;
    vlmv1::ConvertPagesRequest request;
    std::vector<uint32_t> pages;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        std::lock_guard<std::mutex> lock(mutex_);
        preset_ = request.options().preset();
      }
      if (request.has_page_image()) pages.push_back(request.page_image().page_no());
    }
    for (const uint32_t page_no : pages) {
      vlmv1::ConvertPagesResponse event;
      auto* page = event.mutable_page_document();
      page->set_page_no(page_no);
      *page->mutable_document() = page_fragment(page_no, "vlm-convert page");
      stream->Write(event);
    }
    vlmv1::ConvertPagesResponse done;
    done.mutable_complete()->set_pages_ok(static_cast<uint32_t>(pages.size()));
    stream->Write(done);
    return grpc::Status::OK;
  }
  int calls() const { return calls_.load(); }
  vlmv1::VlmPreset preset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return preset_;
  }

 private:
  mutable std::mutex mutex_;
  vlmv1::VlmPreset preset_ = vlmv1::VLM_PRESET_UNSPECIFIED;
  std::atomic<int> calls_{0};
};

class ServerFixture {
 public:
  explicit ServerFixture(grpc::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) throw std::runtime_error("fake server failed to start");
  }
  ~ServerFixture() {
    if (server_ != nullptr) server_->Shutdown(std::chrono::system_clock::now() + 2s);
  }
  std::string target() const { return "127.0.0.1:" + std::to_string(port_); }
  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel(target(), grpc::InsecureChannelCredentials());
  }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

// A multi-page TIFF the leg rasterizes in process, one page per frame.
std::shared_ptr<const std::string> pages_tiff(int count) {
  std::vector<cv::Mat> pages;
  for (int index = 0; index < count; ++index) {
    pages.emplace_back(24, 16, CV_8UC3, cv::Scalar(10 * index, 20, 30));
  }
  std::vector<unsigned char> tiff;
  require(cv::imencodemulti(".tiff", pages, tiff), "encode the multi-page fixture");
  return std::make_shared<const std::string>(reinterpret_cast<const char*>(tiff.data()),
                                             tiff.size());
}

std::shared_ptr<const std::string> one_page_png() {
  cv::Mat image(16, 12, CV_8UC3, cv::Scalar(1, 2, 3));
  std::vector<unsigned char> png;
  require(cv::imencode(".png", image, png, grparse::kPngEncodeParams), "encode fixture");
  return std::make_shared<const std::string>(reinterpret_cast<const char*>(png.data()),
                                             png.size());
}

grparse::LightonOcrOptions lighton_options(const ServerFixture& server) {
  grparse::LightonOcrOptions options;
  options.target = server.target();
  options.timeout = 10s;
  return options;
}

docv1::Document empty_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  return document;
}

std::vector<std::string> texts_of(const docv1::Document& document) {
  std::vector<std::string> texts;
  for (const auto& item : document.texts()) texts.push_back(item.text().base().text());
  return texts;
}

// Pages that come back last-first still merge in page order, each page's
// items keep their own page number, and a stop short of the end token is a
// warning beside a kept page.
void verify_out_of_order_pages_merge_in_page_order() {
  FakeLightonService fake(Answer::kReversed);
  ServerFixture server(&fake);
  docv1::Document document = empty_document();
  const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
      server.channel(), lighton_options(server), {}, pages_tiff(3), /*pdf=*/false, &document);
  require(report.success && report.engine == grparse::VlmBodyEngine::kLightonOcr,
          "three pages convert: " + report.error);
  require(report.pages_sent == 3 && report.pages_ok == 3 && report.pages_failed == 0,
          "every page is sent and converted");
  require(texts_of(document) == std::vector<std::string>{"wire-1", "wire-2", "wire-3"},
          "the fragments merge in page order, not answer order");
  for (int index = 0; index < 3; ++index) {
    require(document.texts(index).text().base().prov(0).page_no() == index + 1,
            "a page's item keeps its page number");
  }
  require(document.pages().size() == 3 && document.pages().contains(3),
          "every page has its entry");
  require(document.texts(0).text().base().source(0).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_END_OF_SEQUENCE,
          "the merged generation carries a typed stop reason");
  require(report.warnings == std::vector<grparse::Warning>{
                                 "lighton ocr: page 1: FINISH_REASON_TOKEN_LIMIT"},
          "the page cut at its token cap is reported by reason name");
  const auto seen = fake.seen();
  require(seen.options_first && seen.options.model() == lightonv1::LIGHTON_MODEL_0_8B &&
              seen.options.page_count() == 3 && seen.options.emit_page_documents() &&
              !seen.options.emit_document(),
          "ParseOptions leads with the checkpoint, the page count and page documents on");
  require(seen.page_nos == std::vector<uint32_t>{1, 2, 3} &&
              seen.png_ok == std::vector<bool>{true, true, true},
          "one sized PNG per page, in order");
}

// A span that starts past page 1 goes out numbered 1..n, as the wire
// requires, and lands back on the document's own page numbers.
void verify_page_range_maps_back_to_document_pages() {
  FakeLightonService fake(Answer::kReversed);
  ServerFixture server(&fake);
  grparse::VlmConvertOptions vlm;
  vlm.page_range = std::make_pair(2, 3);
  docv1::Document document = empty_document();
  const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
      server.channel(), lighton_options(server), vlm, pages_tiff(4), /*pdf=*/false, &document);
  require(report.success && report.pages_sent == 2, "the span converts: " + report.error);
  const auto seen = fake.seen();
  require(seen.options.page_count() == 2 && seen.page_nos == std::vector<uint32_t>{1, 2},
          "the wire sees two pages numbered from 1");
  require(document.texts_size() == 2 &&
              document.texts(0).text().base().prov(0).page_no() == 2 &&
              document.texts(1).text().base().prov(0).page_no() == 3,
          "the items land on document pages 2 and 3");
  require(document.pages().size() == 2 && document.pages().contains(2) &&
              document.pages().at(3).page_no() == 3,
          "the page entries are re-keyed to the document's pages");
  require(report.warnings.size() == 1 &&
              report.warnings[0] == "lighton ocr: page 2: FINISH_REASON_TOKEN_LIMIT",
          "the warning names the document page, not the wire page");
}

// A page the engine could not finish is a warning; what it grounded before
// it stopped is kept. With abort_on_error the same failure fails the leg.
void verify_page_failure_is_a_warning_unless_aborting() {
  {
    FakeLightonService fake(Answer::kSecondPageFails);
    ServerFixture server(&fake);
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), {}, pages_tiff(3), /*pdf=*/false, &document);
    require(report.success && report.pages_ok == 2 && report.pages_failed == 1,
            "two pages convert beside one failure: " + report.error);
    require(texts_of(document) == std::vector<std::string>{"wire-1", "partial-2", "wire-3"},
            "the failed page's partial items keep their place");
    bool reported = false;
    for (const auto& warning : report.warnings) {
      reported = reported || warning == "lighton ocr: page 2: PAGE_FAILURE_CODE_RESOURCE_LIMIT: "
                                        "decode cap reached";
    }
    require(reported, "the failure is reported by code name");
  }
  {
    FakeLightonService fake(Answer::kSecondPageFails);
    ServerFixture server(&fake);
    grparse::VlmConvertOptions vlm;
    vlm.abort_on_error = true;
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), vlm, pages_tiff(3), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::INTERNAL &&
                report.error.contains("PAGE_FAILURE_CODE_RESOURCE_LIMIT"),
            "abort_on_error fails the leg on the page failure: " + report.error);
    require(document.texts_size() == 0, "an aborted leg merges nothing");
  }
}

// The peer's own status ends the leg with its class and message; a stream
// with no trailer or with a page answered twice is not a document.
void verify_server_errors_fail_the_leg() {
  {
    FakeLightonService fake(Answer::kError);
    ServerFixture server(&fake);
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), {}, pages_tiff(2), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::FAILED_PRECONDITION &&
                report.error.contains("serves LIGHTON_MODEL_0_8B"),
            "the peer's status class and message survive: " + report.error);
  }
  {
    FakeLightonService fake(Answer::kNoTrailer);
    ServerFixture server(&fake);
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), {}, pages_tiff(2), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::INTERNAL &&
                report.error.contains("ParseComplete"),
            "a stream with no trailer fails: " + report.error);
    require(document.texts_size() == 0, "nothing merges from an unfinished stream");
  }
  {
    FakeLightonService fake(Answer::kDuplicate);
    ServerFixture server(&fake);
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), {}, pages_tiff(2), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::INTERNAL &&
                report.error.contains("answered twice"),
            "a page answered twice fails: " + report.error);
  }
  {
    // Nothing listens: the leg fails UNAVAILABLE rather than hanging.
    grparse::LightonOcrOptions options;
    options.target = "127.0.0.1:1";
    options.timeout = 5s;
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        grpc::CreateChannel(options.target, grpc::InsecureChannelCredentials()), options, {},
        one_page_png(), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::UNAVAILABLE,
            "an unreachable peer is UNAVAILABLE: " + report.error);
  }
  {
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        nullptr, grparse::LightonOcrOptions{}, {}, one_page_png(), /*pdf=*/false, &document);
    require(!report.success && report.code == grpc::StatusCode::FAILED_PRECONDITION &&
                report.error.contains("GRPARSE_LIGHTON_TARGET"),
            "an unconfigured leg names the variable");
  }
}

// A caller already gone sends nothing; one that goes while the peer works
// cancels the stream instead of waiting out the peer.
void verify_cancel_ends_the_leg() {
  {
    FakeLightonService fake(Answer::kReversed);
    ServerFixture server(&fake);
    docv1::Document document = empty_document();
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), lighton_options(server), {}, one_page_png(), /*pdf=*/false, &document,
        grparse::kNoCollectorDeadline, [] { return true; });
    require(!report.success && report.code == grpc::StatusCode::CANCELLED &&
                report.pages_sent == 0,
            "a cancelled request sends nothing: " + report.error);
    require(fake.calls() == 0, "a cancelled request never dials the peer");
  }
  {
    FakeLightonService fake(Answer::kStall);
    ServerFixture server(&fake);
    grparse::LightonOcrOptions options = lighton_options(server);
    options.timeout = 30s;
    docv1::Document document = empty_document();
    const auto started = std::chrono::steady_clock::now();
    const auto gone_at = started + 300ms;
    const grparse::VlmConvertReport report = grparse::convert_lighton_pages(
        server.channel(), options, {}, pages_tiff(2), /*pdf=*/false, &document,
        grparse::kNoCollectorDeadline,
        [gone_at] { return std::chrono::steady_clock::now() >= gone_at; });
    require(std::chrono::steady_clock::now() - started < 5s, "the leg does not wait out the peer");
    require(!report.success && report.code == grpc::StatusCode::CANCELLED,
            "the leg ends cancelled: " + report.error);
  }
}

void verify_engine_choice() {
  grparse::VlmConvertOptions lighton;
  lighton.preset = vlmv1::VLM_PRESET_LIGHTON_OCR;
  grparse::VlmConvertOptions granite;
  granite.preset = vlmv1::VLM_PRESET_GRANITE_DOCLING;
  require(grparse::choose_vlm_body_engine(lighton, true) == grparse::VlmBodyEngine::kLightonOcr,
          "the LightOnOCR model with the leg configured goes to grpc-lighton-ocr");
  require(grparse::choose_vlm_body_engine(lighton, false) == grparse::VlmBodyEngine::kVlmConvert,
          "without the leg the LightOnOCR model keeps the vlm-convert preset");
  require(grparse::choose_vlm_body_engine(granite, true) == grparse::VlmBodyEngine::kVlmConvert,
          "every other model stays on grpc-vlm-convert");
  grparse::CollectorTargets targets;
  grparse::CollectorEndpoints none(targets);
  require(!none.has_lighton() && none.lighton_channel() == nullptr, "default off");
  targets.lighton.target = "lighton-ocr:50074";
  grparse::CollectorEndpoints wired(targets);
  require(wired.has_lighton() && wired.lighton_channel() != nullptr, "a target makes a channel");
}

class NoRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    throw std::runtime_error("the VLM pipeline ran the CV path");
  }
};

struct RoutedConvert {
  grpc::Status status;
  parsev1::ConvertSourceResponse response;
};

// One ConvertSource with PROCESSING_PIPELINE_VLM and `model` through a
// parser wired to the given peers (an empty target leaves a peer unset).
RoutedConvert convert_through_parser(const std::string& vlm_target,
                                     const std::string& lighton_target,
                                     parsev1::VlmModelType model) {
  NoRecognizer recognizer;
  grparse::PageScheduler scheduler(recognizer, {2, 3, 2, 3, 2, 2, 2});
  grparse::CollectorTargets targets;
  targets.vlm.target = vlm_target;
  targets.vlm.timeout = 10s;
  targets.lighton.target = lighton_target;
  targets.lighton.timeout = 10s;
  grparse::DocumentParserService parser(scheduler,
                                        std::make_shared<grparse::CollectorEndpoints>(targets));
  ServerFixture server(&parser);
  auto client = parsev1::ParseService::NewStub(server.channel());
  parsev1::ConvertSourceRequest request;
  auto* file = request.mutable_request()->add_sources()->mutable_file();
  file->set_filename("page.png");
  // A 1x1 PNG: the leg renders the image itself.
  file->set_base64_string(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");
  auto* options = request.mutable_request()->mutable_options();
  options->set_pipeline(parsev1::PROCESSING_PIPELINE_VLM);
  options->set_vlm_pipeline_model(model);
  RoutedConvert result;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 20s);
  result.status = client->ConvertSource(&context, request, &result.response);
  return result;
}

// Through a running parser: the LightOnOCR model goes to grpc-lighton-ocr
// when its target is set and to the vlm-convert preset when it is not;
// other models never reach grpc-lighton-ocr; and a lighton leg that fails
// fails the request, with grpc-vlm-convert never tried.
void verify_parser_routes_the_lighton_model() {
  FakeVlmConvertService vlm;
  ServerFixture vlm_server(&vlm);
  {
    FakeLightonService lighton(Answer::kReversed);
    ServerFixture lighton_server(&lighton);
    const RoutedConvert routed = convert_through_parser(
        vlm_server.target(), lighton_server.target(), parsev1::VLM_MODEL_TYPE_LIGHTONOCR);
    require(routed.status.ok(), "the lighton leg converts: " + routed.status.error_message());
    const auto& document = routed.response.response().document().doc();
    require(texts_of(document) == std::vector<std::string>{"wire-1"},
            "the body is grpc-lighton-ocr's");
    bool warned = false;
    for (const auto& warning : document.warnings()) {
      warned = warned || (warning.source().collector() == "lighton-ocr" &&
                          warning.message() == "lighton ocr: page 1: FINISH_REASON_TOKEN_LIMIT");
    }
    require(warned, "the leg's warning names lighton-ocr as its source");
    require(lighton.calls() == 1 && vlm.calls() == 0, "grpc-vlm-convert is not dialed");
  }
  {
    const RoutedConvert routed =
        convert_through_parser(vlm_server.target(), "", parsev1::VLM_MODEL_TYPE_LIGHTONOCR);
    require(routed.status.ok(), "the preset converts: " + routed.status.error_message());
    require(texts_of(routed.response.response().document().doc()) ==
                    std::vector<std::string>{"vlm-convert page"} &&
                vlm.calls() == 1 && vlm.preset() == vlmv1::VLM_PRESET_LIGHTON_OCR,
            "without GRPARSE_LIGHTON_TARGET the model is grpc-vlm-convert's LightOnOCR preset");
  }
  {
    FakeLightonService lighton(Answer::kReversed);
    ServerFixture lighton_server(&lighton);
    const RoutedConvert routed = convert_through_parser(
        vlm_server.target(), lighton_server.target(), parsev1::VLM_MODEL_TYPE_GRANITEDOCLING);
    require(routed.status.ok() && vlm.calls() == 2 && lighton.calls() == 0,
            "another model stays on grpc-vlm-convert: " + routed.status.error_message());
  }
  {
    FakeLightonService lighton(Answer::kError);
    ServerFixture lighton_server(&lighton);
    const RoutedConvert routed = convert_through_parser(
        vlm_server.target(), lighton_server.target(), parsev1::VLM_MODEL_TYPE_LIGHTONOCR);
    require(routed.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
                routed.status.error_message().contains("lighton ocr"),
            "the failed leg fails the request: " + routed.status.error_message());
    require(vlm.calls() == 2, "no fallback to grpc-vlm-convert");
  }
  {
    // grpc-vlm-convert unset does not stop a LightOnOCR request.
    FakeLightonService lighton(Answer::kReversed);
    ServerFixture lighton_server(&lighton);
    const RoutedConvert routed =
        convert_through_parser("", lighton_server.target(), parsev1::VLM_MODEL_TYPE_LIGHTONOCR);
    require(routed.status.ok() && lighton.calls() == 1,
            "lighton alone serves its model: " + routed.status.error_message());
    const RoutedConvert other =
        convert_through_parser("", lighton_server.target(), parsev1::VLM_MODEL_TYPE_GRANITEDOCLING);
    require(other.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
                other.status.error_message().contains("GRPARSE_VLM_CONVERT_TARGET"),
            "another model still needs grpc-vlm-convert: " + other.status.error_message());
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("lighton-ocr-test", "all checks passed", {
      verify_out_of_order_pages_merge_in_page_order,
      verify_page_range_maps_back_to_document_pages,
      verify_page_failure_is_a_warning_unless_aborting,
      verify_server_errors_fail_the_leg,
      verify_cancel_ends_the_leg,
      verify_engine_choice,
      verify_parser_routes_the_lighton_model,
  });
}
