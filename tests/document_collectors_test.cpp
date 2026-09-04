// Exercises the Document-emitting collector clients against fake in-process
// services, so the option forwarding (emit_document, the asr model, the
// ebcdic layout), the upload chunk contracts, the document capture, the
// warning surfacing, and the failure paths are proven without any collector
// binary.

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "ai/pipestream/markup/v1/markup_service.grpc.pb.h"
#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "ai/pipestream/poi/v1/poi_service.grpc.pb.h"
#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "calamine/v1/calamine_service.grpc.pb.h"
#include "fastwarc/v1/warc_service.grpc.pb.h"
#include "grparse/document_collectors.h"
#include "grparse/document_parser_service.h"
#include "lolhtml/v1/lolhtml_service.grpc.pb.h"
#include "support/check.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace calaminev1 = calamine::v1;
namespace docv1 = ai::pipestream::document::v1;
namespace ebcdicv1 = ai::pipestream::ebcdic::v1;
namespace emailv1 = ai::pipestream::email::v1;
namespace epubv1 = ai::pipestream::epub::v1;
namespace lolv1 = lolhtml::v1;
namespace markupv1 = ai::pipestream::markup::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;
namespace poiv1 = ai::pipestream::poi::v1;
namespace warcv1 = fastwarc::v1;
namespace xmlv1 = ai::pipestream::xml::v1;

namespace {

using grparse_test::require;

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
      grparse::collect_asr_document(server.channel(), "small", "clip.wav",
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
  const auto outcome = grparse::collect_asr_document(server.channel(), "small", "clip.wav", "abc");
  require(!outcome.success, "an INTERNAL collector failure is an outcome");
  require(outcome.code == grpc::StatusCode::UNAVAILABLE,
          "non-caller status classes collapse to UNAVAILABLE");
  require(outcome.error.contains("decoder blew up"),
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
  require(outcome.error.contains("terminal status"),
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
  require(outcome.error.contains("sniff found nothing"),
          "the collector's own message survives");
}

// Never answers: the leg's own deadline is the only thing that can end a
// call to it.
class HangingXmlService final : public xmlv1::XmlParseService::Service {
 public:
  grpc::Status ParseXml(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<xmlv1::ParseXmlResponse, xmlv1::ParseXmlRequest>* stream)
      override {
    xmlv1::ParseXmlRequest request;
    while (stream->Read(&request)) {
    }
    while (!context->IsCancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return grpc::Status::OK;
  }
};

// The inbound call's deadline reaches the leg's ClientContext, not just the
// helper that computes it: against a collector that never answers, the call
// ends on the caller's deadline instead of sitting out this leg's own
// five-minute ceiling.
void verify_inbound_deadline_bounds_a_hanging_collector() {
  HangingXmlService service;
  ServerFixture server(&service);
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = grparse::collect_xml_document(
      server.channel(), "<a/>", std::chrono::system_clock::now() + std::chrono::milliseconds{300});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(!outcome.success && outcome.code == grpc::StatusCode::DEADLINE_EXCEEDED,
          "a hanging collector ends on the inbound deadline");
  require(elapsed < std::chrono::seconds{30},
          "the leg answers on the inbound deadline, not on its own ceiling");
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
  require(outcome.error.contains("ebcdic_layout_json"),
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
  require(outcome.error.contains("emit_document"),
          "the failure explains the collector predates emit_document");
}

// Streams the epub wire the way the collector does for a two-chapter book:
// the chapters' XHTML and the image bytes as typed events, then the
// skeleton Document (empty chapter groups, a picture by reference), then
// the status trailer.
class BookEpubService final : public epubv1::EpubParseService::Service {
 public:
  grpc::Status ParseEpub(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<epubv1::ParseEpubResponse, epubv1::ParseEpubRequest>*
          stream) override {
    epubv1::ParseEpubRequest request;
    while (stream->Read(&request)) {
    }
    epubv1::ParseEpubResponse event;
    auto* first = event.mutable_chapter();
    first->set_spine_index(0);
    first->set_href("OPS/ch01.xhtml");
    first->set_media_type("application/xhtml+xml");
    first->set_content("<html><body><h1>One</h1><img src=\"images/a.jpg\"/></body></html>");
    stream->Write(event);
    event.Clear();
    auto* image = event.mutable_resource();
    image->set_href("OPS/images/a.jpg");
    image->set_media_type("image/jpeg");
    image->set_kind(epubv1::RESOURCE_KIND_IMAGE);
    image->set_content("JPEGBYTES");
    stream->Write(event);
    event.Clear();
    auto* second = event.mutable_chapter();
    second->set_spine_index(1);
    second->set_href("OPS/ch02.xhtml");
    second->set_media_type("application/xhtml+xml");
    second->set_content("<html><body><h1>Two</h1></body></html>");
    stream->Write(event);
    event.Clear();
    auto* svg = event.mutable_chapter();
    svg->set_spine_index(2);
    svg->set_href("OPS/plate.svg");
    svg->set_media_type("image/svg+xml");
    svg->set_content("<svg/>");
    stream->Write(event);
    event.Clear();

    docv1::Document skeleton;
    skeleton.mutable_body()->set_self_ref("#/body");
    skeleton.mutable_furniture()->set_self_ref("#/furniture");
    skeleton.mutable_source_meta()->set_title("The Book");
    for (const auto* href : {"OPS/ch01.xhtml", "OPS/ch02.xhtml", "OPS/plate.svg"}) {
      auto* group = skeleton.add_groups();
      group->set_self_ref("#/groups/" + std::to_string(skeleton.groups_size() - 1));
      group->mutable_parent()->set_ref("#/body");
      group->set_label(docv1::GROUP_LABEL_CHAPTER);
      group->set_name(href);
      skeleton.mutable_body()->add_children()->set_ref(group->self_ref());
    }
    auto* picture = skeleton.add_pictures();
    picture->set_self_ref("#/pictures/0");
    picture->mutable_parent()->set_ref("#/body");
    picture->mutable_image()->set_mimetype("image/jpeg");
    picture->mutable_image()->set_uri("epub:OPS/images/a.jpg");
    picture->add_source()->mutable_collector()->set_collector("epub");
    skeleton.mutable_body()->add_children()->set_ref("#/pictures/0");
    *event.mutable_document() = skeleton;
    stream->Write(event);
    event.Clear();
    event.mutable_status()->set_chapters_emitted(3);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Parses the XHTML the book fake hands out: expects the HTML hint, and
// projects a heading per <h1> and a picture per <img src>, so the fold's
// href resolution and placement are exercised end to end. Records every
// dial so the test can prove one leg per XHTML chapter.
class HtmlMarkupService final : public markupv1::MarkupParseService::Service {
 public:
  grpc::Status ParseMarkup(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<markupv1::ParseMarkupResponse,
                               markupv1::ParseMarkupRequest>* stream) override {
    markupv1::ParseMarkupRequest request;
    markupv1::MarkupFormat format = markupv1::MARKUP_FORMAT_UNSPECIFIED;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        format = request.options().format();
      } else {
        bytes += request.chunk();
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dials_.push_back(bytes);
    }
    if (format != markupv1::MARKUP_FORMAT_HTML) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "book chapters must be dialed with the HTML hint");
    }
    docv1::Document document;
    document.mutable_body()->set_self_ref("#/body");
    document.mutable_furniture()->set_self_ref("#/furniture");
    document.mutable_source_meta()->set_title("chapter page title");
    const size_t h1 = bytes.find("<h1>");
    if (h1 != std::string::npos) {
      auto* base = document.add_texts()->mutable_section_header()->mutable_base();
      base->set_self_ref("#/texts/0");
      base->mutable_parent()->set_ref("#/body");
      base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
      base->set_text(bytes.substr(h1 + 4, bytes.find("</h1>") - h1 - 4));
      base->add_source()->mutable_collector()->set_collector("markup");
      document.mutable_body()->add_children()->set_ref("#/texts/0");
    }
    const size_t src = bytes.find("src=\"");
    if (src != std::string::npos) {
      auto* picture = document.add_pictures();
      picture->set_self_ref("#/pictures/0");
      picture->mutable_parent()->set_ref("#/body");
      picture->mutable_image()->set_mimetype("image/unknown");
      picture->mutable_image()->set_uri(
          bytes.substr(src + 5, bytes.find('"', src + 5) - src - 5));
      picture->add_source()->mutable_collector()->set_collector("markup");
      document.mutable_body()->add_children()->set_ref("#/pictures/0");
    }
    markupv1::ParseMarkupResponse event;
    *event.mutable_document() = document;
    stream->Write(event);
    event.Clear();
    event.mutable_status();
    stream->Write(event);
    return grpc::Status::OK;
  }

  std::vector<std::string> dials() {
    std::lock_guard<std::mutex> lock(mutex_);
    return dials_;
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> dials_;
};

void verify_epub_book_folds_chapters_and_images() {
  BookEpubService epub;
  ServerFixture epub_server(&epub);
  HtmlMarkupService markup;
  ServerFixture markup_server(&markup);
  const auto outcome = grparse::collect_epub_book(epub_server.channel(),
                                                  markup_server.channel(), "PK\x03\x04zip");
  require(outcome.success, "the book collects: " + outcome.error);
  require(markup.dials().size() == 2,
          "the markup collector is dialed once per XHTML chapter and never for the SVG");

  const auto& book = outcome.document;
  require(book.groups_size() == 3 && book.texts_size() == 2 && book.pictures_size() == 1,
          "the book holds the skeleton's groups, both headings, and one picture");
  require(book.groups(0).children_size() == 2 && book.groups(1).children_size() == 1 &&
              book.groups(2).children_size() == 0,
          "chapter one holds its heading and picture, chapter two its heading, the SVG nothing");
  require(book.texts(0).section_header().base().text() == "One" &&
              book.texts(0).section_header().base().parent().ref() == "#/groups/0" &&
              book.texts(1).section_header().base().text() == "Two" &&
              book.texts(1).section_header().base().parent().ref() == "#/groups/1",
          "each heading sits under its own chapter group");
  const auto& picture = book.pictures(0);
  require(picture.parent().ref() == "#/groups/0",
          "the image sits in the chapter that references it, not at the body");
  require(picture.image().mimetype() == "image/jpeg" &&
              picture.image().uri().starts_with("data:image/jpeg;base64,"),
          "the image is inlined under the manifest's media type");
  require(book.body().children_size() == 3,
          "the body lists the three chapter groups and no orphaned picture");
  require(book.source_meta().title() == "The Book",
          "a chapter's page title never overrides the book's");
  bool svg_noted = false;
  for (const auto& warning : outcome.warnings) {
    if (warning.contains("OPS/plate.svg") && warning.contains("not XHTML")) svg_noted = true;
  }
  require(svg_noted, "the SVG spine item is reported, not silently skipped");
}

void verify_epub_book_without_markup_keeps_the_skeleton() {
  BookEpubService epub;
  ServerFixture epub_server(&epub);
  const auto outcome = grparse::collect_epub_book(epub_server.channel(), nullptr, "PK\x03\x04zip");
  require(outcome.success, "the skeleton still collects without markup: " + outcome.error);
  require(outcome.document.texts_size() == 0 && outcome.document.groups_size() == 3,
          "the chapter groups stay empty");
  require(outcome.document.pictures(0).image().uri() == "epub:OPS/images/a.jpg",
          "without the fold the picture keeps its reference");
  bool named = false;
  for (const auto& warning : outcome.warnings) {
    if (warning.contains("GRPARSE_MARKUP_TARGET")) named = true;
  }
  require(named, "the degradation names the variable that would fix it");
}

// ---- markup ----------------------------------------------------------------

// Succeeds only when the client forwarded emit_document, the format hint the
// caller's filename resolves to, and the payload bytes.
class FakeMarkupService final : public markupv1::MarkupParseService::Service {
 public:
  grpc::Status ParseMarkup(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<markupv1::ParseMarkupResponse,
                               markupv1::ParseMarkupRequest>* stream) override {
    markupv1::ParseMarkupRequest request;
    markupv1::MarkupFormat format = markupv1::MARKUP_FORMAT_UNSPECIFIED;
    bool emit_document = false;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        format = request.options().format();
        emit_document = request.options().emit_document();
      } else {
        bytes += request.chunk();
      }
    }
    if (format != markupv1::MARKUP_FORMAT_MARKDOWN || !emit_document ||
        bytes.empty()) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          "fake markup expects the markdown hint, emit_document, and bytes");
    }
    markupv1::ParseMarkupResponse event;
    *event.mutable_document() = canned_document("markup");
    stream->Write(event);
    event.Clear();
    auto* warning = event.mutable_status()->add_warnings();
    warning->set_code(markupv1::WARNING_CODE_EMBEDDED_HTML_FLATTENED);
    warning->set_message("raw <div> flattened");
    warning->set_count(3);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_markup_forwards_hint_and_collects() {
  FakeMarkupService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_markup_document(
      server.channel(), "notes.md", "", "# Title\n\nBody.\n");
  require(outcome.success, "markup collection succeeds: " + outcome.error);
  require(outcome.document.texts(0).text().base().text() == "from markup",
          "the markup Document arrives unchanged");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] ==
                  "WARNING_CODE_EMBEDDED_HTML_FLATTENED: raw <div> flattened (x3)",
          "markup warnings flatten with code and count");
}

void verify_epub_book_survives_a_failing_chapter() {
  BookEpubService epub;
  ServerFixture epub_server(&epub);
  FakeMarkupService markdown_only;  // rejects the HTML hint
  ServerFixture markup_server(&markdown_only);
  const auto outcome = grparse::collect_epub_book(epub_server.channel(),
                                                  markup_server.channel(), "PK\x03\x04zip");
  require(outcome.success, "a chapter the markup collector rejects never sinks the book");
  require(outcome.document.texts_size() == 0, "the rejected chapters contribute nothing");
  int reported = 0;
  for (const auto& warning : outcome.warnings) {
    if (warning.contains("could not be parsed by the markup collector")) reported++;
  }
  require(reported == 2, "each rejected chapter is reported");
}

// ---- lol-html --------------------------------------------------------------

// The lol-html wire carries no document event, so unlike every other fake
// this one serves raw match events and the assertions land on the client's
// own fold.
class FakeLolHtmlService final : public lolv1::LolHtmlService::Service {
 public:
  enum class Mode { kOk, kError, kPageIdentity };
  explicit FakeLolHtmlService(Mode mode) : mode_(mode) {}

