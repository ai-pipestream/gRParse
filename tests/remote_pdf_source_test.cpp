// Exercises the backend-client PDF source against an in-process fake
// PdfBackendService, so the wire conversion (bottom-left points to
// top-left pixels, font table joins, subset stripping, the OCR-skip gate,
// raster format conversion, typed load failures) is covered without a
// real backend running.
#include <cstdlib>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/core.hpp>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/in_memory_document.h"
#include "grparse/remote_page_source.h"
#include "support/check.h"

namespace {

using grparse_test::require;
namespace pdfv1 = ai::protomolt::parse::pdf::v1;

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
    if (cache_miss(request->document())) {
      caps->set_load_status(pdfv1::LOAD_STATUS_BYTES_REQUIRED);
      return grpc::Status::OK;
    }
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

 private:
  // The server side of the handshake in miniature: a hash-only request for
  // bytes never uploaded is a cache miss; an upload caches the hash.
  bool cache_miss(const pdfv1::PdfDocument& document) {
    if (document.data().empty() && document.has_sha256()) {
      return cached_.find(document.sha256()) == cached_.end();
    }
    if (document.has_sha256()) cached_.insert(document.sha256());
    return false;
  }

  std::set<std::string> cached_;
};

// The cells the digital-coverage gate wants, on the requested page.
void write_cells(pdfv1::ParseResponse* page, uint32_t page_index) {
  auto* chunk = page->mutable_page();
  chunk->set_page_index(page_index);
  double y = 700.0;
  for (int i = 0; i < 5; ++i) {
    auto* cell = chunk->add_text_cells();
    cell->set_text("line number " + std::to_string(i) + " of the fixture");
    cell->mutable_bbox()->set_x0(72.0);
    cell->mutable_bbox()->set_y0(y);
    cell->mutable_bbox()->set_x1(300.0);
    cell->mutable_bbox()->set_y1(y + 12.0);
    cell->set_font_size(12.0);
    y -= 150.0;
  }
}

// A backend with the server side of the content-addressed handshake: a
// hash-only request misses with LOAD_STATUS_BYTES_REQUIRED until the bytes
// arrive once, after which the hash alone serves. Every request is counted
// by shape so the test can assert what crossed the wire.
class CachingFakeBackend final : public pdfv1::PdfBackendService::Service {
 public:
  struct WireCount {
    int calls = 0;
    int hash_only = 0;
    int with_bytes = 0;
    size_t data_bytes = 0;
  };

  WireCount probe;
  WireCount parse;
  WireCount render;
  std::string hash_on_lookup;
  std::string hash_on_upload;

  void clear_cache() {
    const std::lock_guard<std::mutex> lock(mutex_);
    cached_.clear();
  }

  void reset_counts() {
    const std::lock_guard<std::mutex> lock(mutex_);
    probe = WireCount{};
    parse = WireCount{};
    render = WireCount{};
    hash_on_lookup.clear();
    hash_on_upload.clear();
  }

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest* request,
                     pdfv1::ProbeResponse* response) override {
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name("caching-fake");
    caps->set_engine_version("test");
    const pdfv1::LoadStatus verdict = admit(request->document(), probe);
    caps->set_load_status(verdict);
    if (verdict == pdfv1::LOAD_STATUS_OK) caps->set_page_count(2);
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest* request,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    const pdfv1::LoadStatus verdict = admit(request->document(), parse);
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_backend_name("caching-fake");
    caps->set_load_status(verdict);
    if (verdict != pdfv1::LOAD_STATUS_OK) {
      writer->Write(header);
      return grpc::Status::OK;
    }
    caps->set_page_count(2);
    for (uint32_t i = 0; i < 2; ++i) {
      auto* info = header.mutable_header()->add_pages();
      info->set_page_index(i);
      info->set_width_pts(kPageWidthPts);
      info->set_height_pts(kPageHeightPts);
    }
    writer->Write(header);

    pdfv1::ParseResponse page;
    write_cells(&page, request->pages().begin());
    writer->Write(page);
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    const pdfv1::LoadStatus verdict = admit(request->document(), render);
    if (verdict != pdfv1::LOAD_STATUS_OK) {
      // A cache miss on Render arrives as the one-message head stream.
      pdfv1::RenderResponse head;
      head.mutable_head()->set_load_status(verdict);
      writer->Write(head);
      return grpc::Status::OK;
    }
    const int width = static_cast<int>(kPageWidthPts * request->dpi() / 72.0);
    const int height = static_cast<int>(kPageHeightPts * request->dpi() / 72.0);
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(request->pages().begin());
    raster->set_width_px(width);
    raster->set_height_px(height);
    raster->set_stride_bytes(width * 3);
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    raster->set_dpi(request->dpi());
    raster->set_pixels(std::string(static_cast<size_t>(width) * height * 3, '\0'));
    writer->Write(msg);
    return grpc::Status::OK;
  }

 private:
  // Counts the request and answers the cache verdict: OK when the bytes are
  // attached (and caches them under the hash), BYTES_REQUIRED on a hash-only
  // miss, OK on a hash-only hit.
  pdfv1::LoadStatus admit(const pdfv1::PdfDocument& document, WireCount& count) {
    const std::lock_guard<std::mutex> lock(mutex_);
    count.calls += 1;
    if (!document.data().empty()) {
      count.with_bytes += 1;
      count.data_bytes += document.data().size();
      if (document.has_sha256()) {
        hash_on_upload = document.sha256();
        cached_.insert(document.sha256());
      }
      return pdfv1::LOAD_STATUS_OK;
    }
    count.hash_only += 1;
    hash_on_lookup = document.sha256();
    require(document.has_sha256() && document.sha256().size() == 64,
            "a hash-only request carries a 64-character hex sha256");
    return cached_.contains(document.sha256()) ? pdfv1::LOAD_STATUS_OK
                                               : pdfv1::LOAD_STATUS_BYTES_REQUIRED;
  }

  std::mutex mutex_;
  std::set<std::string> cached_;
};

