// Exercises the consensus page source against in-process fake backends
// with differing word orders: the bigram vote must return the majority
// order no matter where the scrambled backend is in the target list, a
// backend that cannot load the document must drop out of the vote, and
// target splitting must trim.
#include <memory>
#include <print>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/consensus_page_source.h"
#include "grparse/in_memory_document.h"
#include "support/check.h"

namespace {

using grparse_test::require;
namespace pdfv1 = ai::pipestream::parse::pdf::v1;

constexpr double kPageWidthPts = 612.0;
constexpr double kPageHeightPts = 792.0;

// One page whose cells are the words handed to the constructor, one word
// per cell, laid out left to right with uniform line-height boxes.
class FakeBackend final : public pdfv1::PdfBackendService::Service {
 public:
  FakeBackend(std::string name, std::vector<std::string> words, bool loads)
      : name_(std::move(name)), words_(std::move(words)), loads_(loads) {}

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest*,
                     pdfv1::ProbeResponse* response) override {
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name(name_);
    caps->set_engine_version("test");
    if (!loads_) {
      caps->set_load_status(pdfv1::LOAD_STATUS_CORRUPT);
      return grpc::Status::OK;
    }
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest*,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    auto* info = header.mutable_header()->add_pages();
    info->set_page_index(0);
    info->set_width_pts(kPageWidthPts);
    info->set_height_pts(kPageHeightPts);
    writer->Write(header);

    pdfv1::ParseResponse page;
    auto* chunk = page.mutable_page();
    chunk->set_page_index(0);
    double x = 72.0;
    for (const auto& word : words_) {
      auto* cell = chunk->add_text_cells();
      cell->set_text(word);
      cell->mutable_bbox()->set_x0(x);
      cell->mutable_bbox()->set_y0(700.0);
      cell->mutable_bbox()->set_x1(x + 40.0);
      cell->mutable_bbox()->set_y1(712.0);
      x += 50.0;
    }
    writer->Write(page);
    writer->Write(pdfv1::ParseResponse{});
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(0);
    raster->set_width_px(10);
    raster->set_height_px(10);
    raster->set_stride_bytes(30);
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    raster->set_dpi(request->dpi());
    raster->set_pixels(std::string(300, '\x7f'));
    writer->Write(msg);
    return grpc::Status::OK;
  }

 private:
  std::string name_;
  std::vector<std::string> words_;
  bool loads_;
};

struct Server {
  std::unique_ptr<FakeBackend> service;
  std::unique_ptr<grpc::Server> server;
  std::string target;
};

Server start(std::string name, std::vector<std::string> words, bool loads) {
  Server out;
  out.service =
      std::make_unique<FakeBackend>(std::move(name), std::move(words), loads);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(out.service.get());
  out.server = builder.BuildAndStart();
  require(out.server != nullptr && port != 0, "fake backend started");
  out.target = "127.0.0.1:" + std::to_string(port);
  return out;
}

std::string joined_text(const grparse::OcrPage& page) {
  std::string text;
  for (const auto& line : page.lines) {
    if (!text.empty()) text += " ";
    text += line.text;
  }
  return text;
}

}  // namespace

int main() {
  const std::vector<std::string> reading = {
      "The", "survey", "team", "returned", "the", "equipment",
      "on", "time", "and", "in", "good", "condition."};
  std::vector<std::string> scrambled = reading;
  for (size_t i = 0; i + 1 < scrambled.size(); i += 2) {
    std::swap(scrambled[i], scrambled[i + 1]);
  }

  Server good_a = start("good-a", reading, true);
  Server good_b = start("good-b", reading, true);
  Server bad = start("scrambled", scrambled, true);
  Server broken = start("broken", {}, false);

  const auto bytes = std::make_shared<const std::string>("%PDF-fake");
  const std::string expected = "The survey team returned the equipment on time and in good condition.";

  // The majority order wins with the scrambled backend in front.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {bad.target, good_a.target, good_b.target}, 144.0);
    require(source->page_count() == 1, "page count from the first backend");
    const auto page = source->extract_digital_page(1);
    require(page.has_value(), "consensus page extracted");
    require(joined_text(*page) == expected,
            "majority order wins over the scrambled leader");
  }

  // A backend that cannot load the document drops out of the vote.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {broken.target, bad.target, good_a.target, good_b.target},
        144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && joined_text(*page) == expected,
            "vote proceeds without the backend that failed to load");
    const cv::Mat mat = source->render_page(1);
    require(!mat.empty(), "raster comes from the first loaded backend");
  }

  // Two backends: agreement is symmetric, sentence continuity decides.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {bad.target, good_a.target}, 144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && joined_text(*page) == expected,
            "continuity breaks the two-backend tie toward running text");
  }

  const auto targets =
      grparse::split_backend_targets(" a:1 , b:2,c:3 ,, ");
  require(targets == std::vector<std::string>({"a:1", "b:2", "c:3"}),
          "target splitting trims and skips empties");

  std::println("consensus-page-source-test: all checks passed");
  return 0;
}
