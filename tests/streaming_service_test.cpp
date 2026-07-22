#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"
#include "grparse/document_assembly.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"

namespace {

using namespace std::chrono_literals;
namespace docling = ai::docling;

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
                          {{text.at(page_number), {{1, 2}, {20, 2}, {20, 12}, {1, 12}}}}};
    page.source = grparse::OcrPage::Source::kDigitalPdf;
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
        scheduler_(recognizer_, {2, 3, 2, 3, 2, 2, 2}, [digital](std::shared_ptr<const std::string>, bool) {
                 return std::make_shared<FakeSource>(digital);
               }),
        parser_service_(scheduler_),
        streaming_service_(scheduler_) {
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

  std::unique_ptr<docling::serve::v1::DoclingStreamingService::Stub> stub() const {
    return docling::serve::v1::DoclingStreamingService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials()));
  }

  std::unique_ptr<docling::serve::v1::DoclingServeService::Stub> unary_stub() const {
    return docling::serve::v1::DoclingServeService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials()));
  }

  grparse::PageScheduler::Metrics metrics() const { return scheduler_.metrics(); }
  int recognizer_calls() const { return recognizer_.calls.load(); }

 private:
  FakeRecognizer recognizer_;
  grparse::PageScheduler scheduler_;
  grparse::DocumentParserService parser_service_;
  grparse::DocumentStreamingService streaming_service_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};

docling::serve::v1::DocumentChunk chunk(bool complete) {
  docling::serve::v1::DocumentChunk value;
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

  std::vector<docling::serve::v1::DocumentStreamEvent> events;
  docling::serve::v1::DocumentStreamEvent event;
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
  docling::serve::v1::DocumentStreamEvent ignored;
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
  docling::serve::v1::DocumentStreamEvent ignored;
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

docling::serve::v1::ConvertSourceRequest unary_request() {
  docling::serve::v1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename("image.png");
  source->set_base64_string("bWVtb3J5");
  request.mutable_request()->mutable_options()->add_to_formats(docling::serve::v1::OUTPUT_FORMAT_TEXT);
  return request;
}

void verify_unary_uses_scheduler_and_shared_assembly(TestServer* server) {
  auto client = server->unary_stub();
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  docling::serve::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, unary_request(), &response);
  require(status.ok(), "unary conversion failed: " + status.error_message());

  const auto& result = response.response().document();
  require(response.response().status() == docling::serve::v1::CONVERSION_STATUS_SUCCESS,
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
  request.mutable_request()->mutable_options()->set_do_ocr(true);
  grpc::ClientContext context;
  docling::serve::v1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, request, &response);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "unsupported conversion options must be rejected");

  request = unary_request();
  request.mutable_request()->mutable_options()->clear_to_formats();
  request.mutable_request()->mutable_options()->add_to_formats(
      docling::serve::v1::OUTPUT_FORMAT_MARKDOWN);
  grpc::ClientContext markdown_context;
  docling::serve::v1::ConvertSourceResponse markdown_response;
  const grpc::Status markdown_status =
      client->ConvertSource(&markdown_context, request, &markdown_response);
  require(markdown_status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "plain text must not be presented as Markdown");
}

void verify_unary_digital_path_bypasses_ocr() {
  TestServer server(0ms, true);
  auto client = server.unary_stub();
  grpc::ClientContext context;
  docling::serve::v1::ConvertSourceResponse response;
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
  std::vector<docling::serve::v1::DocumentStreamEvent> events;
  docling::serve::v1::DocumentStreamEvent event;
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
    require(offset.source() == docling::serve::v1::TEXT_SOURCE_DIGITAL_PDF,
            "digital stream source metadata");
  }
  require(expected_offset == grparse::utf8_codepoint_count(
                                 response.response().document().exports().text()),
          "stream offsets do not cover the unary text export");
}

}  // namespace

int main() {
  try {
    TestServer server;
    verify_ordered_page_stream(&server);
    verify_data_after_complete_is_rejected(&server);
    verify_unary_uses_scheduler_and_shared_assembly(&server);
    verify_unsupported_options_are_rejected(&server);
    verify_unary_digital_path_bypasses_ocr();
    verify_deadline_cancels_scheduler_work();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "streaming-service-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
