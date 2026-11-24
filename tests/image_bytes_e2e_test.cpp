// A picture's image, end to end, as bytes: an office upload goes through
// ConvertSource with picture description on, against a fake office collector
// that sends one embedded image and a fake enrichment peer that describes
// it. The bytes the collector sent are the bytes the enrichment peer
// receives and the bytes the answered Document holds, in ImageRef.data, and
// at no point are they spelled as a base64 data: URI.

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/enrich/v1/enrich_service.grpc.pb.h"
#include "ai/pipestream/office/v1/office_service.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"
#include "support/check.h"

namespace {

using namespace std::chrono_literals;
namespace docv1 = ai::pipestream::document::v1;
namespace enrichv1 = ai::pipestream::enrich::v1;
namespace officev1 = ai::pipestream::office::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;

// Bytes that are not text: a NUL, a high byte, a line feed. Anything that
// carried them through a string encoding and back would have to get every
// one of them right; nothing should be trying.
const std::string kImage("\x89PNG\r\n\x1a\n\x00\xFF\x0A image body", 22);

// One page, one paragraph, one picture.
class PictureOfficeService final : public officev1::OfficeRenderService::Service {
 public:
  grpc::Status StreamPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<officev1::StreamPagesResponse, officev1::StreamPagesRequest>*
          stream) override {
    officev1::StreamPagesRequest request;
    while (stream->Read(&request)) {
    }
    officev1::StreamPagesResponse event;
    officev1::DocumentInfo* info = event.mutable_document_info();
    info->set_document_id("figure.docx");
    info->set_source_format("docx");
    info->set_page_count(1);
    info->set_document_type("text");
    officev1::PageRect* page = info->add_page_rects();
    page->set_width_twips(12240);
    page->set_height_twips(15840);
    stream->Write(event);

    event.Clear();
    officev1::EmbeddedImage* image = event.mutable_embedded_image();
    image->set_page_index(0);
    image->set_name("Figure 1");
    image->set_mime_type("image/png");
    image->set_data(kImage);
    image->set_width_twips(6000);
    image->set_height_twips(4000);
    image->mutable_anchor()->set_x(1440);
    image->mutable_anchor()->set_y(2000);
    stream->Write(event);

    event.Clear();
    event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Describes every picture of the Document it is sent, and keeps what it saw.
class DescribingEnrichService final : public enrichv1::EnrichService::Service {
 public:
  struct Seen {
    std::vector<std::string> image_refs;
    std::vector<std::string> image_mimetypes;
    std::vector<std::string> image_bytes;
    docv1::Document document;
    bool document_parsed = false;
    int calls = 0;
  };

  grpc::Status EnrichDocument(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<enrichv1::EnrichDocumentResponse, enrichv1::EnrichDocumentRequest>*
          stream) override {
    Seen seen;
    std::string document_bytes;
    enrichv1::EnrichDocumentRequest request;
    while (stream->Read(&request)) {
      if (request.has_image()) {
        seen.image_refs.push_back(request.image().self_ref());
        seen.image_mimetypes.push_back(request.image().mimetype());
        seen.image_bytes.push_back(request.image().data());
      } else if (request.has_chunk()) {
        document_bytes += request.chunk().data();
      }
    }
    seen.document_parsed = seen.document.ParseFromString(document_bytes);
    enrichv1::EnrichDocumentResponse event;
    event.mutable_started()->set_picture_descriptions(
        static_cast<uint32_t>(seen.document.pictures_size()));
    stream->Write(event);
    uint32_t described = 0;
    for (const docv1::PictureItem& picture : seen.document.pictures()) {
      event.Clear();
      enrichv1::ItemAnnotation* annotation = event.mutable_annotation();
      annotation->set_self_ref(picture.self_ref());
      annotation->set_model("fake-vlm");
      annotation->mutable_description()->set_text("A figure.");
      stream->Write(event);
      ++described;
    }
    event.Clear();
    event.mutable_complete()->set_succeeded(described);
    stream->Write(event);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen.calls = seen_.calls + 1;
      seen_ = std::move(seen);
    }
    return grpc::Status::OK;
  }

  Seen seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

 private:
  mutable std::mutex mutex_;
  Seen seen_;
};

// The office route never reaches the CV path; a call that got there would be
// a regression of its own.
class UnusedRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    throw std::runtime_error("an office route reached the CV recognizer");
  }
};