  grpc::Status Extract(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<lolv1::ExtractResponse, lolv1::ExtractRequest>*
          stream) override {
    lolv1::ExtractRequest request;
    lolv1::ExtractOptions options;
    bool options_seen = false;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_options()) {
        options = request.options();
        options_seen = true;
      } else {
        bytes += request.chunk();
      }
    }
    if (!options_seen || options.rules_size() != 2 ||
        options.rules(0).id() != "links" ||
        options.rules(0).selector() != "a[href]" ||
        options.rules(0).captures_size() != 3 || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake lol-html expects the JSON-decoded rules and bytes");
    }
    lolv1::ExtractResponse event;
    event.mutable_started()->set_rule_count(2);
    stream->Write(event);
    event.Clear();
    if (mode_ == Mode::kError) {
      auto* error = event.mutable_error();
      error->set_code(lolv1::PARSE_ERROR_CODE_PARSING_AMBIGUITY);
      error->set_message("refused to guess");
      stream->Write(event);
      return grpc::Status::OK;
    }
    if (mode_ == Mode::kPageIdentity) {
      // The page's own identity, exactly as the wire types it: the html
      // language, the canonical link, two meta pairs in their two spellings,
      // the title element and its text, and an anchor whose href sits behind
      // a non-ASCII attribute value.
      write_element(stream, "page", "html", {{"lang", "en-GB"}});
      write_element(stream, "page", "link",
                    {{"rel", "alternate canonical"},
                     {"href", "https://example.com/canonical"}});
      write_element(stream, "page", "meta",
                    {{"name", "description"}, {"content", "A na\xC3\xAFve page"}});
      write_element(stream, "page", "meta",
                    {{"property", "og:title"}, {"content", "Na\xC3\xAFve"}});
      write_element(stream, "page", "title", {});
      write_text(stream, "page", "Na\xC3\xAFve Example", lolv1::TEXT_TYPE_RCDATA);
      write_element(stream, "links", "a",
                    {{"title", "na\xC3\xAFve \xE2\x98\x83"}, {"href", "/na\xC3\xAFve"}});
      write_text(stream, "links", "About us", lolv1::TEXT_TYPE_DATA);
      auto* done = event.mutable_finished();
      done->set_bytes_parsed(bytes.size());
      (*done->mutable_matches_by_rule())["links"] = 1;
      (*done->mutable_matches_by_rule())["page"] = 5;
      stream->Write(event);
      return grpc::Status::OK;
    }
    auto* element = event.mutable_element();
    element->set_rule_id("links");
    element->set_tag_name("a");
    auto* attribute = element->add_attributes();
    attribute->set_name("href");
    attribute->set_value("/about");
    stream->Write(event);
    event.Clear();
    auto* text = event.mutable_text();
    text->set_rule_id("links");
    text->set_text("About us");
    text->set_text_type(lolv1::TEXT_TYPE_DATA);
    stream->Write(event);
    event.Clear();
    auto* finished = event.mutable_finished();
    finished->set_bytes_parsed(bytes.size());
    (*finished->mutable_matches_by_rule())["links"] = 1;
    (*finished->mutable_matches_by_rule())["headings"] = 0;
    stream->Write(event);
    return grpc::Status::OK;
  }

 private:
  using Writer = grpc::ServerReaderWriter<lolv1::ExtractResponse, lolv1::ExtractRequest>;

  static void write_element(
      Writer* stream, const std::string& rule, const std::string& tag,
      const std::vector<std::pair<std::string, std::string>>& attributes) {
    lolv1::ExtractResponse event;
    auto* element = event.mutable_element();
    element->set_rule_id(rule);
    element->set_tag_name(tag);
    for (const auto& [name, value] : attributes) {
      auto* attribute = element->add_attributes();
      attribute->set_name(name);
      attribute->set_name_raw(name);
      attribute->set_value(value);
    }
    stream->Write(event);
  }

  static void write_text(Writer* stream, const std::string& rule,
                         const std::string& text, lolv1::TextType type) {
    lolv1::ExtractResponse event;
    auto* node = event.mutable_text();
    node->set_rule_id(rule);
    node->set_text(text);
    node->set_text_type(type);
    node->set_last_in_node(true);
    stream->Write(event);
  }

  Mode mode_;
};

