#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "grparse/base64.h"
#include "grparse/document_assembly.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"

namespace {

using namespace std::chrono_literals;
namespace docv1 = ai::pipestream::document::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;
namespace pipestream = ai::pipestream;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSource final : public grparse::PageSource {
 public:
  explicit FakeSource(bool digital = false) : digital_(digital) {}
  int page_count() const override { return 3; }
  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    if (!digital_) return std::nullopt;
    static const std::vector<std::string> text{"", "native-one", "native-two", "native-three"};
    grparse::OcrPage page{100, 200,
                          {{text.at(page_number), {{1, 2}, {20, 2}, {20, 12}, {1, 12}}, std::nullopt,
                            grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = true;
    return page;
  }
  cv::Mat render_page(int page_number) const override {
    if (digital_) throw std::runtime_error("digital unary page was rasterized");
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }

 private:
  bool digital_;
};

class FakeRecognizer final : public grparse::PageRecognizer {
 public:
  explicit FakeRecognizer(std::chrono::milliseconds delay = 0ms) : delay_(delay) {}

  grparse::OcrPage extract_page(const cv::Mat& image) override {
    calls.fetch_add(1);
    const int page = image.at<unsigned char>(0, 0);
    std::this_thread::sleep_for(delay_);
    if (page == 1) std::this_thread::sleep_for(30ms);
    static const std::vector<std::string> text{"", "one", "two", "three"};
    return {100, 200, {{text.at(page), {{1, 2}, {20, 2}, {20, 12}, {1, 12}}}}};
  }

  std::atomic<int> calls{0};

 private:
  std::chrono::milliseconds delay_;
};

class TestServer final {
 public:
  explicit TestServer(std::chrono::milliseconds inference_delay = 0ms, bool digital = false)
      : recognizer_(inference_delay),
        scheduler_(recognizer_, {2, 3, 2, 3, 2, 2, 2},
                   [this, digital](std::shared_ptr<const std::string>, bool, double render_dpi) {
                     last_render_dpi_.store(render_dpi);
                     return std::make_shared<FakeSource>(digital);
                   }),
        parser_service_(scheduler_, std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{})),
        streaming_service_(scheduler_, std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{})) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&parser_service_);
    builder.RegisterService(&streaming_service_);
    server_ = builder.BuildAndStart();
    if (!server_ || port_ == 0) throw std::runtime_error("test server failed to start");
  }

  ~TestServer() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
  }

  std::unique_ptr<pipestream::parse::v1::ParseStreamingService::Stub> stub() const {
    return pipestream::parse::v1::ParseStreamingService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials()));
  }

  std::unique_ptr<pipestream::parse::v1::ParseService::Stub> unary_stub() const {
    return pipestream::parse::v1::ParseService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials()));
  }

  grparse::PageScheduler::Metrics metrics() const { return scheduler_.metrics(); }
  int recognizer_calls() const { return recognizer_.calls.load(); }
  double last_render_dpi() const { return last_render_dpi_.load(); }

 private:
  std::atomic<double> last_render_dpi_{0.0};
  FakeRecognizer recognizer_;
  grparse::PageScheduler scheduler_;
  grparse::DocumentParserService parser_service_;
  grparse::DocumentStreamingService streaming_service_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};

// Holds page one back so the pages behind it pile up undelivered, which is what
// exercises the reactor's buffer bound rather than its steady-state path.
class HeadOfLineRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat& image) override {
    const int page = image.at<unsigned char>(0, 0);
    if (page == 1) std::this_thread::sleep_for(400ms);
    return {100, 200, {{"page-" + std::to_string(page), {{1, 2}, {20, 2}, {20, 12}, {1, 12}}}}};
  }
};

class WideSource final : public grparse::PageSource {
 public:
  explicit WideSource(int pages) : pages_(pages) {}
  int page_count() const override { return pages_; }
  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }

 private:
  int pages_;
};