std::unique_ptr<grpc::Server> start(std::vector<grpc::Service*> services, int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
  for (grpc::Service* service : services) builder.RegisterService(service);
  auto server = builder.BuildAndStart();
  if (!server || *port == 0) throw std::runtime_error("image bytes rig failed to start");
  return server;
}

void verify_a_pictures_bytes_survive_conversion_with_enrichment() {
  PictureOfficeService office;
  DescribingEnrichService enrich;
  int peer_port = 0;
  auto peers = start({&office, &enrich}, &peer_port);
  const std::string peer = "127.0.0.1:" + std::to_string(peer_port);

  UnusedRecognizer recognizer;
  grparse::PageScheduler scheduler(recognizer, grparse::PageScheduler::Options{});
  grparse::CollectorTargets targets;
  targets.libreoffice = peer;
  targets.derender.target = peer;
  grparse::DocumentParserService parser(scheduler,
                                        std::make_shared<grparse::CollectorEndpoints>(targets));
  int port = 0;
  auto server = start({&parser}, &port);
  auto client = parsev1::ParseService::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));

  parsev1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename("figure.docx");
  // "PK\x03\x04 not an office file", as the request's base64 text field
  // wants it; the fake collector does not read it.
  source->set_base64_string("UEsDBCBub3QgYW4gb2ZmaWNlIGZpbGU=");
  auto* options = request.mutable_request()->mutable_options();
  options->set_do_picture_description(true);
  // The picture is well under a twentieth of its page; describe it anyway.
  options->set_picture_description_area_threshold(-1);
  parsev1::ConvertSourceResponse response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 30s);
  const grpc::Status status = client->ConvertSource(&context, request, &response);
  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();
  peers->Shutdown(std::chrono::system_clock::now() + 2s);
  peers->Wait();
  require(status.ok(), "the conversion failed: " + status.error_message());

  // What the enrichment peer was sent.
  const DescribingEnrichService::Seen seen = enrich.seen();
  require(seen.calls == 1 && seen.document_parsed, "the enrichment peer was dialed once");
  require(seen.image_refs.size() == 1 && seen.image_refs[0] == "#/pictures/0",
          "the picture's image travels to the enrichment peer, keyed by its reference");
  require(seen.image_bytes[0] == kImage && seen.image_mimetypes[0] == "image/png",
          "the enrichment peer receives the very bytes the collector sent, got " +
              std::to_string(seen.image_bytes[0].size()) + " bytes");
  require(seen.document.pictures_size() == 1 &&
              !seen.document.pictures(0).image().has_data() &&
              seen.document.pictures(0).image().uri().empty(),
          "the Document sent beside it does not carry the image a second time");

  // What the caller is answered with.
  const docv1::Document& document = response.response().document().doc();
  require(document.pictures_size() == 1 && document.pictures(0).has_image(),
          "the answered Document has the picture and its image");
  const docv1::ImageRef& image = document.pictures(0).image();
  require(image.data() == kImage,
          "the answered picture holds the bytes the collector sent, got " +
              std::to_string(image.data().size()) + " bytes");
  require(image.uri().empty(),
          "bytes the Document holds are not also spelled as a URI, got '" +
              image.uri().substr(0, 40) + "'");
  require(image.mimetype() == "image/png", "the media type rides beside the bytes");
  require(document.pictures(0).meta().description().text() == "A figure.",
          "the description the enrichment peer answered landed on the picture");
  for (const docv1::CollectorWarning& warning : document.warnings()) {
    require(warning.code() != docv1::WARNING_CODE_ENRICHMENT_CROP_MISSING,
            "no enrichment call went without its image: " + warning.message());
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("image-bytes-e2e-test", "all checks passed", {
      verify_a_pictures_bytes_survive_conversion_with_enrichment,
  });
}