constexpr const char* kLolHtmlOptionsJson =
    R"({"rules":[{"id":"links","selector":"a[href]",)"
    R"("captures":["CAPTURE_TAG_NAME","CAPTURE_ATTRIBUTES","CAPTURE_TEXT"]},)"
    R"({"id":"headings","selector":"h2","captures":["CAPTURE_TEXT"]}]})";

void verify_lol_html_forwards_rules_and_folds() {
  FakeLolHtmlService service(FakeLolHtmlService::Mode::kOk);
  ServerFixture server(&service);
  const auto outcome = grparse::collect_lol_html_document(
      server.channel(), kLolHtmlOptionsJson,
      "<a href=\"/about\">About us</a>");
  require(outcome.success, "lol-html collection succeeds: " + outcome.error);
  require(outcome.document.groups_size() == 1 &&
              outcome.document.groups(0).name() == "links" &&
              outcome.document.groups(0).parent().ref() == "#/body",
          "matches fold into one group per rule, parented to the body");
  require(outcome.document.texts_size() == 2 &&
              outcome.document.texts(0).text().base().text() ==
                  "<a href=\"/about\">" &&
              outcome.document.texts(1).text().base().text() == "About us",
          "the element match and its text fold in arrival order");
  require(outcome.document.texts(0).text().base().parent().ref() == "#/groups/0" &&
              outcome.document.groups(0).children_size() == 2,
          "folded items parent onto their rule's group reciprocally");
  require(outcome.document.texts(0).text().base().source(0).collector().collector() ==
              "lol-html",
          "folded items carry the lol-html collector source");
  const auto& anchor = outcome.document.texts(0).text().base();
  require(anchor.spans_size() == 1 && anchor.spans(0).hyperlink() == "/about" &&
              anchor.spans(0).range().start() == 9 &&
              anchor.spans(0).range().end() == 15,
          "the typed href becomes a hyperlink run over its own characters");
  require(anchor.hyperlink() == "/about",
          "the first link also fills the item's primary hyperlink slot");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] == "rule 'headings' matched nothing",
          "a zero-match rule surfaces as a warning");
}

void verify_lol_html_captures_page_identity() {
  FakeLolHtmlService service(FakeLolHtmlService::Mode::kPageIdentity);
  ServerFixture server(&service);
  const auto outcome = grparse::collect_lol_html_document(
      server.channel(), kLolHtmlOptionsJson, "<html lang=\"en-GB\">");
  require(outcome.success, "lol-html collection succeeds: " + outcome.error);
  require(outcome.document.origin().web().canonical_uri() ==
              "https://example.com/canonical",
          "a rel=canonical link reaches the web provenance, tokens beside it "
          "notwithstanding");
  require(outcome.document.source_meta().language() == "en-GB",
          "the html lang attribute becomes the document's declared language");
  require(outcome.document.source_meta().title() == "Na\xC3\xAFve Example" &&
              outcome.document.name() == "Na\xC3\xAFve Example",
          "the title element's text names the document");
  require(outcome.document.meta_tags_size() == 3 &&
              outcome.document.meta_tags(0).name() == "description" &&
              outcome.document.meta_tags(0).content() == "A na\xC3\xAFve page" &&
              outcome.document.meta_tags(1).name() == "og:title" &&
              outcome.document.meta_tags(2).name() == "title" &&
              outcome.document.meta_tags(2).content() == "Na\xC3\xAFve Example",
          "meta pairs land under whichever key spelling the page wrote, and "
          "the title lands beside them");

  // The anchor's pseudo-tag: `<a title="naïve ☃" href="/naïve">`. The href
  // value spans code points 25 to 31; a byte count would say 28 to 35.
  const auto& anchor = outcome.document.texts(6).text().base();
  require(anchor.text() ==
              "<a title=\"na\xC3\xAFve \xE2\x98\x83\" href=\"/na\xC3\xAFve\">",
          "the anchor folds as its verbatim pseudo-tag");
  require(anchor.spans_size() == 1 && anchor.spans(0).hyperlink() == "/na\xC3\xAFve" &&
              anchor.spans(0).range().start() == 25 &&
              anchor.spans(0).range().end() == 31,
          "hyperlink runs are code-point ranges, unshifted by a multi-byte "
          "attribute value before them");
}

void verify_lol_html_without_rules_never_dials() {
  const auto outcome =
      grparse::collect_lol_html_document(nullptr, "", "<p>hi</p>");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "missing lol_html_options_json degrades before dialing");
  const auto garbled =
      grparse::collect_lol_html_document(nullptr, "not json", "<p>hi</p>");
  require(!garbled.success && garbled.code == grpc::StatusCode::INVALID_ARGUMENT &&
              garbled.error.contains("ExtractOptions"),
          "unparseable options degrade before dialing, naming the type");
}

void verify_lol_html_in_band_error_is_terminal() {
  FakeLolHtmlService service(FakeLolHtmlService::Mode::kError);
  ServerFixture server(&service);
  const auto outcome = grparse::collect_lol_html_document(
      server.channel(), kLolHtmlOptionsJson, "<select><xmp><script>");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT &&
              outcome.error.contains("PARSE_ERROR_CODE_PARSING_AMBIGUITY"),
          "the in-band terminal error fails the outcome with its typed code");
}

// ---- fastwarc --------------------------------------------------------------

// The fastwarc wire, like lol-html's, carries no document event and no
// terminal status: the fake serves canned record events and the assertions
// land on the client's own fold.
class FakeWarcService final : public warcv1::WarcService::Service {
 public:
  enum class Mode {
    kOk,
    kFramingError,
    kTransportError,
    kTruncated,
    kRequestBeforeResponse
  };
  explicit FakeWarcService(Mode mode) : mode_(mode) {}

