// Exercises the Document-emitting collector clients against fake in-process
// services, so the option forwarding (emit_document, the asr model, the
// ebcdic layout), the upload chunk contracts, the document capture, the
// warning surfacing, and the failure paths are proven without any collector
// binary.

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "grparse/document_collectors.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace docv1 = ai::pipestream::document::v1;
namespace ebcdicv1 = ai::pipestream::ebcdic::v1;
namespace emailv1 = ai::pipestream::email::v1;
namespace epubv1 = ai::pipestream::epub::v1;
namespace xmlv1 = ai::pipestream::xml::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// The one-item Document every fake serves; the clients must carry it across
// unchanged, because the fold already happened in the collector.
docv1::Document canned_document(const std::string& collector) {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  auto* base = document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text("from " + collector);
  base->add_source()->mutable_collector()->set_collector(collector);
  document.mutable_body()->add_children()->set_ref("#/texts/0");
  return document;
}

class ServerFixture {
 public:
  explicit ServerFixture(grpc::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) {
      throw std::runtime_error("fake collector server failed to start");
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

// ---- asr -------------------------------------------------------------------

class FakeAsrService final : public asrv1::AsrService::Service {
 public:
  grpc::Status Transcribe(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<asrv1::TranscribeResponse, asrv1::TranscribeRequest>*
          stream) override {
    asrv1::TranscribeRequest request;
    std::string model;
    bool emit_document = false;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        model = request.options().model();
        emit_document = request.options().emit_document();
      } else {
        bytes += request.chunk().data();
      }
    }
    if (model != "small" || !emit_document || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake asr expects model, emit_document, and media bytes");
    }
    asrv1::TranscribeResponse event;
    event.mutable_final_segment()->set_text("typed segment");
    stream->Write(event);
    event.Clear();
    *event.mutable_document() = canned_document("asr");
    stream->Write(event);
    event.Clear();
    event.mutable_complete()->set_language("en");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

class FailingAsrService final : public asrv1::AsrService::Service {
 public:
  grpc::Status Transcribe(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<asrv1::TranscribeResponse, asrv1::TranscribeRequest>*
          stream) override {
    asrv1::TranscribeRequest request;
    while (stream->Read(&request)) {
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "decoder blew up");
  }
};

void verify_asr_collects_document() {
  FakeAsrService service;
  ServerFixture server(&service);
  // Large enough to prove multi-chunk uploads reassemble.
  const auto outcome =
      grparse::collect_asr_document(server.channel(), "small",
                                    std::string(600U * 1024U, 'x'));
  require(outcome.success, "asr collection succeeds: " + outcome.error);
  require(outcome.document.texts_size() == 1 &&
              outcome.document.texts(0).text().base().text() == "from asr",
          "the collector's own Document arrives unchanged");
  require(outcome.warnings.empty(), "asr has no trailer warnings to surface");
}

void verify_transport_class_collapses_to_unavailable() {
  FailingAsrService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_asr_document(server.channel(), "small", "abc");
  require(!outcome.success, "an INTERNAL collector failure is an outcome");
  require(outcome.code == grpc::StatusCode::UNAVAILABLE,
          "non-caller status classes collapse to UNAVAILABLE");
  require(outcome.error.find("decoder blew up") != std::string::npos,
          "the collector's own message survives");
}

// ---- email -----------------------------------------------------------------

class FakeEmailService final : public emailv1::EmailParseService::Service {
 public:
  grpc::Status ParseEmail(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<emailv1::ParseEmailResponse, emailv1::ParseEmailRequest>*
          stream) override {
    emailv1::ParseEmailRequest request;
    emailv1::ParseEmailOptions options;
    std::string bytes;
    bool complete = false;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        options = request.options();
      } else {
        bytes += request.chunk().data();
        complete = request.chunk().complete();
      }
    }
    if (!options.emit_document() || options.document_id() != "doc-9" ||
        options.filename() != "thread.eml" || options.content_type() != "message/rfc822" ||
        !complete || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake email expects identity, emit_document, and a "
                          "complete-marked upload");
    }
    emailv1::ParseEmailResponse event;
    *event.mutable_document() = canned_document("email");
    stream->Write(event);
    event.Clear();
    event.mutable_status()->set_state(emailv1::ParseStatus::STATE_PARTIAL);
    event.mutable_status()->add_warnings("rtf body skipped");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Sends the document but never the ParseStatus trailer.
class TruncatingEmailService final : public emailv1::EmailParseService::Service {
 public:
  grpc::Status ParseEmail(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<emailv1::ParseEmailResponse, emailv1::ParseEmailRequest>*
          stream) override {
    emailv1::ParseEmailRequest request;
    while (stream->Read(&request)) {
    }
    emailv1::ParseEmailResponse event;
    *event.mutable_document() = canned_document("email");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_email_collects_document_and_warnings() {
  FakeEmailService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_email_document(
      server.channel(), "doc-9", "thread.eml", "message/rfc822",
      std::string(300U * 1024U, 'e'));
  require(outcome.success, "email collection succeeds: " + outcome.error);
  require(outcome.document.texts(0).text().base().text() == "from email",
          "the email Document arrives unchanged");
  require(outcome.warnings.size() == 1 && outcome.warnings[0] == "rtf body skipped",
          "email trailer warnings surface verbatim");
}

void verify_missing_trailer_fails() {
  TruncatingEmailService service;
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_email_document(server.channel(), "d", "f.eml", "", "abc");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a stream without a terminal status fails the collector");
  require(outcome.error.find("terminal status") != std::string::npos,
          "the truncation names itself");
}

// ---- xml -------------------------------------------------------------------

class FakeXmlService final : public xmlv1::XmlParseService::Service {
 public:
  grpc::Status ParseXml(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<xmlv1::ParseXmlResponse, xmlv1::ParseXmlRequest>* stream)
      override {
    xmlv1::ParseXmlRequest request;
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
                          "fake xml expects emit_document and bytes");
    }
    xmlv1::ParseXmlResponse event;
    *event.mutable_document() = canned_document("xml");
    stream->Write(event);
    event.Clear();
    auto* warning = event.mutable_status()->add_warnings();
    warning->set_code(xmlv1::WARNING_CODE_UNMAPPED_ELEMENT);
    warning->set_message("mystery element");
    warning->set_count(3);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

class RejectingXmlService final : public xmlv1::XmlParseService::Service {
 public:
  grpc::Status ParseXml(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<xmlv1::ParseXmlResponse, xmlv1::ParseXmlRequest>* stream)
      override {
    xmlv1::ParseXmlRequest request;
    while (stream->Read(&request)) {
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "sniff found nothing");
  }
};

void verify_xml_collects_document_and_formats_warnings() {
  FakeXmlService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_xml_document(server.channel(), "<a/>");
  require(outcome.success, "xml collection succeeds: " + outcome.error);
  require(outcome.document.texts(0).text().base().text() == "from xml",
          "the xml Document arrives unchanged");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] ==
                  "WARNING_CODE_UNMAPPED_ELEMENT: mystery element (x3)",
          "structured xml warnings flatten with code and count");
}

void verify_caller_status_classes_survive() {
  RejectingXmlService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_xml_document(server.channel(), "<a/>");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "INVALID_ARGUMENT survives the mapping");
  require(outcome.error.find("sniff found nothing") != std::string::npos,
          "the collector's own message survives");
}

