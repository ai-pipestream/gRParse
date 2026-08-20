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
#include "ai/pipestream/markup/v1/markup_service.grpc.pb.h"
#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "fastwarc/v1/warc_service.grpc.pb.h"
#include "grparse/document_collectors.h"
#include "grparse/document_parser_service.h"
#include "lolhtml/v1/lolhtml_service.grpc.pb.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace docv1 = ai::pipestream::document::v1;
namespace ebcdicv1 = ai::pipestream::ebcdic::v1;
namespace emailv1 = ai::pipestream::email::v1;
namespace epubv1 = ai::pipestream::epub::v1;
namespace lolv1 = lolhtml::v1;
namespace markupv1 = ai::pipestream::markup::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;
namespace warcv1 = fastwarc::v1;
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

// ---- lol-html --------------------------------------------------------------

// The lol-html wire carries no document event, so unlike every other fake
// this one serves raw match events and the assertions land on the client's
// own fold.
class FakeLolHtmlService final : public lolv1::LolHtmlService::Service {
 public:
  explicit FakeLolHtmlService(bool fail) : fail_(fail) {}

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
    if (fail_) {
      auto* error = event.mutable_error();
      error->set_code(lolv1::PARSE_ERROR_CODE_PARSING_AMBIGUITY);
      error->set_message("refused to guess");
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
  bool fail_;
};

constexpr const char* kLolHtmlOptionsJson =
    R"({"rules":[{"id":"links","selector":"a[href]",)"
    R"("captures":["CAPTURE_TAG_NAME","CAPTURE_ATTRIBUTES","CAPTURE_TEXT"]},)"
    R"({"id":"headings","selector":"h2","captures":["CAPTURE_TEXT"]}]})";

void verify_lol_html_forwards_rules_and_folds() {
  FakeLolHtmlService service(/*fail=*/false);
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
  require(outcome.warnings.size() == 1 &&
              outcome.warnings[0] == "rule 'headings' matched nothing",
          "a zero-match rule surfaces as a warning");
}

void verify_lol_html_without_rules_never_dials() {
  const auto outcome =
      grparse::collect_lol_html_document(nullptr, "", "<p>hi</p>");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "missing lol_html_options_json degrades before dialing");
  const auto garbled =
      grparse::collect_lol_html_document(nullptr, "not json", "<p>hi</p>");
  require(!garbled.success && garbled.code == grpc::StatusCode::INVALID_ARGUMENT &&
              garbled.error.find("ExtractOptions") != std::string::npos,
          "unparseable options degrade before dialing, naming the type");
}

void verify_lol_html_in_band_error_is_terminal() {
  FakeLolHtmlService service(/*fail=*/true);
  ServerFixture server(&service);
  const auto outcome = grparse::collect_lol_html_document(
      server.channel(), kLolHtmlOptionsJson, "<select><xmp><script>");
  require(!outcome.success && outcome.code == grpc::StatusCode::INVALID_ARGUMENT &&
              outcome.error.find("PARSE_ERROR_CODE_PARSING_AMBIGUITY") !=
                  std::string::npos,
          "the in-band terminal error fails the outcome with its typed code");
}

// ---- fastwarc --------------------------------------------------------------

// The fastwarc wire, like lol-html's, carries no document event and no
// terminal status: the fake serves canned record events (some batched, some
// not) and the assertions land on the client's own fold.
class FakeWarcService final : public warcv1::WarcService::Service {
 public:
  enum class Mode { kOk, kFramingError, kTransportError, kTruncated };
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
    if (!config.parse_http() || !config.include_payload() ||
        !config.include_headers() || config.response_batch_size() != 64 ||
        bytes.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "fake fastwarc expects http parsing, payload and headers "
                          "included, response batching, and archive bytes");
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
    // First record, start and first chunk packed in one batch: the client
    // opted into batching, so the flatten is its side of the contract.
    auto* batch = event.mutable_batch();
    auto* start = batch->add_items()->mutable_record_start()->mutable_metadata();
    start->set_record_type(warcv1::WARC_RECORD_TYPE_RESPONSE);
    start->set_stream_pos(0);
    start->set_content_length(23);
    start->set_is_http(true);
    start->set_http_content_type("text/html");
    start->set_record_id("<urn:uuid:test-1>");
    start->mutable_record_date()->set_seconds(1704164645);  // 2024-01-02T03:04:05Z
    auto* chunk = batch->add_items()->mutable_payload_chunk();
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
    // Second record: a warcinfo with no HTTP metadata, served unbatched.
    auto* meta = event.mutable_record_start()->mutable_metadata();
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
          "payload chunks reassemble across a batch boundary, in order");
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