  grpc::Status ParseWarc(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<warcv1::ParseWarcResponse, warcv1::ParseWarcRequest>*
          stream) override {
    warcv1::ParseWarcRequest request;
    warcv1::ParseWarcConfig config;
    std::string bytes;
    while (stream->Read(&request)) {
      if (request.has_config()) {
        config = request.config();
      } else {
        bytes += request.chunk();
      }
    }
    if (!config.parse_http() || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake fastwarc expects http parsing and archive bytes");
    }
    if (mode_ == Mode::kTransportError) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "socket went away");
    }
    warcv1::ParseWarcResponse event;
    if (mode_ == Mode::kTruncated) {
      // One textual record past the 64 KiB fold cap, in two chunks.
      auto* start = event.mutable_record_start()->mutable_metadata();
      start->set_record_type(warcv1::WARC_RECORD_TYPE_RESPONSE);
      start->set_stream_pos(0);
      start->set_content_length(71680);
      start->set_is_http(true);
      start->set_http_content_type("text/plain");
      stream->Write(event);
      event.Clear();
      auto* chunk = event.mutable_payload_chunk();
      chunk->set_offset(0);
      chunk->set_data(std::string(65536, 'a'));
      stream->Write(event);
      event.Clear();
      chunk = event.mutable_payload_chunk();
      chunk->set_offset(65536);
      chunk->set_data(std::string(6144, 'b'));
      stream->Write(event);
      event.Clear();
      event.mutable_record_end()->set_payload_length(71680);
      stream->Write(event);
      return grpc::Status::OK;
    }
    if (mode_ == Mode::kRequestBeforeResponse) {
      // The request record precedes its response and declares the same
      // target: the fold must let the response supersede it.
      auto* asked = event.mutable_record_start()->mutable_metadata();
      asked->set_record_index(0);
      asked->set_record_type(warcv1::WARC_RECORD_TYPE_REQUEST);
      asked->set_stream_pos(0);
      add_header(asked->mutable_warc_headers(), "WARC-Target-URI",
                 "https://example.com/page");
      stream->Write(event);
      event.Clear();
      event.mutable_record_end()->set_record_index(0);
      stream->Write(event);
      event.Clear();
    }
    // The response record, with both lossless header blocks the server
    // transmits.
    auto* start = event.mutable_record_start()->mutable_metadata();
    start->set_record_index(mode_ == Mode::kRequestBeforeResponse ? 1 : 0);
    start->set_record_type(warcv1::WARC_RECORD_TYPE_RESPONSE);
    start->set_stream_pos(0);
    start->set_content_length(23);
    start->set_is_http(true);
    start->set_http_parsed(true);
    start->set_http_content_type("text/html");
    start->set_record_id("<urn:uuid:test-1>");
    start->mutable_record_date()->set_seconds(1704164645);  // 2024-01-02T03:04:05Z
    auto* warc_headers = start->mutable_warc_headers();
    warc_headers->set_encoding(warcv1::HEADER_ENCODING_UNICODE);
    add_header(warc_headers, "WARC-Type", "response");
    add_header(warc_headers, "WARC-Target-URI", "https://example.com/page");
    add_header(warc_headers, "WARC-Payload-Digest", "sha1:PAYLOAD");
    add_header(warc_headers, "WARC-Block-Digest", "sha1:BLOCK");
    auto* http_headers = start->mutable_http_headers();
    http_headers->set_encoding(warcv1::HEADER_ENCODING_LATIN1);
    http_headers->set_status_line("HTTP/1.1 200 OK");
    add_header(http_headers, "Content-Type", "text/html; charset=utf-8");
    add_header(http_headers, "Last-Modified", "Tue, 02 Jan 2024 03:00:00 GMT");
    add_header(http_headers, "Content-Language", "en-GB");
    add_header(http_headers, "ETag", "\"deadbeef\"");
    add_header(http_headers, "Server", "fake/1.0");
    stream->Write(event);
    event.Clear();
    auto* chunk = event.mutable_payload_chunk();
    chunk->set_offset(0);
    chunk->set_data("<html>hello");
    stream->Write(event);
    event.Clear();
    chunk = event.mutable_payload_chunk();
    chunk->set_offset(11);
    chunk->set_data(" warc</html>");
    stream->Write(event);
    event.Clear();
    event.mutable_record_end()->set_payload_length(23);
    stream->Write(event);
    event.Clear();
    if (mode_ == Mode::kRequestBeforeResponse) return grpc::Status::OK;
    auto* skipped = event.mutable_record_error();
    skipped->set_stream_pos(64);
    skipped->set_recoverable(true);
    skipped->set_message("bad http headers");
    stream->Write(event);
    event.Clear();
    if (mode_ == Mode::kFramingError) {
      // Terminal by contract: the server sends this last and ends OK.
      auto* fatal = event.mutable_record_error();
      fatal->set_stream_pos(128);
      fatal->set_recoverable(false);
      fatal->set_message("warc framing lost");
      stream->Write(event);
      return grpc::Status::OK;
    }
    // Second record: a warcinfo with no HTTP metadata and no target URI, so
    // it must not disturb the web provenance the response established.
    auto* meta = event.mutable_record_start()->mutable_metadata();
    meta->set_record_index(1);
    meta->set_record_type(warcv1::WARC_RECORD_TYPE_WARCINFO);
    meta->set_stream_pos(100);
    meta->set_content_length(14);
    stream->Write(event);
    event.Clear();
    chunk = event.mutable_payload_chunk();
    chunk->set_offset(0);
    chunk->set_data("software: fake");
    stream->Write(event);
    event.Clear();
    event.mutable_record_end()->set_payload_length(14);
    stream->Write(event);
    return grpc::Status::OK;
  }

 private:
  static void add_header(warcv1::HeaderBlock* block, const std::string& name,
                         const std::string& value) {
    auto* field = block->add_fields();
    field->set_name(name);
    field->set_value(value);
  }

  Mode mode_;
};

void verify_fastwarc_folds_records_and_warnings() {
  FakeWarcService service(FakeWarcService::Mode::kOk);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success, "fastwarc collection succeeds: " + outcome.error);
  require(outcome.document.groups_size() == 2 &&
              outcome.document.groups(0).name() == "response @ 0" &&
              outcome.document.groups(0).parent().ref() == "#/body" &&
              outcome.document.groups(1).name() == "warcinfo @ 100",
          "records fold into one group per record, in stream order");
  const auto& texts = outcome.document.texts();
  require(texts.size() == 13, "every folded metadata and payload item lands");
  require(texts[0].text().base().text() == "warc-type: response" &&
              texts[0].text().base().parent().ref() == "#/groups/0" &&
              texts[0].text().base().source(0).collector().collector() == "fastwarc",
          "metadata items are source-tagged and parented to their record group");
  require(texts[2].text().base().text() == "record-id: <urn:uuid:test-1>",
          "the record id folds when declared");
  require(texts[3].text().base().text() == "record-date: 2024-01-02T03:04:05Z",
          "the record date folds as RFC 3339");
  require(texts[4].text().base().text() == "http-content-type: text/html",
          "the embedded HTTP content type folds");
  require(texts[5].text().base().text() == "content-length: 23" &&
              texts[6].text().base().text() == "payload-length: 23",
          "declared and streamed lengths fold");
  require(texts[7].text().base().text() == "<html>hello warc</html>",
          "payload chunks reassemble in offset order");
  require(outcome.document.groups(0).children_size() == 8,
          "the response group holds its items reciprocally");
  require(texts[8].text().base().text() == "warc-type: warcinfo" &&
              texts[12].text().base().text() == "software: fake",
          "a record with no HTTP metadata still folds its payload as text");
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] ==
                  "record at stream offset 64 skipped: bad http headers",
          "a recoverable record error becomes a warning");
}

void verify_fastwarc_captures_web_provenance() {
  FakeWarcService service(FakeWarcService::Mode::kOk);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success, "fastwarc collection succeeds: " + outcome.error);
  const auto& web = outcome.document.origin().web();
  require(web.target_uri() == "https://example.com/page",
          "WARC-Target-URI reaches the document's web provenance");
  require(web.crawl_time().seconds() == 1704164645 &&
              web.crawl_time_raw() == "2024-01-02T03:04:05Z",
          "the parsed WARC-Date lands typed, with the raw spelling beside it");
  require(web.http_status() == 200,
          "the status code parses out of the raw HTTP status line");
  require(web.content_language() == "en-GB",
          "the declared response language lands in its own field");
  const auto& headers = web.headers();
  require(headers.size() == 6, "only the selected response headers are kept");
  require(headers.at("content-type") == "text/html; charset=utf-8" &&
              headers.at("last-modified") == "Tue, 02 Jan 2024 03:00:00 GMT" &&
              headers.at("content-language") == "en-GB" &&
              headers.at("etag") == "\"deadbeef\"",
          "the selected response headers keep their values under lower-cased names");
  require(!headers.contains("server"),
          "a header nothing reads is not invented into the capture");
  require(headers.at("warc-payload-digest") == "sha1:PAYLOAD" &&
              headers.at("warc-block-digest") == "sha1:BLOCK",
          "the declared digests ride the header map under their WARC names");
}

void verify_fastwarc_response_supersedes_the_request_record() {
  FakeWarcService service(FakeWarcService::Mode::kRequestBeforeResponse);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success, "fastwarc collection succeeds: " + outcome.error);
  const auto& web = outcome.document.origin().web();
  require(web.target_uri() == "https://example.com/page",
          "the request record's target holds until the response arrives");
  require(web.http_status() == 200 && web.headers().contains("etag"),
          "the response record supersedes the request record's provenance");
}