// ---- ebcdic ----------------------------------------------------------------

class FakeEbcdicService final : public ebcdicv1::EbcdicParseService::Service {
 public:
  grpc::Status ParseEbcdic(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<ebcdicv1::ParseEbcdicResponse, ebcdicv1::ParseEbcdicRequest>*
          stream) override {
    ebcdicv1::ParseEbcdicRequest request;
    std::string layout_json;
    bool emit_document = false;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        layout_json = request.options().layout_json();
        emit_document = request.options().emit_document();
      } else {
        bytes += request.chunk();
      }
    }
    if (layout_json != R"({"records": []})" || !emit_document || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake ebcdic expects the layout json verbatim");
    }
    ebcdicv1::ParseEbcdicResponse event;
    *event.mutable_document() = canned_document("ebcdic");
    stream->Write(event);
    event.Clear();
    auto* warning = event.mutable_status()->add_warnings();
    warning->set_code(ebcdicv1::WARNING_CODE_TRAILING_PARTIAL_RECORD);
    warning->set_message("7 bytes left over");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_ebcdic_forwards_layout_and_collects() {
  FakeEbcdicService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_ebcdic_document(
      server.channel(), R"({"records": []})", "\xC1\xC2\xC3");
  require(outcome.success, "ebcdic collection succeeds: " + outcome.error);
  require(outcome.document.texts(0).text().base().text() == "from ebcdic",
          "the ebcdic Document arrives unchanged");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] ==
                  "WARNING_CODE_TRAILING_PARTIAL_RECORD: 7 bytes left over",
          "structured ebcdic warnings flatten with their code");
}