// The reactor's stream buffer bound must track the configured page window:
// with a wide window, a slow head-of-line page piling up the pages behind it
// is well-behaved backpressure the stream absorbs, never a
// RESOURCE_EXHAUSTED offense.
void verify_wide_page_window_streams_completely() {
  constexpr int kPages = 8;
  grparse::PageScheduler::Options options{
      .document_queue_capacity = 4,
      .render_queue_capacity = 16,
      .inference_queue_capacity = 16,
      .assembly_queue_capacity = 16,
      .render_workers = 4,
      .inference_workers = 4,
      .assembly_workers = 2,
      .page_window = kPages,
  };

  HeadOfLineRecognizer recognizer;
  grparse::PageScheduler scheduler(recognizer, options,
                                   [pages = kPages](std::shared_ptr<const std::string>, bool, double) {
                                     return std::make_shared<WideSource>(pages);
                                   });
  grparse::DocumentStreamingService streaming_service(
      scheduler, std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{}));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&streaming_service);
  auto server = builder.BuildAndStart();
  require(server && port != 0, "wide-window test server failed to start");

  auto client = pipestream::parse::v1::ParseStreamingService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 20s);
  auto stream = client->StreamProcessDocument(&context);
  pipestream::parse::v1::DocumentChunk source;
  source.set_document_id("wide-window");
  source.set_filename("image.png");
  source.set_content_type("image/png");
  source.set_data("in-memory-source");
  source.set_complete(true);
  require(stream->Write(source), "wide-window client could not write source chunk");
  stream->WritesDone();

  std::vector<int> page_numbers;
  pipestream::parse::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) {
    if (event.has_page()) page_numbers.push_back(event.page().page_number());
  }
  const grpc::Status status = stream->Finish();
  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();

  require(status.ok(), "wide page window stream failed: " + status.error_message());
  require(page_numbers.size() == static_cast<size_t>(kPages), "wide window page count");
  for (int index = 0; index < kPages; ++index) {
    require(page_numbers.at(index) == index + 1, "wide window pages must stay in document order");
  }
}

pipestream::parse::v1::DocumentChunk chunk(bool complete) {
  pipestream::parse::v1::DocumentChunk value;
  value.set_document_id("contract-test");
  value.set_filename("image.png");
  value.set_content_type("image/png");
  value.set_data("in-memory-source");
  value.set_complete(complete);
  return value;
}

void verify_ordered_page_stream(TestServer* server) {
  auto client = server->stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  auto stream = client->StreamProcessDocument(&context);
  require(stream->Write(chunk(true)), "client could not write source chunk");
  stream->WritesDone();

  std::vector<pipestream::parse::v1::DocumentStreamEvent> events;
  pipestream::parse::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) events.push_back(event);
  const grpc::Status status = stream->Finish();
  require(status.ok(), "stream failed: " + status.error_message());
  require(events.size() == 4, "expected three page events and one final event");

  for (int index = 0; index < 3; ++index) {
    const auto& page = events.at(index).page();
    require(events.at(index).total_pages() == 3, "total page count");
    require(page.page_number() == index + 1, "pages must emit in document order");
    require(page.texts_size() == 1 && page.text_offsets_size() == 1, "page text payload");
    require(page.texts(0).text().base().self_ref() == "#/texts/" + std::to_string(index),
            "stable stream reference");
  }
  require(events.at(0).page().text_offsets(0).utf_start() == 0 &&
              events.at(0).page().text_offsets(0).utf_end() == 3,
          "page one offsets");
  require(events.at(1).page().text_offsets(0).utf_start() == 4 &&
              events.at(1).page().text_offsets(0).utf_end() == 7,
          "page two offsets");
  require(events.at(2).page().text_offsets(0).utf_start() == 8 &&
              events.at(2).page().text_offsets(0).utf_end() == 13,
          "page three offsets");
  require(events.back().has_complete(), "terminal metadata event");
}

void verify_data_after_complete_is_rejected(TestServer* server) {
  auto client = server->stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  auto stream = client->StreamProcessDocument(&context);
  require(stream->Write(chunk(true)), "client could not write complete chunk");
  stream->Write(chunk(false));
  stream->WritesDone();
  pipestream::parse::v1::DocumentStreamEvent ignored;
  while (stream->Read(&ignored)) {
  }
  const grpc::Status status = stream->Finish();
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "data after complete should be INVALID_ARGUMENT");
}

void verify_deadline_cancels_scheduler_work() {
  TestServer server(200ms);
  auto client = server.stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 100ms);
  auto stream = client->StreamProcessDocument(&context);
  require(stream->Write(chunk(true)), "deadline client could not write source chunk");
  stream->WritesDone();
  pipestream::parse::v1::DocumentStreamEvent ignored;
  while (stream->Read(&ignored)) {
  }
  const grpc::Status status = stream->Finish();
  require(status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
              status.error_code() == grpc::StatusCode::CANCELLED,
          "deadline should cancel the stream");
  for (int attempt = 0; attempt < 100 && server.metrics().pages_cancelled == 0; ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  require(server.metrics().pages_cancelled > 0, "deadline did not cancel queued page work");
}

pipestream::parse::v1::ConvertSourceRequest unary_request() {
  pipestream::parse::v1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename("image.png");
  source->set_base64_string("bWVtb3J5");
  request.mutable_request()->mutable_options()->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_TEXT);
  return request;
}