void verify_fastwarc_framing_error_keeps_records() {
  FakeWarcService service(FakeWarcService::Mode::kFramingError);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success && outcome.document.groups_size() == 1,
          "a non-recoverable framing error keeps the records already folded");
  require(outcome.warnings.size() == 2 &&
              outcome.warnings[1].contains("framing error at stream offset 128"),
          "the framing error surfaces as a warning, not a failure");
}

void verify_fastwarc_transport_failure_without_records() {
  FakeWarcService service(FakeWarcService::Mode::kTransportError);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a transport failure before any record is a failed outcome");
  require(outcome.error.contains("socket went away"),
          "the transport message survives");
}

void verify_fastwarc_truncates_payload_text() {
  FakeWarcService service(FakeWarcService::Mode::kTruncated);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success && outcome.document.groups_size() == 1,
          "the oversized record still folds");
  const std::string suffix =
      "\n[fastwarc: payload truncated to the first 65536 of 71680 bytes]";
  const std::string& payload = outcome.document.texts(5).text().base().text();
  require(payload.size() == 65536 + suffix.size() &&
              payload.starts_with(std::string(16, 'a')) &&
              payload.ends_with(suffix),
          "the payload folds to the 64 KiB cap with the truncation noted");
}

}  // namespace

// ---- poi --------------------------------------------------------------------

