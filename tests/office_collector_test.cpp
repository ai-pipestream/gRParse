// Exercises the libreoffice collector client against a fake in-process
// OfficeRenderService serving canned typed events, so the transport, the
// mapper folding, and the failure paths are proven without LibreOffice.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/office/v1/office_service.grpc.pb.h"
#include "grparse/office_collector.h"

namespace officev1 = ai::pipestream::office::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Reads the upload, checks the chunk contract, and streams a one-paragraph
// typed document back.
class FakeOfficeService final : public officev1::OfficeRenderService::Service {
 public:
  grpc::Status StreamPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<officev1::StreamPagesResponse,
                               officev1::StreamPagesRequest>* stream) override {
    officev1::StreamPagesRequest request;
    std::string bytes;
    std::string document_id;
    std::string filename;
    bool complete = false;
    while (stream->Read(&request)) {
      if (document_id.empty()) {
        document_id = request.chunk().document_id();
        filename = request.chunk().filename();
      }
      bytes += request.chunk().data();
      complete = request.chunk().complete();
    }
    if (document_id.empty() || filename.empty() || !complete || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake collector expects a complete identified upload");
    }

    officev1::StreamPagesResponse event;
    officev1::DocumentInfo* info = event.mutable_document_info();
    info->set_document_id(document_id);
    info->set_source_format("odt");
    info->set_document_type("text");
    info->set_page_count(1);
    officev1::PageRect* page = info->add_page_rects();
    page->set_x_twips(284);
    page->set_y_twips(284);
    page->set_width_twips(12240);
    page->set_height_twips(15840);
    stream->Write(event);

    event.Clear();
    officev1::Paragraph* paragraph = event.mutable_paragraph();
    paragraph->set_list_level(-1);
    paragraph->set_page_index(0);
    paragraph->set_char_offset(0);
    officev1::TextRun* run = paragraph->add_runs();
    run->set_text("hello office");
    run->set_char_offset(0);
    run->set_char_length(12);
    run->set_hyperlink_url("https://example.test/office");
    stream->Write(event);

    event.Clear();
    officev1::Comment* comment = event.mutable_comment();
    comment->set_index(0);
    comment->set_name("cmt1");
    comment->set_author("Alice");
    comment->set_text("collector comment");
    comment->set_char_start(0);
    comment->set_char_end(5);
    comment->set_page_index(0);
    stream->Write(event);

    event.Clear();
    officev1::FormField* field = event.mutable_form_field();
    field->set_index(0);
    field->set_kind(officev1::FORM_FIELD_KIND_CHECKBOX);
    field->set_field_type("vnd.oasis.opendocument.field.FORMCHECKBOX");
    field->set_name("check1");
    field->set_checked(true);
    field->set_char_start(6);
    field->set_char_end(6);
    field->set_selected_index(-1);
    stream->Write(event);

    event.Clear();
    event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
    event.mutable_status()->add_warnings("one office warning");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Rejects the upload with a load-failure status, like the real worker does
// for an unloadable document.
class RejectingOfficeService final : public officev1::OfficeRenderService::Service {
 public:
  grpc::Status StreamPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<officev1::StreamPagesResponse,
                               officev1::StreamPagesRequest>* stream) override {
    officev1::StreamPagesRequest request;
    while (stream->Read(&request)) {
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "cannot load document");
  }
};

// Ends the stream cleanly without ever sending the terminal RenderStatus.
class TruncatingOfficeService final : public officev1::OfficeRenderService::Service {
 public:
  grpc::Status StreamPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<officev1::StreamPagesResponse,
                               officev1::StreamPagesRequest>* stream) override {
    officev1::StreamPagesRequest request;
    while (stream->Read(&request)) {
    }
    officev1::StreamPagesResponse event;
    event.mutable_document_info()->set_document_type("text");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

class ServerFixture {
 public:
  explicit ServerFixture(grpc::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) {
      throw std::runtime_error("fake office server failed to start");
    }
  }
  ~ServerFixture() {
    if (server_ != nullptr) server_->Shutdown();
  }

  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                               grpc::InsecureChannelCredentials());
  }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