void verify_unary_uses_scheduler_and_shared_assembly(TestServer* server) {
  auto client = server->unary_stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  pipestream::parse::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, unary_request(), &response);
  require(status.ok(), "unary conversion failed: " + status.error_message());

  const auto& result = response.response().document();
  require(response.response().status() == pipestream::parse::v1::CONVERSION_STATUS_SUCCESS,
          "unary conversion status");
  require(result.doc().pages_size() == 3 && result.doc().texts_size() == 3,
          "unary page and text counts");
  require(result.doc().texts(0).text().base().self_ref() == "#/texts/0" &&
              result.doc().texts(2).text().base().self_ref() == "#/texts/2",
          "unary stable references");
  require(result.exports().text() == "one\ntwo\nthree", "unary text export");
}

void verify_unsupported_options_are_rejected(TestServer* server) {
  auto client = server->unary_stub();
  auto request = unary_request();
  request.mutable_request()->mutable_options()->set_images_scale(2.0);
  grpc::ClientContext context;
  pipestream::parse::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, request, &response);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "unsupported conversion options must be rejected");
  require(status.error_message().contains("images_scale"),
          "the rejection must name the unimplemented option: " + status.error_message());

  request = unary_request();
  request.mutable_request()->mutable_options()->clear_to_formats();
  request.mutable_request()->mutable_options()->add_to_formats(
      pipestream::parse::v1::OUTPUT_FORMAT_UNSPECIFIED);
  grpc::ClientContext unspecified_context;
  pipestream::parse::v1::ConvertSourceResponse unspecified_response;
  const grpc::Status unspecified_status =
      client->ConvertSource(&unspecified_context, request, &unspecified_response);
  require(unspecified_status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "an unrenderable output format must be rejected");
  require(unspecified_status.error_message().contains("OUTPUT_FORMAT_UNSPECIFIED"),
          "the rejection must name the unrenderable format: " +
              unspecified_status.error_message());
}

// The recognition options are accepted, validated by name, and steer the CV
// leg: do_ocr false yields no text from pages without an embedded layer, and
// render_scale resolves to the DPI the source factory sees.
void verify_recognition_options_steer_the_cv_leg(TestServer* server) {
  auto client = server->unary_stub();

  // do_ocr true and force_ocr true are the recognizing modes; on a source
  // with no embedded layer both convert every page through the recognizer.
  for (const bool force : {false, true}) {
    auto request = unary_request();
    if (force) {
      request.mutable_request()->mutable_options()->set_force_ocr(true);
    } else {
      request.mutable_request()->mutable_options()->set_do_ocr(true);
    }
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    pipestream::parse::v1::ConvertSourceResponse response;
    const grpc::Status status = client->ConvertSource(&context, request, &response);
    require(status.ok(), "a recognizing mode must be accepted: " + status.error_message());
    require(response.response().document().exports().text() == "one\ntwo\nthree",
            "a recognizing mode must deliver recognized text");
  }

  // do_ocr false reads only the embedded layer; these pages have none, so
  // the parse succeeds with pages and no text.
  {
    const int calls_before = server->recognizer_calls();
    auto request = unary_request();
    request.mutable_request()->mutable_options()->set_do_ocr(false);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    pipestream::parse::v1::ConvertSourceResponse response;
    const grpc::Status status = client->ConvertSource(&context, request, &response);
    require(status.ok(), "do_ocr false must be accepted: " + status.error_message());
    require(server->recognizer_calls() == calls_before,
            "do_ocr false must not invoke the recognizer");
    const auto& document = response.response().document().doc();
    require(document.pages_size() == 3 && document.texts_size() == 0,
            "pages without an embedded layer must yield no text under do_ocr false");
    require(response.response().document().exports().text().empty(),
            "the do_ocr false text export must be empty");
  }

  // render_scale is multiples of 72 DPI; 2.0 reaches the source factory as
  // 144, unset keeps the 200 DPI default.
  {
    auto request = unary_request();
    request.mutable_request()->mutable_options()->set_render_scale(2.0);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    pipestream::parse::v1::ConvertSourceResponse response;
    const grpc::Status status = client->ConvertSource(&context, request, &response);
    require(status.ok(), "render_scale 2.0 must be accepted: " + status.error_message());
    require(server->last_render_dpi() == 144.0,
            "render_scale must resolve to multiples of 72 DPI");

    grpc::ClientContext default_context;
    default_context.set_deadline(std::chrono::system_clock::now() + 10s);
    pipestream::parse::v1::ConvertSourceResponse default_response;
    const grpc::Status default_status =
        client->ConvertSource(&default_context, unary_request(), &default_response);
    require(default_status.ok(), "default-scale conversion failed");
    require(server->last_render_dpi() == 200.0,
            "an unset render_scale must keep the 200 DPI default");
  }

  // Out-of-range scales and the contradictory mode pair are rejected by name.
  for (const double scale : {0.5, 9.0}) {
    auto request = unary_request();
    request.mutable_request()->mutable_options()->set_render_scale(scale);
    grpc::ClientContext context;
    pipestream::parse::v1::ConvertSourceResponse response;
    const grpc::Status status = client->ConvertSource(&context, request, &response);
    require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
            "render_scale outside [1.0, 8.0] must be rejected");
    require(status.error_message().contains("render_scale"),
            "the rejection must name render_scale: " + status.error_message());
  }
  {
    auto request = unary_request();
    request.mutable_request()->mutable_options()->set_do_ocr(false);
    request.mutable_request()->mutable_options()->set_force_ocr(true);
    grpc::ClientContext context;
    pipestream::parse::v1::ConvertSourceResponse response;
    const grpc::Status status = client->ConvertSource(&context, request, &response);
    require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
            "do_ocr false with force_ocr true must be rejected");
    require(status.error_message().contains("do_ocr") &&
                status.error_message().contains("force_ocr"),
            "the rejection must name both options: " + status.error_message());
  }
}

