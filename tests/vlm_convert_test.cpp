#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/vlm/v1/vlm_convert.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_previews.h"
#include "grparse/vlm_convert.h"
#include "support/check.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;

class FakeVlmConvertService final : public vlmv1::VlmConvertService::Service {
 public:
  grpc::Status ConvertPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<vlmv1::ConvertPagesResponse, vlmv1::ConvertPagesRequest>*
          stream) override {
    vlmv1::ConvertPagesRequest request;
    Seen seen;
    bool first = true;
    while (stream->Read(&request)) {
      if (first) {
        seen.options_first = request.has_options();
        first = false;
      }
      if (request.has_options()) {
        seen.options = request.options();
      } else if (request.has_page_image()) {
        seen.page_nos.push_back(request.page_image().page_no());
        seen.page_bytes.push_back(request.page_image().png().size());
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_ = seen;
    }
    ++calls_;
    for (uint32_t page_no : seen.page_nos) {
      vlmv1::ConvertPagesResponse event;
      event.mutable_page_started()->set_page_no(page_no);
      stream->Write(event);
      event.Clear();
      auto* page = event.mutable_page_document();
      page->set_page_no(page_no);
      auto* text = page->mutable_document()->add_texts()->mutable_text();
      text->mutable_base()->set_self_ref("#/texts/" + std::to_string(page_no - 1));
      text->mutable_base()->set_text("page-" + std::to_string(page_no));
      (*page->mutable_document()->mutable_pages())[page_no].set_page_no(page_no);
      stream->Write(event);
    }
    vlmv1::ConvertPagesResponse done;
    done.mutable_complete()->set_pages_started(seen.page_nos.size());
    done.mutable_complete()->set_pages_ok(seen.page_nos.size());
    stream->Write(done);
    return grpc::Status::OK;
  }

  struct Seen {
    bool options_first = false;
    vlmv1::ConvertOptions options;
    std::vector<uint32_t> page_nos;
    std::vector<size_t> page_bytes;
  };

  Seen seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }
  int calls() const { return calls_.load(); }

 private:
  mutable std::mutex mutex_;
  Seen seen_;
  std::atomic<int> calls_{0};
};

class ServerFixture {
 public:
  explicit ServerFixture(grpc::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) {
      throw std::runtime_error("fake vlm-convert server failed to start");
    }
  }
  ~ServerFixture() {
    if (server_ != nullptr) server_->Shutdown();
  }
  std::string target() const { return "127.0.0.1:" + std::to_string(port_); }
  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel(target(), grpc::InsecureChannelCredentials());
  }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

void verify_apply_maps_granite_docling_and_raw_api_url() {
  parsev1::ConvertDocumentOptions convert;
  convert.set_vlm_pipeline_model(parsev1::VLM_MODEL_TYPE_GRANITEDOCLING);
  convert.set_render_scale(2.0);
  convert.set_abort_on_error(true);
  convert.add_page_range(1);
  convert.add_page_range(2);
  grparse::VlmConvertOptions options;
  grparse::apply_vlm_convert_options(convert, &options);
  require(options.preset == vlmv1::VLM_PRESET_GRANITE_DOCLING, "granite-docling preset");
  require(options.render_dpi == 144.0, "render_scale 2.0 is 144 DPI");
  require(options.abort_on_error && options.page_range.has_value() &&
              options.page_range->first == 1 && options.page_range->second == 2,
          "abort and page_range copy");

  parsev1::ConvertDocumentOptions api;
  api.set_vlm_pipeline_model_api("http://vlm.example:8080/v1");
  grparse::VlmConvertOptions api_options;
  grparse::apply_vlm_convert_options(api, &api_options);
  require(api_options.endpoint == "http://vlm.example:8080/v1", "http api string is endpoint");
}

void verify_convert_rasters_dials_and_merges() {
  FakeVlmConvertService fake;
  ServerFixture server(&fake);
  auto bytes = std::make_shared<const std::string>("fake-pdf-bytes");
  // Inject a page source by converting through a channel; convert_vlm_pages
  // opens via open_in_memory_document, so use a real tiny PNG raster instead.
  cv::Mat image(16, 12, CV_8UC3, cv::Scalar(1, 2, 3));
  std::vector<unsigned char> png;
  require(cv::imencode(".png", image, png, grparse::kPngEncodeParams), "encode fixture");
  bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(png.data()),
                                             png.size());

  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(5000);
  options.preset = vlmv1::VLM_PRESET_GRANITE_DOCLING;
  options.endpoint = "http://vlm.test:9";
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const grparse::VlmConvertReport report =
      grparse::convert_vlm_pages(server.channel(), options, bytes, /*pdf=*/false, &document);
  require(report.success && report.pages_sent == 1 && report.pages_ok == 1,
          "one raster page converts: " + report.error);
  require(document.texts_size() == 1 && document.texts(0).text().base().text() == "page-1",
          "page document merges into the target");
  const FakeVlmConvertService::Seen seen = fake.seen();
  require(seen.options_first && seen.options.preset() == vlmv1::VLM_PRESET_GRANITE_DOCLING &&
              seen.options.endpoint() == "http://vlm.test:9" && seen.page_nos.size() == 1 &&
              seen.page_nos[0] == 1 && seen.page_bytes[0] > 0,
          "options lead and one full-page PNG is sent");
}

void verify_missing_target_is_failed_precondition() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  auto bytes = std::make_shared<const std::string>("x");
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      nullptr, grparse::VlmConvertOptions{}, bytes, false, &document);
  require(!report.success && report.code == grpc::StatusCode::FAILED_PRECONDITION &&
              report.error.contains("GRPARSE_VLM_CONVERT_TARGET"),
          "unconfigured target names the env var");
}

void verify_endpoints_lazy_channel() {
  grparse::CollectorTargets targets;
  require(!targets.vlm.enabled(), "default off");
  grparse::CollectorEndpoints empty(targets);
  require(!empty.has_vlm() && empty.vlm_channel() == nullptr, "no channel without target");
  targets.vlm.target = "vlm-convert:50058";
  grparse::CollectorEndpoints wired(targets);
  require(wired.has_vlm() && wired.vlm_channel() != nullptr &&
              wired.vlm_channel() == wired.vlm_channel(),
          "configured target gets one lazy channel");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("vlm-convert-test", "all checks passed", {
      verify_apply_maps_granite_docling_and_raw_api_url,
      verify_convert_rasters_dials_and_merges,
      verify_missing_target_is_failed_precondition,
      verify_endpoints_lazy_channel,
  });
}