namespace {

// Serves one canned typed stream: document info with a title, paragraphs in
// four styles, a body table, a sheet with a formula cell, a slide, an
// embedded object, and the terminal status with one warning.
class FakePoiService final : public poiv1::PoiParseService::Service {
 public:
  grpc::Status ParseDocument(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<poiv1::ParseEvent, poiv1::ParseRequestChunk>* stream)
      override {
    poiv1::ParseRequestChunk chunk;
    std::string document_id;
    std::string filename;
    std::string bytes;
    bool complete = false;
    while (stream->Read(&chunk)) {
      if (document_id.empty()) {
        document_id = chunk.document_id();
        filename = chunk.filename();
      }
      bytes += chunk.data();
      complete = chunk.complete();
    }
    if (document_id.empty() || filename.empty() || !complete || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake poi expects a complete identified upload");
    }

    poiv1::ParseEvent event;
    poiv1::DocumentInfo* info = event.mutable_document_info();
    info->set_document_id(document_id);
    info->set_format(poiv1::DOCUMENT_FORMAT_XLSX);
    info->mutable_metadata()->set_title("Quarterly Report");
    info->mutable_metadata()->set_author("Alice");
    info->mutable_metadata()->set_last_modified_by("Bob");
    stream->Write(event);

    event.Clear();
    event.mutable_paragraph()->set_text("Quarterly Report");
    event.mutable_paragraph()->set_style("Title");
    stream->Write(event);

    event.Clear();
    event.mutable_paragraph()->set_text("Overview");
    event.mutable_paragraph()->set_style("Heading1");
    stream->Write(event);

    event.Clear();
    event.mutable_paragraph()->set_text("body text");
    stream->Write(event);

    event.Clear();
    event.mutable_paragraph()->set_text("first point");
    event.mutable_paragraph()->set_style("ListParagraph");
    stream->Write(event);

    event.Clear();
    poiv1::Table* table = event.mutable_table();
    for (int row = 0; row < 2; ++row) {
      poiv1::TableRow* table_row = table->add_rows();
      table_row->add_cells()->set_text(row == 0 ? "h1" : "v1");
      table_row->add_cells()->set_text(row == 0 ? "h2" : "v2");
    }
    stream->Write(event);

    event.Clear();
    poiv1::Sheet* sheet = event.mutable_sheet();
    sheet->set_index(0);
    sheet->set_name("Data");
    poiv1::SheetRow* header = sheet->add_rows();
    header->set_row_index(0);
    header->add_cells()->set_text("Name");
    poiv1::SheetCell* header_score = header->add_cells();
    header_score->set_column_index(1);
    header_score->set_text("Score");
    poiv1::SheetRow* data = sheet->add_rows();
    data->set_row_index(1);
    poiv1::SheetCell* name = data->add_cells();
    name->set_text("a");
    poiv1::SheetCell* score = data->add_cells();
    score->set_column_index(1);
    score->set_formatted("84");
    score->set_number(84);
    score->set_formula("B1*2");
    stream->Write(event);

    event.Clear();
    poiv1::Slide* slide = event.mutable_slide();
    slide->set_index(0);
    slide->set_title("Intro");
    slide->add_texts("bullet");
    slide->add_notes("speaker note");
    stream->Write(event);

    event.Clear();
    poiv1::EmbeddedObject* object = event.mutable_embedded_object();
    object->set_id("ole1");
    object->set_filename("chart.xlsx");
    object->set_content_type("application/vnd.ms-excel");
    object->set_size_bytes(100);
    stream->Write(event);

    event.Clear();
    event.mutable_status()->set_state(poiv1::ParseStatus::STATE_OK);
    event.mutable_status()->add_warnings("header skipped");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

class RejectingPoiService final : public poiv1::PoiParseService::Service {
 public:
  grpc::Status ParseDocument(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<poiv1::ParseEvent, poiv1::ParseRequestChunk>* stream)
      override {
    poiv1::ParseRequestChunk chunk;
    while (stream->Read(&chunk)) {
    }
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "document exceeds the 70 MiB cap");
  }
};

// Ends the stream cleanly without the terminal ParseStatus.
class TruncatingPoiService final : public poiv1::PoiParseService::Service {
 public:
  grpc::Status ParseDocument(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<poiv1::ParseEvent, poiv1::ParseRequestChunk>* stream)
      override {
    poiv1::ParseRequestChunk chunk;
    while (stream->Read(&chunk)) {
    }
    poiv1::ParseEvent event;
    event.mutable_paragraph()->set_text("orphan");
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_poi_folds_typed_events() {
  FakePoiService service;
  ServerFixture server(&service);
  // Large enough to prove multi-chunk uploads reassemble.
  const std::string bytes(600U * 1024U, 'x');
  const auto outcome = grparse::collect_poi_document(
      server.channel(), "doc-7", "book.xlsx",
      "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", bytes);
  require(outcome.success, "poi collection succeeds: " + outcome.error);
  require(outcome.warnings.size() == 1 && outcome.warnings[0] == "header skipped",
          "poi status warnings surface verbatim");

  const docv1::Document& document = outcome.document;
  require(document.source_meta().title() == "Quarterly Report" &&
              document.source_meta().authors_size() == 1 &&
              document.source_meta().authors(0) == "Alice" &&
              document.source_meta().modified_by() == "Bob",
          "the document info folds into source_meta");

  require(document.texts_size() == 7,
          "title, heading, paragraph, list item, slide title, bullet, and note fold");
  const docv1::TextItemBase& title = document.texts(0).title().base();
  require(title.label() == docv1::DOC_ITEM_LABEL_TITLE &&
              title.text() == "Quarterly Report" && title.style_name() == "Title",
          "the Title style folds to a title item keeping the style name");
  const auto& heading = document.texts(1).section_header();
  require(heading.base().label() == docv1::DOC_ITEM_LABEL_SECTION_HEADER &&
              heading.level() == 1 && heading.base().text() == "Overview",
          "Heading1 folds to a level-1 section header");
  require(document.texts(2).text().base().label() == docv1::DOC_ITEM_LABEL_PARAGRAPH,
          "an unstyled paragraph folds to a paragraph item");
  require(document.texts(3).list_item().base().label() == docv1::DOC_ITEM_LABEL_LIST_ITEM &&
              document.texts(3).list_item().base().style_name() == "ListParagraph",
          "a list style folds to a list item");
  for (int i = 0; i < document.texts_size(); ++i) {
    const auto& item = document.texts(i);
    const docv1::TextItemBase* base = nullptr;
    if (item.has_title()) base = &item.title().base();
    if (item.has_section_header()) base = &item.section_header().base();
    if (item.has_list_item()) base = &item.list_item().base();
    if (item.has_text()) base = &item.text().base();
    require(base != nullptr && base->source_size() == 1 &&
                base->source(0).collector().collector() == "poi",
            "every poi item carries the poi collector source");
    require(base->self_ref() == "#/texts/" + std::to_string(i),
            "item refs are dense and local");
  }

  require(document.tables_size() == 2, "the body table and the sheet fold into tables");
  const docv1::TableData& body_table = document.tables(0).data();
  require(body_table.num_rows() == 2 && body_table.num_cols() == 2 &&
              body_table.table_cells_size() == 4 &&
              body_table.table_cells(3).text() == "v2",
          "the body table folds with its cells");
  require(document.tables(0).parent().ref() == "#/body" &&
              document.body().children(0).ref() == "#/texts/0",
          "the body table hangs off the body beside the texts");

  require(document.groups_size() == 2, "the sheet and the slide fold into groups");
  const docv1::GroupItem& sheet_group = document.groups(0);
  require(sheet_group.label() == docv1::GROUP_LABEL_SHEET &&
              sheet_group.name() == "Data" && sheet_group.sheet().index() == 0,
          "the sheet group carries the sheet identity");
  const docv1::TableData& sheet_table = document.tables(1).data();
  require(document.tables(1).parent().ref() == sheet_group.self_ref() &&
              sheet_group.children(0).ref() == document.tables(1).self_ref(),
          "the sheet table hangs off the sheet group, reciprocally");
  require(sheet_table.num_rows() == 2 && sheet_table.num_cols() == 2,
          "the sheet table sizes from the populated cells");
  const docv1::TableCell* formula_cell = nullptr;
  for (const auto& cell : sheet_table.table_cells()) {
    if (cell.start_row_offset_idx() == 1 && cell.start_col_offset_idx() == 1) {
      formula_cell = &cell;
    }
  }
  require(formula_cell != nullptr && formula_cell->text() == "84" &&
              formula_cell->value().formula() == "B1*2",
          "a formula cell keeps the formula and the cached value's display");
  require(sheet_table.row_prov_size() == 2 &&
              sheet_table.row_prov(1).grid().sheet() == "Data" &&
              sheet_table.row_prov(1).grid().row() == 1,
          "sheet rows carry grid provenance");

  const docv1::GroupItem& slide_group = document.groups(1);
  require(slide_group.label() == docv1::GROUP_LABEL_SLIDE &&
              slide_group.name() == "Intro",
          "the slide folds into its own group");
  const docv1::TextItemBase& slide_title = document.texts(4).section_header().base();
  require(slide_title.parent().ref() == slide_group.self_ref() &&
              slide_title.text() == "Intro",
          "the slide title heads its group");
  require(document.texts(6).text().base().content_layer() == docv1::CONTENT_LAYER_NOTES,
          "speaker notes land on the notes layer");

  require(document.attachments_size() == 1 &&
              document.attachments(0).id() == "ole1" &&
              document.attachments(0).name() == "chart.xlsx" &&
              document.attachments(0).media_type() == "application/vnd.ms-excel" &&
              document.attachments(0).size_bytes() == 100,
          "an embedded object registers as an attachment descriptor");
}

void verify_poi_collector_failure_survives_its_code() {
  RejectingPoiService service;
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_poi_document(server.channel(), "d", "big.docx", "", "bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::RESOURCE_EXHAUSTED,
          "the collector's byte-cap rejection keeps its status class");
  require(outcome.error.contains("70 MiB"), "the collector's message survives");
}

void verify_poi_truncated_stream_fails() {
  TruncatingPoiService service;
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_poi_document(server.channel(), "d", "cut.docx", "", "bytes");
  require(!outcome.success && outcome.error.contains("terminal status"),
          "a stream without ParseStatus is a failure, not an empty success");
}

void verify_poi_unreachable_endpoint_degrades() {
  const auto channel = grpc::CreateChannel("127.0.0.1:1",
                                           grpc::InsecureChannelCredentials());
  const auto outcome =
      grparse::collect_poi_document(channel, "d", "nowhere.docx", "", "bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "an unreachable poi collector degrades to UNAVAILABLE");
}

}  // namespace

// ---- calamine ---------------------------------------------------------------

namespace {

// The handle lifecycle: OpenWorkbook uploads and names two sheets,
// StreamWorksheetRange serves typed cells per sheet index, CloseWorkbook
// counts its calls so the tests can prove the handle is always released.
class FakeCalamineService final : public calaminev1::CalamineService::Service {
 public:
  grpc::Status OpenWorkbook(
      grpc::ServerContext*,
      grpc::ServerReader<calaminev1::OpenWorkbookRequest>* reader,
      calaminev1::OpenWorkbookResponse* response) override {
    calaminev1::OpenWorkbookRequest frame;
    bool options_seen = false;
    std::string bytes;
    while (reader->Read(&frame)) {
      if (frame.has_options()) {
        options_seen = true;
      } else {
        bytes += frame.chunk();
      }
    }
    if (!options_seen || bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake calamine expects options then bytes");
    }
    response->set_workbook_id("wb-1");
    response->set_detected_format(calaminev1::WORKBOOK_FORMAT_XLSX);
    calaminev1::Metadata* metadata = response->mutable_metadata();
    calaminev1::Sheet* first = metadata->add_sheets();
    first->set_name("First");
    first->set_typ(calaminev1::SHEET_TYPE_WORKSHEET);
    first->set_visible(calaminev1::SHEET_VISIBLE_VISIBLE);
    calaminev1::Sheet* second = metadata->add_sheets();
    second->set_name("Second");
    second->set_typ(calaminev1::SHEET_TYPE_WORKSHEET);
    second->set_visible(calaminev1::SHEET_VISIBLE_HIDDEN);
    calaminev1::DefinedName* name = metadata->add_defined_names();
    name->set_name("Answer");
    name->set_definition("First!$B$2");
    return grpc::Status::OK;
  }

  grpc::Status StreamWorksheetRange(
      grpc::ServerContext*, const calaminev1::StreamWorksheetRangeRequest* request,
      grpc::ServerWriter<calaminev1::StreamWorksheetRangeResponse>* writer) override {
    if (request->workbook_id() != "wb-1") {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown handle");
    }
    calaminev1::StreamWorksheetRangeResponse event;
    event.mutable_started()->set_sheet_name(
        request->sheet().sheet_index() == 0 ? "First" : "Second");
    writer->Write(event);
    if (request->sheet().sheet_index() == 0) {
      event.Clear();
      calaminev1::WorksheetRowBatch* batch = event.mutable_rows();
      calaminev1::WorksheetRow* header = batch->add_rows();
      header->set_row_index(0);
      header->add_values()->set_string_value("Name");
      header->add_values()->set_string_value("Score");
      // Row 1 is skipped entirely: the gap is the sheet's empty region.
      calaminev1::WorksheetRow* data = batch->add_rows();
      data->set_row_index(2);
      data->add_values()->set_int_value(7);
      data->add_values()->set_float_value(2.5);
      data->add_values()->set_bool_value(true);
      // 45943.5 is 2025-10-13 12:00:00 in the 1900 date system (the
      // contract's own example); 45000 is 2023-03-15 with no time of day.
      calaminev1::CellData* when = data->add_values();
      when->mutable_date_time()->set_value(45943.5);
      calaminev1::CellData* day = data->add_values();
      day->mutable_date_time()->set_value(45000);
      data->add_values()->set_error(calaminev1::CELL_ERROR_TYPE_DIV0);
      data->add_values()->mutable_empty();
      writer->Write(event);
    }
    return grpc::Status::OK;
  }

  grpc::Status CloseWorkbook(grpc::ServerContext*,
                             const calaminev1::CloseWorkbookRequest* request,
                             calaminev1::CloseWorkbookResponse* response) override {
    if (request->workbook_id() == "wb-1") {
      ++closed_;
      response->set_closed(true);
    }
    return grpc::Status::OK;
  }

  int closed() const { return closed_; }

 private:
  int closed_ = 0;
};

// Opens the handle, then fails the only sheet's read.
class FailingSheetCalamineService final : public calaminev1::CalamineService::Service {
 public:
  grpc::Status OpenWorkbook(
      grpc::ServerContext*,
      grpc::ServerReader<calaminev1::OpenWorkbookRequest>* reader,
      calaminev1::OpenWorkbookResponse* response) override {
    calaminev1::OpenWorkbookRequest frame;
    while (reader->Read(&frame)) {
    }
    response->set_workbook_id("wb-9");
    calaminev1::Sheet* sheet = response->mutable_metadata()->add_sheets();
    sheet->set_name("Broken");
    sheet->set_visible(calaminev1::SHEET_VISIBLE_VISIBLE);
    return grpc::Status::OK;
  }

  grpc::Status StreamWorksheetRange(
      grpc::ServerContext*, const calaminev1::StreamWorksheetRangeRequest*,
      grpc::ServerWriter<calaminev1::StreamWorksheetRangeResponse>*) override {
    return grpc::Status(grpc::StatusCode::INTERNAL, "sheet read blew up");
  }

  grpc::Status CloseWorkbook(grpc::ServerContext*,
                             const calaminev1::CloseWorkbookRequest*,
                             calaminev1::CloseWorkbookResponse* response) override {
    ++closed_;
    response->set_closed(true);
    return grpc::Status::OK;
  }

  int closed() const { return closed_; }

 private:
  int closed_ = 0;
};

void verify_calamine_folds_sheets() {
  FakeCalamineService service;
  ServerFixture server(&service);
  // Large enough to prove multi-chunk uploads reassemble.
  const std::string bytes(600U * 1024U, 'c');
  const auto outcome = grparse::collect_calamine_document(server.channel(), bytes);
  require(outcome.success, "calamine collection succeeds: " + outcome.error);
  require(service.closed() == 1, "the workbook handle is closed on success");

  const docv1::Document& document = outcome.document;
  require(document.groups_size() == 2 && document.tables_size() == 2,
          "each sheet folds into a group holding one table");
  require(document.groups(0).label() == docv1::GROUP_LABEL_SHEET &&
              document.groups(0).name() == "First" &&
              document.groups(0).content_layer() == docv1::CONTENT_LAYER_BODY,
          "a visible sheet folds onto the body layer");
  require(document.groups(1).name() == "Second" &&
              document.groups(1).content_layer() == docv1::CONTENT_LAYER_INVISIBLE,
          "a hidden sheet folds onto the invisible layer");
  require(document.named_ranges_size() == 1 &&
              document.named_ranges(0).name() == "Answer" &&
              document.named_ranges(0).expression() == "First!$B$2" &&
              document.named_ranges(0).kind() == "named",
          "defined names fold into named ranges");

  const docv1::TableData& data = document.tables(0).data();
  require(document.tables(0).source(0).collector().collector() == "calamine",
          "the sheet table carries the calamine collector source");
  require(data.num_rows() == 3 && data.num_cols() == 6,
          "the table sizes from the populated cells, gaps included");
  require(data.table_cells_size() == 8, "empty cells fold to nothing");

  const docv1::TableCell* cells[6] = {nullptr};
  for (const auto& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == 2 &&
        cell.start_col_offset_idx() < 6) {
      cells[cell.start_col_offset_idx()] = &cell;
    }
  }
  require(cells[0] != nullptr && cells[0]->text() == "7" &&
              cells[0]->value().number() == 7,
          "an int cell folds as a number");
  require(cells[1] != nullptr && cells[1]->text() == "2.5" &&
              cells[1]->value().number() == 2.5,
          "a float cell folds as a number with its shortest spelling");
  require(cells[2] != nullptr && cells[2]->text() == "TRUE" &&
              cells[2]->value().boolean(),
          "a bool cell folds as a boolean");
  require(cells[3] != nullptr && cells[3]->text() == "2025-10-13 12:00:00" &&
              cells[3]->value().datetime().year() == 2025 &&
              cells[3]->value().datetime().month() == 10 &&
              cells[3]->value().datetime().day() == 13 &&
              cells[3]->value().datetime().hour() == 12,
          "a serial datetime folds as a civil datetime");
  require(cells[4] != nullptr && cells[4]->text() == "2023-03-15" &&
              cells[4]->value().datetime().year() == 2023 &&
              cells[4]->value().datetime().month() == 3 &&
              cells[4]->value().datetime().day() == 15,
          "a whole-day serial folds as a date without a time");
  require(cells[5] != nullptr && cells[5]->text() == "#DIV/0!" &&
              cells[5]->value().error() == "#DIV/0!",
          "an error cell folds as its error literal");
  require(data.row_prov_size() == 2 && data.row_prov(1).grid().row() == 2 &&
              data.row_prov(1).grid().sheet() == "First",
          "rows carry grid provenance in the sheet's absolute addresses");
}

void verify_calamine_sheet_failure_still_closes() {
  FailingSheetCalamineService service;
  ServerFixture server(&service);
  const auto outcome = grparse::collect_calamine_document(server.channel(), "bytes");
  require(!outcome.success && outcome.error.contains("Broken"),
          "a sheet that produced nothing fails the leg naming the sheet");
  require(service.closed() == 1,
          "the workbook handle is closed even when the read fails");
}

void verify_calamine_unreachable_endpoint_degrades() {
  const auto channel = grpc::CreateChannel("127.0.0.1:1",
                                           grpc::InsecureChannelCredentials());
  const auto outcome = grparse::collect_calamine_document(channel, "bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "an unreachable calamine collector degrades to UNAVAILABLE");
}

}  // namespace

// ---- pdf --------------------------------------------------------------------

namespace {

// Serves the routing contract: an info event with a configurable
// classification, one page event for text-bearing documents, the folded
// document (emit_document is asserted on), and the status trailer.
class FakePdfService final : public pdfv1::PdfParseService::Service {
 public:
  FakePdfService(pdfv1::PdfType type, std::vector<uint32_t> pages_needing_ocr,
                 bool encoding_issues = false)
      : type_(type),
        pages_needing_ocr_(std::move(pages_needing_ocr)),
        encoding_issues_(encoding_issues) {}

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
                          "fake pdf expects emit_document and bytes");
    }
    pdfv1::ParsePdfResponse event;
    auto* info = event.mutable_info();
    info->set_pdf_type(type_);
    info->set_confidence(0.95F);
    info->set_page_count(3);
    for (const uint32_t page : pages_needing_ocr_) info->add_pages_needing_ocr(page);
    stream->Write(event);
    event.Clear();
    if (type_ == pdfv1::PDF_TYPE_TEXT_BASED || type_ == pdfv1::PDF_TYPE_MIXED) {
      auto* page = event.mutable_page();
      page->set_page_no(1);
      page->set_markdown("# page one");
      stream->Write(event);
      event.Clear();
    }
    *event.mutable_document() = canned_document("pdf");
    stream->Write(event);
    event.Clear();
    auto* status = event.mutable_status();
    status->set_pages_extracted(1);
    status->set_has_encoding_issues(encoding_issues_);
    auto* warning = status->add_warnings();
    warning->set_code(pdfv1::PARSE_WARNING_CODE_PASSWORD_FALLBACK);
    warning->set_message("extracted whole-document");
    stream->Write(event);
    return grpc::Status::OK;
  }

