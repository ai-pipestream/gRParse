// An in-process PdfBackendService for tests that send PDFs through the CV
// path. gRParse links no PDF engine: PDFs go to the backend that
// GRPARSE_PDF_BACKEND names, so a test registers the documents it will send
// (page geometry, rotation, text cells) and scopes the variable to this
// server. Rasters come back as blank white pages of the rendered size.
// Header only, and test-only: nothing under src/ or include/ may include it.
//
// The fake speaks the content-addressed handshake the way a real backend
// does: a hash-only request for bytes it has not seen is a cache miss
// (LOAD_STATUS_BYTES_REQUIRED), and bytes it does not know are CORRUPT.
#ifndef GRPARSE_TESTS_SUPPORT_FAKE_PDF_BACKEND_H
#define GRPARSE_TESTS_SUPPORT_FAKE_PDF_BACKEND_H

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "support/check.h"

namespace grparse_test {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

// One text cell in contract page space: PDF user space, bottom-left origin,
// before the page's /Rotate.
struct FakeTextCell {
  std::string text;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  std::string font = "Helvetica";
  double font_size = 12.0;
};

// One page: the unrotated MediaBox (which is also the CropBox unless one is
// set), the /Rotate value, and the text layer.
struct FakePdfPage {
  double width_pts = 612.0;
  double height_pts = 792.0;
  int rotation_degrees = 0;
  // {x0, y0, x1, y1}; unset means the MediaBox [0 0 width height].
  std::optional<std::vector<double>> crop_box = std::nullopt;
  std::vector<FakeTextCell> cells = {};
};

// Lines of 24pt text from the top of a Letter page down, one cell per line,
// 60pt apart: the shape a simple born-digital page has.
inline FakePdfPage text_page(const std::vector<std::string>& lines, int rotation_degrees = 0) {
  FakePdfPage page;
  page.rotation_degrees = rotation_degrees;
  double baseline = page.height_pts - 72.0;
  for (const auto& line : lines) {
    page.cells.push_back(FakeTextCell{line, 72.0, baseline - 5.0,
                                      72.0 + 14.4 * static_cast<double>(line.size()),
                                      baseline + 19.0, "ABCDEF+Helvetica", 24.0});
    baseline -= 60.0;
  }
  return page;
}

class FakePdfBackend final : public pdfv1::PdfBackendService::Service {
 public:
  // Registers the bytes a test will send and what the backend reads in them.
  void add_document(const std::string& bytes, std::vector<FakePdfPage> pages) {
    const std::lock_guard<std::mutex> lock(mutex_);
    known_[bytes] = std::move(pages);
  }