// Every requested output format renders in one response; leaving to_formats
// empty keeps the plain-text default alone.
void verify_unary_multi_format_exports(TestServer* server) {
  auto client = server->unary_stub();
  auto request = unary_request();
  auto* options = request.mutable_request()->mutable_options();
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_HTML);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_JSON);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_VTT);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE);
  options->add_to_formats(pipestream::parse::v1::OUTPUT_FORMAT_YAML);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  pipestream::parse::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, request, &response);
  require(status.ok(), "multi-format conversion failed: " + status.error_message());

  const auto& exports = response.response().document().exports();
  require(exports.text() == "one\ntwo\nthree", "multi-format text export");
  require(exports.has_md() && exports.md() == "one\n\ntwo\n\nthree",
          "multi-format markdown export: " + exports.md());
  require(exports.has_html() &&
              exports.html().contains("<p>one</p>") &&
              exports.html().starts_with("<!DOCTYPE html>"),
          "multi-format html export: " + exports.html());
  require(exports.has_json() &&
              exports.json().contains("\"texts\"") &&
              exports.json().contains("\"self_ref\""),
          "multi-format json export");
  // The CV pages carry provenance, so the doctags text items may carry loc
  // tokens between the tag and the content.
  require(exports.has_doctags() && exports.doctags().starts_with("<doctag>") &&
              exports.doctags().contains("one</text>"),
          "multi-format doctags export: " + exports.doctags());
  require(exports.has_doclang() &&
              exports.doclang().starts_with(
                  "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">"),
          "multi-format doclang export: " + exports.doclang());
  require(exports.has_vtt() && exports.vtt() == "WEBVTT",
          "an untimed document's vtt export is the bare header: " + exports.vtt());
  require(exports.has_html_split_page() &&
              exports.html_split_page().contains("<div class='page'>"),
          "multi-format split-page export");
  require(exports.has_yaml() && exports.yaml().contains("texts:"),
          "multi-format yaml export: " + exports.yaml().substr(0, 200));

  auto default_request = unary_request();
  default_request.mutable_request()->mutable_options()->clear_to_formats();
  grpc::ClientContext default_context;
  default_context.set_deadline(std::chrono::system_clock::now() + 10s);
  pipestream::parse::v1::ConvertSourceResponse default_response;
  const grpc::Status default_status =
      client->ConvertSource(&default_context, default_request, &default_response);
  require(default_status.ok(),
          "default-format conversion failed: " + default_status.error_message());
  const auto& default_exports = default_response.response().document().exports();
  require(default_exports.has_text() && !default_exports.has_md() &&
              !default_exports.has_html() && !default_exports.has_json() &&
              !default_exports.has_doctags() && !default_exports.has_doclang() &&
              !default_exports.has_vtt() && !default_exports.has_html_split_page() &&
              !default_exports.has_yaml(),
          "empty to_formats must keep the plain-text default alone");
}