void verify_fastwarc_framing_error_keeps_records() {
  FakeWarcService service(FakeWarcService::Mode::kFramingError);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(outcome.success && outcome.document.groups_size() == 1,
          "a non-recoverable framing error keeps the records already folded");
  require(outcome.warnings.size() == 2 &&
              outcome.warnings[1].find("framing error at stream offset 128") !=
                  std::string::npos,
          "the framing error surfaces as a warning, not a failure");
}

void verify_fastwarc_transport_failure_without_records() {
  FakeWarcService service(FakeWarcService::Mode::kTransportError);
  ServerFixture server(&service);
  const auto outcome =
      grparse::collect_fastwarc_document(server.channel(), "WARC/1.0 fake bytes");
  require(!outcome.success && outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a transport failure before any record is a failed outcome");
  require(outcome.error.find("socket went away") != std::string::npos,
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
              payload.compare(0, 16, std::string(16, 'a')) == 0 &&
              payload.compare(65536, suffix.size(), suffix) == 0,
          "the payload folds to the 64 KiB cap with the truncation noted");
}

}  // namespace

// ---- pdf --------------------------------------------------------------------

namespace {

// Serves the routing contract: an info event with a configurable
// classification, one page event for text-bearing documents, the folded
// document (emit_document is asserted on), and the status trailer.
class FakePdfService final : public pdfv1::PdfParseService::Service {
 public:
  FakePdfService(pdfv1::PdfType type, std::vector<uint32_t> pages_needing_ocr)
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
    auto* warning = status->add_warnings();
    warning->set_code(pdfv1::PARSE_WARNING_CODE_PASSWORD_FALLBACK);
    warning->set_message("extracted whole-document");
    stream->Write(event);
    return grpc::Status::OK;
  }

 private:
  pdfv1::PdfType type_;
  std::vector<uint32_t> pages_needing_ocr_;
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
}

void verify_pdf_collector_failure_is_an_outcome() {
  FailingPdfService service;
  ServerFixture server(&service);
  const auto result = grparse::collect_pdf(server.channel(), "%PDF-fake");
  require(!result.outcome.success && result.outcome.code == grpc::StatusCode::UNAVAILABLE,
          "a collector panic surfaces as a failed, unavailable outcome");
  require(result.outcome.error.find("lopdf panicked") != std::string::npos,
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
    verify_markup_forwards_hint_and_collects();
    verify_lol_html_forwards_rules_and_folds();
    verify_lol_html_without_rules_never_dials();
    verify_lol_html_in_band_error_is_terminal();
    verify_fastwarc_folds_records_and_warnings();
    verify_fastwarc_framing_error_keeps_records();
    verify_fastwarc_transport_failure_without_records();
    verify_fastwarc_truncates_payload_text();
    verify_pdf_collects_document_classification_and_warnings();
    verify_pdf_scanned_reports_the_ocr_page_set();
    verify_pdf_routing_decision_logic();
    verify_pdf_collector_failure_is_an_outcome();
    verify_pdf_endpoint_configuration();
    verify_pdf_plain_leg_returns_the_document();
  } catch (const std::exception& failure) {
    std::cerr << "FAILED: " << failure.what() << std::endl;
    return 1;
  }
  std::cout << "document collectors test passed" << std::endl;
  return 0;
}