  int render_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return render_calls_;
  }

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest* request,
                     pdfv1::ProbeResponse* response) override {
    const auto [status, pages] = load(request->document());
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name("fake-pdf-backend");
    caps->set_engine_version("test");
    caps->set_load_status(status);
    if (status == pdfv1::LOAD_STATUS_OK) caps->set_page_count(static_cast<uint32_t>(pages.size()));
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest* request,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    const auto [status, pages] = load(request->document());
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_backend_name("fake-pdf-backend");
    caps->set_load_status(status);
    if (status != pdfv1::LOAD_STATUS_OK) {
      writer->Write(header);
      return grpc::Status::OK;
    }
    caps->set_page_count(static_cast<uint32_t>(pages.size()));
    for (size_t index = 0; index < pages.size(); ++index) {
      const FakePdfPage& page = pages[index];
      auto* info = header.mutable_header()->add_pages();
      info->set_page_index(static_cast<uint32_t>(index));
      const bool quarter_turn = page.rotation_degrees % 180 != 0;
      info->set_width_pts(quarter_turn ? page.height_pts : page.width_pts);
      info->set_height_pts(quarter_turn ? page.width_pts : page.height_pts);
      info->set_rotation_degrees(page.rotation_degrees);
      auto* media = info->mutable_media_box();
      media->set_x1(page.width_pts);
      media->set_y1(page.height_pts);
      if (page.crop_box.has_value()) {
        auto* crop = info->mutable_crop_box();
        crop->set_x0((*page.crop_box)[0]);
        crop->set_y0((*page.crop_box)[1]);
        crop->set_x1((*page.crop_box)[2]);
        crop->set_y1((*page.crop_box)[3]);
      } else {
        *info->mutable_crop_box() = *media;
      }
    }
    writer->Write(header);

    const uint32_t begin = request->has_pages() ? request->pages().begin() : 0;
    const uint32_t end = request->has_pages() ? request->pages().end()
                                              : static_cast<uint32_t>(pages.size());
    std::map<std::string, uint32_t> fonts;
    for (uint32_t index = begin; index < end && index < pages.size(); ++index) {
      pdfv1::ParseResponse chunk;
      chunk.mutable_page()->set_page_index(index);
      for (const FakeTextCell& source : pages[index].cells) {
        auto [font, added] = fonts.emplace(source.font, static_cast<uint32_t>(fonts.size()));
        if (added) {
          pdfv1::ParseResponse table;
          auto* ref = table.mutable_fonts()->add_fonts();
          ref->set_font_id(font->second);
          ref->set_base_name(source.font);
          writer->Write(table);
        }
        auto* cell = chunk.mutable_page()->add_text_cells();
        cell->set_text(source.text);
        cell->mutable_bbox()->set_x0(source.x0);
        cell->mutable_bbox()->set_y0(source.y0);
        cell->mutable_bbox()->set_x1(source.x1);
        cell->mutable_bbox()->set_y1(source.y1);
        cell->set_font_id(font->second);
        cell->set_font_size(source.font_size);
      }
      writer->Write(chunk);
    }
    pdfv1::ParseResponse trailer;
    trailer.mutable_trailer();
    writer->Write(trailer);
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    const auto [status, pages] = load(request->document());
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++render_calls_;
    }
    if (status != pdfv1::LOAD_STATUS_OK) {
      pdfv1::RenderResponse head;
      head.mutable_head()->set_load_status(status);
      writer->Write(head);
      return grpc::Status::OK;
    }
    const uint32_t begin = request->has_pages() ? request->pages().begin() : 0;
    const uint32_t end = request->has_pages() ? request->pages().end()
                                              : static_cast<uint32_t>(pages.size());
    for (uint32_t index = begin; index < end && index < pages.size(); ++index) {
      const FakePdfPage& page = pages[index];
      const bool quarter_turn = page.rotation_degrees % 180 != 0;
      const double width_pts = quarter_turn ? page.height_pts : page.width_pts;
      const double height_pts = quarter_turn ? page.width_pts : page.height_pts;
      const auto width = static_cast<uint32_t>(width_pts * request->dpi() / 72.0 + 0.5);
      const auto height = static_cast<uint32_t>(height_pts * request->dpi() / 72.0 + 0.5);
      pdfv1::RenderResponse message;
      auto* raster = message.mutable_raster();
      raster->set_page_index(index);
      raster->set_width_px(width);
      raster->set_height_px(height);
      raster->set_stride_bytes(width * 3);
      raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
      raster->set_dpi(request->dpi());
      raster->set_pixels(std::string(static_cast<size_t>(width) * height * 3, '\xFF'));
      writer->Write(message);
    }
    return grpc::Status::OK;
  }

 private:
  // The load verdict and, on OK, the registered pages: bytes on the wire are
  // looked up (and cached under the hash the client sent beside them), a
  // bare hash must already be cached.
  std::pair<pdfv1::LoadStatus, std::vector<FakePdfPage>> load(const pdfv1::PdfDocument& document) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (document.data().empty()) {
      const auto cached = by_hash_.find(document.sha256());
      if (cached == by_hash_.end()) return {pdfv1::LOAD_STATUS_BYTES_REQUIRED, {}};
      return {pdfv1::LOAD_STATUS_OK, cached->second};
    }
    const auto known = known_.find(document.data());
    if (known == known_.end()) return {pdfv1::LOAD_STATUS_CORRUPT, {}};
    if (document.has_sha256()) by_hash_[document.sha256()] = known->second;
    return {pdfv1::LOAD_STATUS_OK, known->second};
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::vector<FakePdfPage>> known_;
  std::map<std::string, std::vector<FakePdfPage>> by_hash_;
  int render_calls_ = 0;
};

// Serves a FakePdfBackend on a loopback port and points GRPARSE_PDF_BACKEND
// at it for the object's lifetime; the previous value comes back after.
class ScopedPdfBackend final {
 public:
  ScopedPdfBackend() {
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&backend_);
    server_ = builder.BuildAndStart();
    require(server_ != nullptr && port != 0, "fake PDF backend started");
    target_ = "127.0.0.1:" + std::to_string(port);
    if (const char* previous = std::getenv("GRPARSE_PDF_BACKEND")) previous_ = previous;
    setenv("GRPARSE_PDF_BACKEND", target_.c_str(), 1);
  }

  ~ScopedPdfBackend() {
    if (previous_.has_value()) {
      setenv("GRPARSE_PDF_BACKEND", previous_->c_str(), 1);
    } else {
      unsetenv("GRPARSE_PDF_BACKEND");
    }
    server_->Shutdown();
  }

  ScopedPdfBackend(const ScopedPdfBackend&) = delete;
  ScopedPdfBackend& operator=(const ScopedPdfBackend&) = delete;

  FakePdfBackend& backend() { return backend_; }
  const std::string& target() const { return target_; }

 private:
  FakePdfBackend backend_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
  std::optional<std::string> previous_;
};

}  // namespace grparse_test

#endif  // GRPARSE_TESTS_SUPPORT_FAKE_PDF_BACKEND_H