// A backend that claims the bytes do not match the hash the client computed.
// The client computed that hash itself, so the verdict is a hard error, not
// a retry.
class HashMismatchBackend final : public pdfv1::PdfBackendService::Service {
 public:
  int parse_calls = 0;
  int render_calls = 0;

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest*,
                     pdfv1::ProbeResponse* response) override {
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name("hash-mismatch-fake");
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest*,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    parse_calls += 1;
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_load_status(pdfv1::LOAD_STATUS_HASH_MISMATCH);
    caps->set_load_detail("fixture declares a mismatch");
    writer->Write(header);
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest*,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    render_calls += 1;
    pdfv1::RenderResponse head;
    head.mutable_head()->set_load_status(pdfv1::LOAD_STATUS_HASH_MISMATCH);
    head.mutable_head()->set_load_detail("fixture declares a mismatch");
    writer->Write(head);
    return grpc::Status::OK;
  }
};

// A backend whose cache never fills: BYTES_REQUIRED even after the bytes
// arrive. The client retries exactly once, then fails hard.
class NeverCachedBackend final : public pdfv1::PdfBackendService::Service {
 public:
  int parse_calls = 0;
  bool probe_misses = false;
  int probe_calls = 0;

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest*,
                     pdfv1::ProbeResponse* response) override {
    probe_calls += 1;
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name("never-cached-fake");
    if (probe_misses) {
      caps->set_load_status(pdfv1::LOAD_STATUS_BYTES_REQUIRED);
      return grpc::Status::OK;
    }
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(1);
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest*,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    parse_calls += 1;
    pdfv1::ParseResponse header;
    header.mutable_header()->mutable_capabilities()->set_load_status(
        pdfv1::LOAD_STATUS_BYTES_REQUIRED);
    writer->Write(header);
    return grpc::Status::OK;
  }
};

// Starts a fake backend on its own port; returns the target. The server
// outlives the caller's scope by reference.
template <typename Service>
std::string serve(Service& backend, std::unique_ptr<grpc::Server>& server) {
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&backend);
  server = builder.BuildAndStart();
  require(server != nullptr && port != 0, "fake backend started");
  return "127.0.0.1:" + std::to_string(port);
}