void verify_unary_digital_path_bypasses_ocr() {
  TestServer server(0ms, true);
  auto client = server.unary_stub();
  grpc::ClientContext context;
  pipestream::parse::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, unary_request(), &response);
  require(status.ok(), "digital unary conversion failed: " + status.error_message());
  require(server.recognizer_calls() == 0, "unary conversion bypassed the scheduler digital path");
  require(response.response().document().exports().text() ==
              "native-one\nnative-two\nnative-three",
          "digital unary text export");

  auto stream_client = server.stub();
  grpc::ClientContext stream_context;
  auto stream = stream_client->StreamProcessDocument(&stream_context);
  require(stream->Write(chunk(true)), "digital stream could not write source chunk");
  stream->WritesDone();
  std::vector<pipestream::parse::v1::DocumentStreamEvent> events;
  pipestream::parse::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) events.push_back(event);
  const grpc::Status stream_status = stream->Finish();
  require(stream_status.ok(), "digital stream failed: " + stream_status.error_message());
  require(events.size() == 4 && events.back().has_complete(), "digital stream event count");
  require(server.recognizer_calls() == 0, "digital unary or stream conversion entered RapidOCR");

  const auto& unary_document = response.response().document().doc();
  uint64_t expected_offset = 0;
  for (int page_index = 0; page_index < 3; ++page_index) {
    const auto& streamed_page = events.at(page_index).page();
    require(streamed_page.texts_size() == 1 && streamed_page.text_offsets_size() == 1,
            "digital stream page payload");
    require(streamed_page.texts(0).SerializeAsString() ==
                unary_document.texts(page_index).SerializeAsString(),
            "unary and stream core text fidelity differs");
    const auto& offset = streamed_page.text_offsets(0);
    require(offset.utf_start() == expected_offset, "digital stream offset start");
    expected_offset = offset.utf_end() + (page_index == 2 ? 0 : 1);
    require(offset.source() == pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF,
            "digital stream source metadata");
  }
  require(expected_offset == grparse::utf8_codepoint_count(
                                 response.response().document().exports().text()),
          "stream offsets do not cover the unary text export");
}

// A weak embedded layer over a renderable page: selective recognition would
// merge, recognition off keeps the native line alone, and forced recognition
// replaces it with the recognizer's text.
class WeakDigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 3; }

  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    static const std::vector<std::string> text{"", "native-one", "native-two", "native-three"};
    grparse::OcrPage page{100, 200,
                          {{text.at(page_number), {{1, 30}, {20, 30}, {20, 40}, {1, 40}},
                            std::nullopt, grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = false;
    return page;
  }

  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }
};

pipestream::parse::v1::DocumentChunk recognition_chunk(const std::string& data, bool complete) {
  pipestream::parse::v1::DocumentChunk value;
  value.set_document_id("recognition-stream");
  value.set_filename("image.png");
  value.set_content_type("image/png");
  value.set_data(data);
  value.set_complete(complete);
  return value;
}

struct StreamRun {
  std::vector<pipestream::parse::v1::DocumentStreamEvent> events;
  grpc::Status status;
};

StreamRun run_chunks(pipestream::parse::v1::ParseStreamingService::Stub* client,
                     std::vector<pipestream::parse::v1::DocumentChunk> chunks) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  auto stream = client->StreamProcessDocument(&context);
  for (auto& value : chunks) {
    require(stream->Write(value), "recognition stream could not write a chunk");
  }
  stream->WritesDone();
  StreamRun run;
  pipestream::parse::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) run.events.push_back(event);
  run.status = stream->Finish();
  return run;
}