void verify_collects_and_folds_typed_stream() {
  FakeOfficeService service;
  ServerFixture server(&service);
  // Large enough to prove multi-chunk uploads reassemble.
  const std::string bytes(600U * 1024U, 'x');
  const auto outcome = grparse::collect_office_document(
      server.channel(), "doc-1", "letter.odt", "", bytes);
  require(outcome.success, "collection succeeds: " + outcome.error);
  require(outcome.warnings.size() == 1 && outcome.warnings[0] == "one office warning",
          "collector warnings surface verbatim");
  const auto& document = outcome.document;
  require(document.texts_size() == 3,
          "paragraph, comment, and form field fold into text items");
  require(document.texts(0).text().base().text() == "hello office",
          "typed paragraph folds into a text item");
  require(document.texts(0).text().base().hyperlink() ==
              "https://example.test/office",
          "the run's hyperlink lands on the folded item");
  require(document.texts(0).text().base().source_size() == 1 &&
              document.texts(0).text().base().source(0).collector().collector() ==
                  "libreoffice",
          "folded items carry the libreoffice collector source");
  bool comment_ok = false;
  bool checkbox_ok = false;
  for (const auto& item : document.texts()) {
    if (!item.has_text()) continue;
    const auto& base = item.text().base();
    if (base.text() == "collector comment") {
      comment_ok = base.meta().custom_fields().count("author") == 1 &&
                   base.meta().custom_fields().at("author").string_value() ==
                       "Alice";
    }
    if (base.label() ==
        ai::pipestream::document::v1::DOC_ITEM_LABEL_CHECKBOX_SELECTED) {
      checkbox_ok = base.meta().custom_fields().at("checked").bool_value();
    }
  }
  require(comment_ok, "the comment folds with its author");
  require(checkbox_ok, "the checkbox form field folds with its state");
  bool comment_group = false;
  bool form_group = false;
  for (const auto& group : document.groups()) {
    if (group.label() ==
        ai::pipestream::document::v1::GROUP_LABEL_COMMENT_SECTION) {
      comment_group = group.parent().ref() == "#/furniture";
    }
    if (group.label() == ai::pipestream::document::v1::GROUP_LABEL_FORM_AREA) {
      form_group = true;
    }
  }
  require(comment_group && form_group,
          "comments and form fields land in their sections");
  require(document.pages_size() == 1, "page rects become page items");
}

void verify_load_failure_degrades_to_outcome() {
  RejectingOfficeService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_office_document(
      server.channel(), "doc-2", "broken.odt", "", "bytes");
  require(!outcome.success, "load failure must not report success");
  require(outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "the collector's own status class survives");
  require(outcome.error.find("cannot load document") != std::string::npos,
          "the collector's error text survives");
}

void verify_truncated_stream_is_a_failure() {
  TruncatingOfficeService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_office_document(
      server.channel(), "doc-3", "cut.odt", "", "bytes");
  require(!outcome.success && outcome.error.find("terminal status") != std::string::npos,
          "a stream without RenderStatus is a failure, not an empty success");
}

void verify_unreachable_endpoint_is_a_failure() {
  const auto channel = grpc::CreateChannel("127.0.0.1:1",
                                           grpc::InsecureChannelCredentials());
  const auto outcome = grparse::collect_office_document(
      channel, "doc-4", "nowhere.odt", "", "bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "an unreachable collector degrades to UNAVAILABLE");
}

}  // namespace

int main() {
  try {
    verify_collects_and_folds_typed_stream();
    verify_load_failure_degrades_to_outcome();
    verify_truncated_stream_is_a_failure();
    verify_unreachable_endpoint_is_a_failure();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "office-collector-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