 private:
  pdfv1::PdfType type_;
  std::vector<uint32_t> pages_needing_ocr_;
  bool encoding_issues_;
};

class FailingPdfService final : public pdfv1::PdfParseService::Service {
 public:
  grpc::Status ParsePdf(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<pdfv1::ParsePdfResponse, pdfv1::ParsePdfRequest>* stream)
      override {
    pdfv1::ParsePdfRequest request;
    while (stream->Read(&request)) {
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "lopdf panicked");
  }
};

void verify_pdf_collects_document_classification_and_warnings() {
  FakePdfService service(pdfv1::PDF_TYPE_TEXT_BASED, {});
  ServerFixture server(&service);
  const auto result = grparse::collect_pdf(server.channel(), std::string(300U * 1024U, 'p'));
  require(result.outcome.success, "pdf collection succeeds: " + result.outcome.error);
  require(result.outcome.document.texts(0).text().base().text() == "from pdf",
          "the pdf Document arrives unchanged");
  require(result.classification.pdf_class == grparse::PdfClass::kTextBased,
          "the info event's classification is captured");
  require(result.classification.pages_needing_ocr.empty(),
          "a text-based document names no OCR pages");
  require(result.outcome.warnings.size() == 1 &&
              result.outcome.warnings[0] ==
                  "PARSE_WARNING_CODE_PASSWORD_FALLBACK: extracted whole-document",
          "trailer warnings flatten with their code");
  require(grparse::route_pdf_by_classification(result.classification).fast_path,
          "a fully text-based classification routes to the fast path");
}

void verify_pdf_scanned_reports_the_ocr_page_set() {
  FakePdfService service(pdfv1::PDF_TYPE_SCANNED, {1, 2, 3});
  ServerFixture server(&service);
  const auto result = grparse::collect_pdf(server.channel(), "%PDF-fake");
  require(result.outcome.success, "scanned pdf collection succeeds: " + result.outcome.error);
  require(result.classification.pdf_class == grparse::PdfClass::kScanned,
          "the scanned classification is captured");
  require(result.classification.pages_needing_ocr ==
              std::vector<int>({1, 2, 3}),
          "the OCR page set passes through 1-indexed, as on the wire");
  const auto decision = grparse::route_pdf_by_classification(result.classification);
  require(!decision.fast_path && decision.ocr_pages == std::vector<int>({1, 2, 3}),
          "a scanned document routes its page set to the CV path");
}

void verify_pdf_routing_decision_logic() {
  grparse::PdfClassification unseen;
  const auto no_answer = grparse::route_pdf_by_classification(unseen);
  require(!no_answer.fast_path && no_answer.ocr_pages.empty(),
          "no classification leaves the CV heuristic in charge");

  grparse::PdfClassification text_based;
  text_based.pdf_class = grparse::PdfClass::kTextBased;
  require(grparse::route_pdf_by_classification(text_based).fast_path,
          "text-based with no OCR pages is the fast path");

  grparse::PdfClassification garbled_text = text_based;
  garbled_text.pages_needing_ocr = {4};
  const auto not_fast = grparse::route_pdf_by_classification(garbled_text);
  require(!not_fast.fast_path && not_fast.ocr_pages == std::vector<int>({4}),
          "a text-based document with an OCR page is not the fast path");

  grparse::PdfClassification mixed;
  mixed.pdf_class = grparse::PdfClass::kMixed;
  mixed.pages_needing_ocr = {2, 5};
  const auto mixed_decision = grparse::route_pdf_by_classification(mixed);
  require(!mixed_decision.fast_path &&
              mixed_decision.ocr_pages == std::vector<int>({2, 5}),
          "a mixed document routes exactly the named pages");

  grparse::PdfClassification incoherent;
  incoherent.pdf_class = grparse::PdfClass::kImageBased;
  const auto fallback = grparse::route_pdf_by_classification(incoherent);
  require(!fallback.fast_path && fallback.ocr_pages.empty(),
          "an OCR-needing classification with no page set falls back to the heuristic");

  // The PF2-character-sheet shape: TEXT_BASED at full confidence, no pages
  // named, but the trailer flagged the text layer as untrustworthy. The
  // extraction must not become the parse result.
  grparse::PdfClassification garbled_encoding = text_based;
  garbled_encoding.encoding_issues = true;
  const auto declined = grparse::route_pdf_by_classification(garbled_encoding);
  require(!declined.fast_path && declined.ocr_pages.empty() && declined.force_ocr,
          "text-based with encoding issues is not the fast path and forces "
          "recognition in place of the untrustworthy layer");
  require(!grparse::route_pdf_by_classification(text_based).force_ocr &&
              !mixed_decision.force_ocr,
          "without encoding issues nothing escalates to forced recognition");

  grparse::PdfClassification garbled_mixed = mixed;
  garbled_mixed.encoding_issues = true;
  require(grparse::route_pdf_by_classification(garbled_mixed).force_ocr,
          "encoding issues force recognition for every classification");
}

void verify_pdf_encoding_issues_defeat_the_fast_path() {
  FakePdfService service(pdfv1::PDF_TYPE_TEXT_BASED, {}, /*encoding_issues=*/true);
  ServerFixture server(&service);
  const auto result = grparse::collect_pdf(server.channel(), "%PDF-fake");
  require(result.outcome.success, "pdf collection succeeds: " + result.outcome.error);
  require(result.classification.pdf_class == grparse::PdfClass::kTextBased,
          "the classification itself is still text-based");
  require(result.classification.encoding_issues,
          "the trailer's has_encoding_issues rides the classification");
  bool warned = false;
  for (const auto& warning : result.outcome.warnings) {
    if (warning.contains("encoding issues")) warned = true;
  }
  require(warned, "the untrustworthy text layer is warned about");
  const auto route = grparse::route_pdf_by_classification(result.classification);
  require(!route.fast_path,
          "a text-based classification with encoding issues does not fast-path");
  require(route.force_ocr,
          "the declined fast path escalates to forced recognition, skipping "
          "the digital extraction of the flagged layer");
}

void verify_pdf_collector_failure_is_an_outcome() {
  FailingPdfService service;
  ServerFixture server(&service);
  const auto result = grparse::collect_pdf(server.channel(), "%PDF-fake");
  require(!result.outcome.success && result.outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a collector panic surfaces as a failed, unavailable outcome");
  require(result.outcome.error.contains("lopdf panicked"),
          "the collector's own message survives");
  require(result.classification.pdf_class == grparse::PdfClass::kUnknown,
          "a failed stream carries no classification");
}

void verify_pdf_endpoint_configuration() {
  grparse::CollectorTargets targets;
  grparse::CollectorEndpoints unconfigured(targets);
  require(!unconfigured.has(parsev1::COLLECTOR_PDF),
          "an unset GRPARSE_PDF_TARGET leaves the collector unconfigured");
  targets.pdf = "pdf-inspector:50067";
  grparse::CollectorEndpoints configured(targets);
  require(configured.has(parsev1::COLLECTOR_PDF) &&
              configured.target(parsev1::COLLECTOR_PDF) == "pdf-inspector:50067",
          "the configured target resolves for the pdf collector");
  require(configured.channel(parsev1::COLLECTOR_PDF) != nullptr,
          "a configured target yields a channel");
}

void verify_pdf_plain_leg_returns_the_document() {
  FakePdfService service(pdfv1::PDF_TYPE_MIXED, {2});
  ServerFixture server(&service);
  const auto outcome = grparse::collect_pdf_document(server.channel(), "%PDF-fake");
  require(outcome.success && outcome.document.texts(0).text().base().text() == "from pdf",
          "the plain leg returns the collector's document whatever the class");
}

}  // namespace

void verify_source_title_promotes_to_a_title_item() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  docv1::TextItemBase* paragraph = document.add_texts()->mutable_text()->mutable_base();
  paragraph->set_self_ref("#/texts/0");
  paragraph->mutable_parent()->set_ref("#/body");
  paragraph->set_text("Byrd & Davis");
  document.mutable_body()->add_children()->set_ref("#/texts/0");
  require(!grparse::promote_source_title(&document), "no source title, nothing to promote");
  document.mutable_source_meta()->set_title("Baughman & Datron");
  require(grparse::promote_source_title(&document), "a source title becomes a title item");
  require(document.texts_size() == 2 && document.texts(1).has_title() &&
              document.texts(1).title().base().text() == "Baughman & Datron" &&
              document.texts(1).title().base().label() == docv1::DOC_ITEM_LABEL_TITLE,
          "the item carries the title text");
  require(document.body().children_size() == 2 &&
              document.body().children(0).ref() == "#/texts/1" &&
              document.body().children(1).ref() == "#/texts/0",
          "the title leads the body");
  require(document.texts(1).title().base().source(0).collector().collector() == "grparse" &&
              document.texts(1).title().base().source(0).collector().model() == "source-meta-title",
          "the item is attributed as derived from the collector's claim");
  require(!grparse::promote_source_title(&document), "a body with a title is left alone");
  require(!grparse::promote_source_title(nullptr), "a null document is a no-op");
}

int main() {
  return grparse_test::run_test_main({.on_failure = "FAILED", .on_success = "document collectors test passed"}, {
      verify_source_title_promotes_to_a_title_item,
      verify_asr_collects_document,
      verify_transport_class_collapses_to_unavailable,
      verify_email_collects_document_and_warnings,
      verify_missing_trailer_fails,
      verify_xml_collects_document_and_formats_warnings,
      verify_caller_status_classes_survive,
      verify_inbound_deadline_bounds_a_hanging_collector,
      verify_ebcdic_forwards_layout_and_collects,
      verify_ebcdic_without_layout_never_dials,
      verify_epub_collects_document,
      verify_missing_document_event_fails,
      verify_epub_book_folds_chapters_and_images,
      verify_epub_book_without_markup_keeps_the_skeleton,
      verify_epub_book_survives_a_failing_chapter,
      verify_markup_forwards_hint_and_collects,
      verify_lol_html_forwards_rules_and_folds,
      verify_lol_html_without_rules_never_dials,
      verify_lol_html_in_band_error_is_terminal,
      verify_lol_html_captures_page_identity,
      verify_fastwarc_folds_records_and_warnings,
      verify_fastwarc_captures_web_provenance,
      verify_fastwarc_response_supersedes_the_request_record,
      verify_fastwarc_framing_error_keeps_records,
      verify_fastwarc_transport_failure_without_records,
      verify_fastwarc_truncates_payload_text,
      verify_poi_folds_typed_events,
      verify_poi_collector_failure_survives_its_code,
      verify_poi_truncated_stream_fails,
      verify_poi_unreachable_endpoint_degrades,
      verify_calamine_folds_sheets,
      verify_calamine_sheet_failure_still_closes,
      verify_calamine_unreachable_endpoint_degrades,
      verify_pdf_collects_document_classification_and_warnings,
      verify_pdf_scanned_reports_the_ocr_page_set,
      verify_pdf_routing_decision_logic,
      verify_pdf_encoding_issues_defeat_the_fast_path,
      verify_pdf_collector_failure_is_an_outcome,
      verify_pdf_endpoint_configuration,
      verify_pdf_plain_leg_returns_the_document,
  });
}