// The streaming chunk fields do_ocr, force_ocr, and render_scale resolve from
// the first chunk that sets each one, steer the CV leg exactly like the unary
// options, and fail the stream by name when invalid.
void verify_stream_resolves_recognition_options() {
  FakeRecognizer recognizer;
  std::atomic<double> factory_dpi{0.0};
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 2, 2, 2},
      [&factory_dpi](std::shared_ptr<const std::string>, bool, double render_dpi) {
        factory_dpi.store(render_dpi);
        return std::make_shared<WeakDigitalSource>();
      });
  grparse::DocumentStreamingService streaming_service(
      scheduler, std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{}));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&streaming_service);
  auto server = builder.BuildAndStart();
  require(server && port != 0, "recognition test server failed to start");
  auto client = pipestream::parse::v1::ParseStreamingService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));

  // force_ocr on the first chunk: every page is recognized and the native
  // lines are gone from the delivered text.
  {
    auto first = recognition_chunk("in-memory", false);
    first.set_force_ocr(true);
    const StreamRun run =
        run_chunks(client.get(), {first, recognition_chunk("-source", true)});
    require(run.status.ok(), "forced stream failed: " + run.status.error_message());
    require(run.events.size() == 4 && run.events.back().has_complete(),
            "forced stream event count");
    require(recognizer.calls.load() == 3, "forced stream must recognize every page");
    for (int index = 0; index < 3; ++index) {
      const auto& page = run.events.at(index).page();
      require(page.texts_size() == 1 &&
                  !page.texts(0).text().base().text().starts_with("native-"),
              "forced stream text must come from recognition, not the embedded layer");
    }
  }

  // do_ocr false, set on a later chunk: the first chunk that sets a field
  // wins wherever it sits, and only the embedded layer is delivered.
  {
    const int calls_before = recognizer.calls.load();
    auto last = recognition_chunk("-source", true);
    last.set_do_ocr(false);
    const StreamRun run =
        run_chunks(client.get(), {recognition_chunk("in-memory", false), last});
    require(run.status.ok(), "recognition-off stream failed: " + run.status.error_message());
    require(recognizer.calls.load() == calls_before,
            "recognition-off stream must not invoke the recognizer");
    static const std::vector<std::string> native{"native-one", "native-two", "native-three"};
    for (int index = 0; index < 3; ++index) {
      const auto& page = run.events.at(index).page();
      require(page.texts_size() == 1 &&
                  page.texts(0).text().base().text() == native.at(index),
              "recognition-off stream must deliver the embedded layer alone");
    }
  }

  // render_scale resolves to multiples of 72 DPI at the source factory.
  {
    auto first = recognition_chunk("in-memory-source", true);
    first.set_render_scale(3.0);
    const StreamRun run = run_chunks(client.get(), {first});
    require(run.status.ok(), "scaled stream failed: " + run.status.error_message());
    require(factory_dpi.load() == 216.0,
            "the streamed render_scale must reach the source factory as DPI");
  }

  // Invalid values fail the stream by name, the contradiction even when its
  // halves arrive on different chunks.
  {
    auto first = recognition_chunk("in-memory-source", true);
    first.set_render_scale(9.0);
    const StreamRun run = run_chunks(client.get(), {first});
    require(run.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
                run.status.error_message().contains("render_scale"),
            "an out-of-range streamed render_scale must be rejected by name: " +
                run.status.error_message());
  }
  {
    auto first = recognition_chunk("in-memory", false);
    first.set_do_ocr(false);
    auto last = recognition_chunk("-source", true);
    last.set_force_ocr(true);
    const StreamRun run = run_chunks(client.get(), {first, last});
    require(run.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
                run.status.error_message().contains("do_ocr") &&
                run.status.error_message().contains("force_ocr"),
            "a cross-chunk mode contradiction must be rejected by name: " +
                run.status.error_message());
  }

  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();
}

void verify_get_service_info(TestServer* server) {
  auto client = server->unary_stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  pipestream::parse::v1::GetServiceInfoResponse response;
  const grpc::Status status =
      client->GetServiceInfo(&context, pipestream::parse::v1::GetServiceInfoRequest{}, &response);
  require(status.ok(), "GetServiceInfo failed: " + status.error_message());
  require(response.name() == "gRParse" && !response.version().empty(),
          "GetServiceInfo name/version");
  require(response.ui().title() == "gRParse" && response.ui().path() == "/ui/grparse" &&
              !response.ui().description().empty(),
          "GetServiceInfo ui advertisement");
}

}  // namespace

// ---- pdf inspector routing ---------------------------------------------------

namespace {

// A routable PDF source: full-coverage digital layers on every page, but
// renderable — the inspector's page set can send a page through OCR that
// the coverage heuristic alone would have settled.
class RoutableDigitalSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 3; }
  std::optional<grparse::OcrPage> extract_digital_page(int page_number) const override {
    static const std::vector<std::string> text{"", "native-one", "native-two", "native-three"};
    grparse::OcrPage page{100, 200,
                          {{text.at(page_number), {{1, 2}, {20, 2}, {20, 12}, {1, 12}},
                            std::nullopt, grparse::TextOrigin::kDigitalPdf}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
    page.skip_ocr = true;
    return page;
  }
  cv::Mat render_page(int page_number) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(page_number)).clone();
  }
};

// Serves the inspector's routing contract: the classification first, then
// one folded document, then the status trailer.
class FakePdfInspector final : public pdfv1::PdfParseService::Service {
 public:
  FakePdfInspector(pdfv1::PdfType type, std::vector<uint32_t> pages_needing_ocr)
      : type_(type), pages_needing_ocr_(std::move(pages_needing_ocr)) {}