// Expects the block to raise InvalidDocument naming the load status.
template <typename Fn>
bool throws_load_status(Fn&& fn, const std::string& status_name) {
  try {
    fn();
  } catch (const grparse::InvalidDocument& error) {
    return std::string(error.what()).find(status_name) != std::string::npos;
  }
  return false;
}

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

  // --- Content-addressed handshake ----------------------------------------
  // The client hashes the document once and addresses it by sha256 on every
  // call; a cache miss earns exactly one retry carrying the bytes.
  const auto doc = std::make_shared<const std::string>("%PDF-handshake-fixture");
  {
    CachingFakeBackend caching;
    std::unique_ptr<grpc::Server> caching_server;
    const std::string caching_target = serve(caching, caching_server);

    const auto source = grparse::open_remote_pdf_document(doc, caching_target, dpi);
    require(source->page_count() == 2, "caching fake reports two pages");
    require(caching.probe.calls == 2, "a cache-miss Probe retries once with the bytes");
    require(caching.probe.hash_only == 1 && caching.probe.with_bytes == 1,
            "the first Probe is a hash-only lookup, the retry carries the bytes");
    require(caching.probe.data_bytes == doc->size(), "Probe uploads the document exactly once");
    require(!caching.hash_on_lookup.empty() &&
                caching.hash_on_lookup == caching.hash_on_upload,
            "lookup and upload name the same hash");

    caching.clear_cache();
    const auto page1 = source->extract_digital_page(1);
    require(page1.has_value(), "page one extracts after the Parse cache miss");
    require(caching.parse.calls == 2, "a cache-miss Parse retries once with the bytes");
    require(caching.parse.hash_only == 1 && caching.parse.with_bytes == 1,
            "the first Parse is hash-only, its retry carries the bytes");
    const auto page2 = source->extract_digital_page(2);
    require(page2.has_value(), "page two extracts");
    require(caching.parse.calls == 3, "page two costs one Parse");
    require(caching.parse.with_bytes == 1 && caching.parse.hash_only == 2,
            "the second page's Parse goes by hash alone");

    caching.clear_cache();
    const cv::Mat rendered = source->render_page(1);
    require(!rendered.empty(), "render succeeds after the head cache miss");
    require(caching.render.calls == 2 && caching.render.hash_only == 1 &&
                caching.render.with_bytes == 1,
            "a Render head BYTES_REQUIRED retries once with the bytes");

    // The kill switch restores always-send-bytes, with no hash on the wire.
    setenv("GRPARSE_PDF_BACKEND_HANDSHAKE", "off", 1);
    caching.clear_cache();
    caching.reset_counts();
    const auto plain = grparse::open_remote_pdf_document(doc, caching_target, dpi);
    require(caching.probe.calls == 1 && caching.probe.with_bytes == 1,
            "handshake off sends the bytes on Probe");
    require(plain->extract_digital_page(1).has_value() &&
                plain->extract_digital_page(2).has_value(),
            "handshake off still parses both pages");
    require(caching.parse.calls == 2 && caching.parse.with_bytes == 2 &&
                caching.parse.data_bytes == 2 * doc->size(),
            "handshake off sends the bytes with every Parse");
    unsetenv("GRPARSE_PDF_BACKEND_HANDSHAKE");
    caching_server->Shutdown();
  }

  // HASH_MISMATCH is a hard error on every surface: the client computed the
  // hash itself, so a mismatch can only be corruption or a bug.
  {
    HashMismatchBackend mismatch;
    std::unique_ptr<grpc::Server> mismatch_server;
    const std::string mismatch_target = serve(mismatch, mismatch_server);
    const auto source = grparse::open_remote_pdf_document(doc, mismatch_target, dpi);
    require(throws_load_status([&] { static_cast<void>(source->extract_digital_page(1)); },
                               "HASH_MISMATCH"),
            "a Parse header HASH_MISMATCH raises the typed error");
    require(mismatch.parse_calls == 1, "HASH_MISMATCH is never retried");
    require(throws_load_status([&] { static_cast<void>(source->render_page(1)); },
                               "HASH_MISMATCH"),
            "a Render head HASH_MISMATCH raises the typed error");
    mismatch_server->Shutdown();
  }

  // A second BYTES_REQUIRED after the bytes were sent is a hard error; the
  // client retries exactly once on both the Parse and the Probe surface.
  {
    NeverCachedBackend never_cached;
    std::unique_ptr<grpc::Server> never_cached_server;
    const std::string never_cached_target = serve(never_cached, never_cached_server);
    const auto source = grparse::open_remote_pdf_document(doc, never_cached_target, dpi);
    require(throws_load_status([&] { static_cast<void>(source->extract_digital_page(1)); },
                               "BYTES_REQUIRED"),
            "a second BYTES_REQUIRED on Parse is a hard error");
    require(never_cached.parse_calls == 2, "the Parse retry happens exactly once");
    never_cached.probe_misses = true;
    require(throws_load_status(
                [&] { static_cast<void>(grparse::open_remote_pdf_document(doc, never_cached_target, dpi)); },
                "BYTES_REQUIRED"),
            "a second BYTES_REQUIRED on Probe fails the open");
    require(never_cached.probe_calls == 3, "the Probe retry happens exactly once");
    never_cached_server->Shutdown();
  }

  server->Shutdown();
  std::println("remote-pdf-source-test: all checks passed");
  return 0;
}