void verify_ebcdic_without_layout_never_dials() {
  // No server behind the channel: the layout check must fire first.
  const auto channel = grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
  const auto outcome = grparse::collect_ebcdic_document(channel, "", "\xC1");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "a missing layout is the caller's error, reported before dialing");
  require(outcome.error.find("ebcdic_layout_json") != std::string::npos,
          "the error names the option that was missing");
}

// ---- epub ------------------------------------------------------------------

class FakeEpubService final : public epubv1::EpubParseService::Service {
 public:
  grpc::Status ParseEpub(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<epubv1::ParseEpubResponse, epubv1::ParseEpubRequest>*
          stream) override {
    epubv1::ParseEpubRequest request;
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
                          "fake epub expects emit_document and bytes");
    }
    epubv1::ParseEpubResponse event;
    *event.mutable_document() = canned_document("epub");
    stream->Write(event);
    event.Clear();
    auto* warning = event.mutable_status()->add_warnings();
    warning->set_code(epubv1::PARSE_WARNING_CODE_UNSPECIFIED);
    warning->set_message("odd entry");
    warning->set_href("OEBPS/x.bin");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Finishes the stream with only a trailer: an epub collector that predates
// emit_document.
class DocumentlessEpubService final : public epubv1::EpubParseService::Service {
 public:
  grpc::Status ParseEpub(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<epubv1::ParseEpubResponse, epubv1::ParseEpubRequest>*
          stream) override {
    epubv1::ParseEpubRequest request;
    while (stream->Read(&request)) {
    }
    epubv1::ParseEpubResponse event;
    event.mutable_status()->set_chapters_emitted(2);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_epub_collects_document() {
  FakeEpubService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_epub_document(server.channel(), "PK\x03\x04zip");
  require(outcome.success, "epub collection succeeds: " + outcome.error);
  require(outcome.document.texts(0).text().base().text() == "from epub",
          "the epub Document arrives unchanged");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] ==
                  "PARSE_WARNING_CODE_UNSPECIFIED: odd entry (OEBPS/x.bin)",
          "epub warnings flatten with code and href");
}

void verify_missing_document_event_fails() {
  DocumentlessEpubService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_epub_document(server.channel(), "PK\x03\x04zip");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a trailer without a document event fails the collector");
  require(outcome.error.find("emit_document") != std::string::npos,
          "the failure explains the collector predates emit_document");
}

}  // namespace

int main() {
  try {
    verify_asr_collects_document();
    verify_transport_class_collapses_to_unavailable();
    verify_email_collects_document_and_warnings();
    verify_missing_trailer_fails();
    verify_xml_collects_document_and_formats_warnings();
    verify_caller_status_classes_survive();
    verify_ebcdic_forwards_layout_and_collects();
    verify_ebcdic_without_layout_never_dials();
    verify_epub_collects_document();
    verify_missing_document_event_fails();
  } catch (const std::exception& failure) {
    std::cerr << "FAILED: " << failure.what() << std::endl;
    return 1;
  }
  std::cout << "document collectors test passed" << std::endl;
  return 0;
}