  grpc::Status ParsePdf(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<pdfv1::ParsePdfResponse, pdfv1::ParsePdfRequest>* stream)
      override {
    pdfv1::ParsePdfRequest request;
    bool emit_document = false;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        emit_document = request.options().emit_document();
      } else {
        bytes += request.chunk();
      }
    }
    if (!emit_document || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake pdf inspector expects emit_document and bytes");
    }
    pdfv1::ParsePdfResponse event;
    auto* info = event.mutable_info();
    info->set_pdf_type(type_);
    info->set_page_count(3);
    for (const uint32_t page : pages_needing_ocr_) info->add_pages_needing_ocr(page);
    stream->Write(event);
    event.Clear();
    docv1::Document document;
    document.mutable_body()->set_self_ref("#/body");
    document.mutable_furniture()->set_self_ref("#/furniture");
    auto* base = document.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/0");
    base->mutable_parent()->set_ref("#/body");
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_text("from pdf inspector");
    base->add_source()->mutable_collector()->set_collector("pdf");
    document.mutable_body()->add_children()->set_ref("#/texts/0");
    *event.mutable_document() = std::move(document);
    stream->Write(event);
    event.Clear();
    event.mutable_status()->set_pages_extracted(0);
    stream->Write(event);
    return grpc::Status::OK;
  }

 private:
  pdfv1::PdfType type_;
  std::vector<uint32_t> pages_needing_ocr_;
};

class FailingPdfInspector final : public pdfv1::PdfParseService::Service {
 public:
  grpc::Status ParsePdf(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<pdfv1::ParsePdfResponse, pdfv1::ParsePdfRequest>* stream)
      override {
    pdfv1::ParsePdfRequest request;
    while (stream->Read(&request)) {
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "inspector down");
  }
};

class PdfInspectorServer {
 public:
  explicit PdfInspectorServer(pdfv1::PdfParseService::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (!server_ || port_ == 0) throw std::runtime_error("fake pdf inspector failed to start");
  }
  ~PdfInspectorServer() {
    if (server_) server_->Shutdown();
  }
  std::string target() const { return "127.0.0.1:" + std::to_string(port_); }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

struct UnaryPdfRun {
  grpc::Status status;
  pipestream::parse::v1::ConvertSourceResponse response;
  int recognizer_calls = 0;
};

// One unary ConvertSource for a .pdf against a gRParse whose only
// configured collector is the given pdf inspector target.
UnaryPdfRun run_unary_pdf(const std::string& pdf_target) {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 2, 2, 2},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RoutableDigitalSource>();
      });
  grparse::CollectorTargets targets;
  targets.pdf = pdf_target;
  grparse::DocumentParserService parser_service(
      scheduler, std::make_shared<grparse::CollectorEndpoints>(targets));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&parser_service);
  auto server = builder.BuildAndStart();
  require(server && port != 0, "pdf routing test server failed to start");
  auto client = pipestream::parse::v1::ParseService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  pipestream::parse::v1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename("routing.pdf");
  const std::string bytes = "%PDF-in-memory";
  source->set_base64_string(grparse::encode_base64(bytes.data(), bytes.size()));
  UnaryPdfRun run;
  run.status = client->ConvertSource(&context, request, &run.response);
  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();
  run.recognizer_calls = recognizer.calls.load();
  return run;
}

struct StreamPdfRun {
  grpc::Status status;
  std::vector<pipestream::parse::v1::DocumentStreamEvent> events;
  int recognizer_calls = 0;
};

StreamPdfRun run_stream_pdf(const std::string& pdf_target) {
  FakeRecognizer recognizer;
  grparse::PageScheduler scheduler(
      recognizer, {2, 3, 2, 3, 2, 2, 2},
      [](std::shared_ptr<const std::string>, bool, double) {
        return std::make_shared<RoutableDigitalSource>();
      });
  grparse::CollectorTargets targets;
  targets.pdf = pdf_target;
  grparse::DocumentStreamingService streaming_service(
      scheduler, std::make_shared<grparse::CollectorEndpoints>(targets));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&streaming_service);
  auto server = builder.BuildAndStart();
  require(server && port != 0, "pdf routing stream server failed to start");
  auto client = pipestream::parse::v1::ParseStreamingService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  auto stream = client->StreamProcessDocument(&context);
  pipestream::parse::v1::DocumentChunk chunk;
  chunk.set_document_id("pdf-routing");
  chunk.set_filename("routing.pdf");
  chunk.set_content_type("application/pdf");
  chunk.set_data("%PDF-in-memory");
  chunk.set_complete(true);
  require(stream->Write(chunk), "pdf routing client could not write the source chunk");
  stream->WritesDone();
  StreamPdfRun run;
  pipestream::parse::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) run.events.push_back(event);
  run.status = stream->Finish();
  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();
  run.recognizer_calls = recognizer.calls.load();
  return run;
}

