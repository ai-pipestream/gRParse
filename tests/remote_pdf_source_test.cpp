// Exercises the backend-client PDF source against an in-process fake
// PdfBackendService, so the wire conversion (bottom-left points to
// top-left pixels, font table joins, subset stripping, the OCR-skip gate,
// raster format conversion, typed load failures) is covered without a
// real backend running.
#include <memory>
#include <print>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/core.hpp>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/in_memory_document.h"
#include "grparse/remote_page_source.h"
#include "support/check.h"

namespace {

using grparse_test::require;
namespace pdfv1 = ai::pipestream::parse::pdf::v1;

constexpr double kPageWidthPts = 612.0;
constexpr double kPageHeightPts = 792.0;

// A one-page backend whose text layer clears the digital-coverage gate:
// five cells spread over most of the page height, each with enough
// characters.
class FakeBackend final : public pdfv1::PdfBackendService::Service {
 public:
  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest* request,
                     pdfv1::ProbeResponse* response) override {
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name("fake-backend");
    caps->set_engine_version("test");
    if (request->document().data() == "%PDF-broken") {
      caps->set_load_status(pdfv1::LOAD_STATUS_CORRUPT);
      caps->set_load_detail("fixture says no");
      return grpc::Status::OK;
    }
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest* request,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    require(request->families_size() >= 1, "client narrows the family list");
    require(request->has_pages() && request->pages().begin() == 0 &&
                request->pages().end() == 1,
            "client requests the single page range");
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    auto* info = header.mutable_header()->add_pages();
    info->set_page_index(0);
    info->set_width_pts(kPageWidthPts);
    info->set_height_pts(kPageHeightPts);
    writer->Write(header);

    pdfv1::ParseResponse fonts;
    auto* font = fonts.mutable_fonts()->add_fonts();
    font->set_font_id(7);
    font->set_base_name("ABCDEF+Times-Roman");
    writer->Write(fonts);

    pdfv1::ParseResponse page;
    auto* chunk = page.mutable_page();
    chunk->set_page_index(0);
    double y = 700.0;
    for (int i = 0; i < 5; ++i) {
      auto* cell = chunk->add_text_cells();
      cell->set_text("line number " + std::to_string(i) + " of the fixture");
      cell->mutable_bbox()->set_x0(72.0);
      cell->mutable_bbox()->set_y0(y);
      cell->mutable_bbox()->set_x1(300.0);
      cell->mutable_bbox()->set_y1(y + 12.0);
      cell->set_font_id(7);
      cell->set_font_size(12.0);
      y -= 150.0;
    }
    writer->Write(page);

    pdfv1::ParseResponse trailer;
    auto* count = trailer.mutable_trailer()->add_counts();
    count->set_family(pdfv1::PDF_FAMILY_TEXT_CELLS);
    count->set_count(5);
    writer->Write(trailer);
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    require(request->pixel_format() == pdfv1::PIXEL_FORMAT_BGR8,
            "client asks for the fold's BGR layout");
    // Answer in RGBA8 on purpose: the client must convert.
    const int width = static_cast<int>(kPageWidthPts * request->dpi() / 72.0);
    const int height = static_cast<int>(kPageHeightPts * request->dpi() / 72.0);
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(0);
    raster->set_width_px(width);
    raster->set_height_px(height);
    raster->set_stride_bytes(width * 4);
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_RGBA8);
    raster->set_dpi(request->dpi());
    // Solid red in RGBA; the converted BGR mat must read (0, 0, 255).
    std::string pixels(static_cast<size_t>(width) * height * 4, '\0');
    for (size_t i = 0; i < pixels.size(); i += 4) {
      pixels[i] = static_cast<char>(0xFF);      // R
      pixels[i + 3] = static_cast<char>(0xFF);  // A
    }
    raster->set_pixels(pixels);
    writer->Write(msg);
    return grpc::Status::OK;
  }
};

}  // namespace

int main() {
  FakeBackend backend;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&backend);
  auto server = builder.BuildAndStart();
  require(server != nullptr && port != 0, "fake backend started");
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const auto bytes = std::make_shared<const std::string>("%PDF-fake");
  const double dpi = 144.0;  // scale factor 2 keeps the pixel math legible
  const auto source = grparse::open_remote_pdf_document(bytes, target, dpi);
  require(source->page_count() == 1, "page count from Probe");

  const auto page = source->extract_digital_page(1);
  require(page.has_value(), "digital page extracted");
  require(page->width == 1224 && page->height == 1584,
          "page dimensions scale by dpi/72");
  require(page->source == grparse::OcrPage::Source::kDigitalPdf,
          "page is marked digital");
  require(page->lines.size() == 5, "all cells became lines");
  require(page->skip_ocr, "five spread-out lines clear the OCR-skip gate");
  const auto& first = page->lines.front();
  require(first.text == "line number 0 of the fixture", "text passes through");
  require(first.font_name.has_value() && *first.font_name == "Times-Roman",
          "subset prefix stripped from the font name");
  require(first.font_size_pt.has_value() && *first.font_size_pt == 12.0,
          "font size passes through");
  // Cell at y0=700, y1=712 on a 792pt page: top pixel = (792-712)*2 = 160.
  require(first.polygon.size() == 4, "line polygon has four corners");
  require(first.polygon[0].y == 160 && first.polygon[2].y == 184,
          "boxes flip into the top-left raster frame");
  require(first.polygon[0].x == 144 && first.polygon[1].x == 600,
          "horizontal box edges scale");

  const cv::Mat mat = source->render_page(1);
  require(mat.type() == CV_8UC3, "raster converts to the fold's BGR mat");
  require(mat.cols == 1224 && mat.rows == 1584, "raster is page size at DPI");
  const auto px = mat.at<cv::Vec3b>(10, 10);
  require(px[0] == 0 && px[1] == 0 && px[2] == 255,
          "RGBA red converts to BGR red");

  bool threw = false;
  try {
    const auto broken = std::make_shared<const std::string>("%PDF-broken");
    grparse::open_remote_pdf_document(broken, target, dpi);
  } catch (const grparse::InvalidDocument& error) {
    threw = std::string(error.what()).find("CORRUPT") != std::string::npos;
  }
  require(threw, "typed load failure raises InvalidDocument with the status");

  require(!grparse::remote_pdf_backend_target().has_value() ||
              std::getenv("GRPARSE_PDF_BACKEND") != nullptr,
          "target helper only reports when the variable is set");

  server->Shutdown();
  std::println("remote-pdf-source-test: all checks passed");
  return 0;
}