void verify_pdf_fast_path_skips_the_cv_pipeline() {
  FakePdfInspector inspector(pdfv1::PDF_TYPE_TEXT_BASED, {});
  PdfInspectorServer inspector_server(&inspector);
  const UnaryPdfRun run = run_unary_pdf(inspector_server.target());
  require(run.status.ok(), "fast-path parse failed: " + run.status.error_message());
  require(run.recognizer_calls == 0, "the fast path must not touch the recognizer");
  const auto& document = run.response.response().document().doc();
  require(document.texts_size() == 1 &&
              document.texts(0).text().base().text() == "from pdf inspector",
          "the collector's folded document is the parse result");
  require(document.texts(0).text().base().source(0).collector().collector() == "pdf",
          "the fast-path document keeps the collector's source tag");
}

void verify_pdf_classification_restricts_recognition() {
  FakePdfInspector inspector(pdfv1::PDF_TYPE_MIXED, {2});
  PdfInspectorServer inspector_server(&inspector);
  const UnaryPdfRun run = run_unary_pdf(inspector_server.target());
  require(run.status.ok(), "routed parse failed: " + run.status.error_message());
  require(run.recognizer_calls == 1,
          "only the inspector's page hits the recognizer, not the coverage heuristic's set");
  require(run.response.response().document().exports().text().contains("native-one"),
          "the cleared pages settle on their embedded layers");
}

void verify_pdf_collector_failure_degrades_to_the_cv_path() {
  FailingPdfInspector inspector;
  PdfInspectorServer inspector_server(&inspector);
  const UnaryPdfRun run = run_unary_pdf(inspector_server.target());
  require(run.status.ok(), "a down inspector must not fail the parse: " +
                               run.status.error_message());
  require(run.recognizer_calls == 0,
          "the fallback CV run parses digitally, exactly as without the collector");
  const auto& fields = run.response.response().document().doc().body().meta().custom_fields();
  require(fields.count("collector_warnings:pdf") == 1 &&
              fields.at("collector_warnings:pdf").list_value().values(0).string_value().contains(
                  "fell back to the in-process CV path"),
          "the degradation is recorded as a collector warning");
}

void verify_streaming_pdf_fast_path_emits_the_collector_document() {
  FakePdfInspector inspector(pdfv1::PDF_TYPE_TEXT_BASED, {});
  PdfInspectorServer inspector_server(&inspector);
  const StreamPdfRun run = run_stream_pdf(inspector_server.target());
  require(run.status.ok(), "fast-path stream failed: " + run.status.error_message());
  require(run.recognizer_calls == 0, "the streamed fast path must not touch the recognizer");
  require(run.events.size() == 2, "one collector document event, then the complete event");
  require(run.events.at(0).has_collector_document() &&
              run.events.at(0).collector_document().collector() ==
                  pipestream::parse::v1::COLLECTOR_PDF &&
              run.events.at(0).collector_document().document().texts(0).text().base().text() ==
                  "from pdf inspector",
          "the collector document event carries the inspector's fold");
  require(run.events.at(1).has_complete(), "the stream closes with the complete event");
}

void verify_streaming_pdf_classification_restricts_recognition() {
  FakePdfInspector inspector(pdfv1::PDF_TYPE_SCANNED, {1, 2, 3});
  PdfInspectorServer inspector_server(&inspector);
  const StreamPdfRun run = run_stream_pdf(inspector_server.target());
  require(run.status.ok(), "routed stream failed: " + run.status.error_message());
  require(run.recognizer_calls == 3, "a scanned document recognizes every named page");
  require(run.events.size() == 4 && run.events.back().has_complete(),
          "the CV path's page events stream, then the complete event");
  for (int index = 0; index < 3; ++index) {
    require(run.events.at(index).has_page() &&
                run.events.at(index).page().page_number() == index + 1,
            "routed CV pages stream in document order");
  }
}

}  // namespace

int main() {
  try {
    TestServer server;
    verify_ordered_page_stream(&server);
    verify_data_after_complete_is_rejected(&server);
    verify_unary_uses_scheduler_and_shared_assembly(&server);
    verify_unsupported_options_are_rejected(&server);
    verify_recognition_options_steer_the_cv_leg(&server);
    verify_unary_multi_format_exports(&server);
    verify_stream_resolves_recognition_options();
    verify_unary_digital_path_bypasses_ocr();
    verify_wide_page_window_streams_completely();
    verify_deadline_cancels_scheduler_work();
    verify_get_service_info(&server);
    verify_pdf_fast_path_skips_the_cv_pipeline();
    verify_pdf_classification_restricts_recognition();
    verify_pdf_collector_failure_degrades_to_the_cv_path();
    verify_streaming_pdf_fast_path_emits_the_collector_document();
    verify_streaming_pdf_classification_restricts_recognition();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "streaming-service-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
