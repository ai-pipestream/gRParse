#include "grparse/remote_page_source.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/imgproc.hpp>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grparse {
namespace {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

// Same limits as the collector channels: a page raster at model DPI must
// fit in one message.
constexpr int kMaxMessageBytes = 520 * 1024 * 1024;
constexpr double kPdfUserSpaceDpi = 72.0;

// A hung backend must not hang the document; consensus mode multiplies
// the exposure, so each RPC carries a deadline sized to its work.
constexpr auto kProbeDeadline = std::chrono::seconds(30);
constexpr auto kParseDeadline = std::chrono::seconds(300);
constexpr auto kRenderDeadline = std::chrono::seconds(600);

void set_deadline(grpc::ClientContext* context, std::chrono::seconds budget) {
  context->set_deadline(std::chrono::system_clock::now() + budget);
}

// Born-digital coverage gate, kept numerically identical to the in-process
// path in in_memory_document.cpp so flipping the backend never changes the
// OCR-skip decision for the same text layer.
constexpr size_t kMinDigitalNonWhitespace = 32;
constexpr size_t kMinDigitalLines = 4;
constexpr double kMinDigitalVerticalCoverage = 0.12;
constexpr size_t kStrongDigitalNonWhitespace = 128;

// One channel per backend target per process; channels multiplex.
std::shared_ptr<grpc::Channel> channel_for(const std::string& target) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<grpc::Channel>> channels;
  const std::lock_guard<std::mutex> lock(mutex);
  auto it = channels.find(target);
  if (it != channels.end()) return it->second;
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(kMaxMessageBytes);
  args.SetMaxSendMessageSize(kMaxMessageBytes);
  auto channel = grpc::CreateCustomChannel(
      target, grpc::InsecureChannelCredentials(), args);
  channels.emplace(target, channel);
  return channel;
}

class RemotePdfPageSource final : public PageSource {
 public:
  RemotePdfPageSource(std::shared_ptr<const std::string> bytes,
                      const std::string& target, double render_dpi)
      : bytes_(std::move(bytes)),
        render_dpi_(render_dpi),
        render_scale_(render_dpi / kPdfUserSpaceDpi),
        stub_(pdfv1::PdfBackendService::NewStub(channel_for(target))) {
    grpc::ClientContext context;
    set_deadline(&context, kProbeDeadline);
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(*bytes_);
    pdfv1::ProbeResponse response;
    const grpc::Status status = stub_->Probe(&context, request, &response);
    if (!status.ok()) {
      throw InvalidDocument("PDF backend unreachable: " +
                            status.error_message());
    }
    const auto& caps = response.capabilities();
    if (caps.load_status() != pdfv1::LOAD_STATUS_OK) {
      throw InvalidDocument(
          "PDF backend could not load the document: " +
          pdfv1::LoadStatus_Name(caps.load_status()) +
          (caps.has_load_detail() ? " (" + caps.load_detail() + ")" : ""));
    }
    pages_ = static_cast<int>(caps.page_count());
    if (pages_ <= 0) throw InvalidDocument("PDF does not contain a renderable page");
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    check_page(page_number);
    grpc::ClientContext context;
    set_deadline(&context, kParseDeadline);
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(*bytes_);
    request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
    request.add_families(pdfv1::PDF_FAMILY_FONTS);
    auto* range = request.mutable_pages();
    range->set_begin(static_cast<uint32_t>(page_number - 1));
    range->set_end(static_cast<uint32_t>(page_number));
    auto reader = stub_->Parse(&context, request);

    pdfv1::ParseResponse message;
    double page_width_pts = 0.0;
    double page_height_pts = 0.0;
    std::map<uint32_t, std::string> font_names;
    std::vector<pdfv1::TextCell> cells;
    while (reader->Read(&message)) {
      switch (message.payload_case()) {
        case pdfv1::ParseResponse::kHeader:
          for (const auto& info : message.header().pages()) {
            if (info.page_index() == static_cast<uint32_t>(page_number - 1)) {
              page_width_pts = info.width_pts();
              page_height_pts = info.height_pts();
            }
          }
          break;
        case pdfv1::ParseResponse::kFonts:
          for (const auto& font : message.fonts().fonts()) {
            font_names[font.font_id()] = font.base_name();
          }
          break;
        case pdfv1::ParseResponse::kPage:
          for (const auto& cell : message.page().text_cells()) {
            cells.push_back(cell);
          }
          break;
        default:
          break;
      }
    }
    const grpc::Status status = reader->Finish();
    if (!status.ok()) {
      throw InvalidDocument("PDF backend parse failed: " +
                            status.error_message());
    }
    if (page_height_pts <= 0.0) return std::nullopt;

    OcrPage result;
    result.width = scaled(page_width_pts);
    result.height = scaled(page_height_pts);
    result.source = OcrPage::Source::kDigitalPdf;
    size_t non_whitespace_bytes = 0;
    double text_top = page_height_pts;
    double text_bottom = 0.0;
    result.lines.reserve(cells.size());
    for (const auto& cell : cells) {
      if (cell.text().empty()) continue;
      for (const unsigned char byte : cell.text()) {
        if (std::isspace(byte) == 0) ++non_whitespace_bytes;
      }
      // Contract boxes are bottom-left origin; the fold works in the
      // top-left raster frame.
      const double top_pts = page_height_pts - cell.bbox().y1();
      const double bottom_pts = page_height_pts - cell.bbox().y0();
      text_top = std::min(text_top, top_pts);
      text_bottom = std::max(text_bottom, bottom_pts);
      const int left = scaled(cell.bbox().x0());
      const int top = scaled(top_pts);
      const int right = scaled(cell.bbox().x1());
      const int bottom = scaled(bottom_pts);
      OcrLine line{cell.text(),
                   {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                   std::nullopt,
                   TextOrigin::kDigitalPdf};
      if (cell.has_font_id()) {
        auto it = font_names.find(cell.font_id());
        if (it != font_names.end() && !it->second.empty()) {
          std::string font = it->second;
          // Embedded subsets carry a random six-letter prefix (ABCDEF+Real).
          if (const auto plus = font.find('+');
              plus != std::string::npos && plus + 1 < font.size()) {
            font.erase(0, plus + 1);
          }
          if (!font.empty()) line.font_name = std::move(font);
        }
      }
      if (cell.has_font_size() && cell.font_size() > 0) {
        line.font_size_pt = cell.font_size();
      }
      result.lines.push_back(std::move(line));
    }
    if (result.lines.empty()) return std::nullopt;

    const double vertical_coverage =
        page_height_pts > 0.0 ? (text_bottom - text_top) / page_height_pts : 0.0;
    result.skip_ocr = non_whitespace_bytes >= kMinDigitalNonWhitespace &&
                      result.lines.size() >= kMinDigitalLines &&
                      (vertical_coverage >= kMinDigitalVerticalCoverage ||
                       non_whitespace_bytes >= kStrongDigitalNonWhitespace);
    return result;
  }

  cv::Mat render_page(int page_number) const override {
    check_page(page_number);
    grpc::ClientContext context;
    set_deadline(&context, kRenderDeadline);
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(*bytes_);
    request.set_dpi(render_dpi_);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto* range = request.mutable_pages();
    range->set_begin(static_cast<uint32_t>(page_number - 1));
    range->set_end(static_cast<uint32_t>(page_number));
    auto reader = stub_->Render(&context, request);

    pdfv1::RenderResponse message;
    cv::Mat rendered;
    while (reader->Read(&message)) {
      const auto& raster = message.raster();
      if (raster.width_px() == 0 || raster.height_px() == 0) continue;
      const cv::Mat view = raster_view(raster);
      rendered = to_bgr(view, raster.pixel_format());
    }
    const grpc::Status status = reader->Finish();
    if (!status.ok()) {
      throw InvalidDocument("PDF backend render failed: " +
                            status.error_message());
    }
    if (rendered.empty()) {
      throw InvalidDocument("PDF page could not be rendered by the backend");
    }
    return rendered;
  }

 private:
  void check_page(int page_number) const {
    if (page_number < 1 || page_number > pages_) {
      throw InvalidDocument("PDF page number is out of range");
    }
  }

  int scaled(double user_space_units) const {
    return static_cast<int>(std::lround(user_space_units * render_scale_));
  }

  static cv::Mat raster_view(const pdfv1::PageRaster& raster) {
    const int channels =
        raster.pixel_format() == pdfv1::PIXEL_FORMAT_GRAY8 ? 1
        : raster.pixel_format() == pdfv1::PIXEL_FORMAT_RGBA8 ||
                raster.pixel_format() == pdfv1::PIXEL_FORMAT_BGRA8
            ? 4
            : 3;
    return cv::Mat(static_cast<int>(raster.height_px()),
                   static_cast<int>(raster.width_px()), CV_8UC(channels),
                   const_cast<char*>(raster.pixels().data()),
                   raster.stride_bytes());
  }

  // The fold consumes BGR (what the in-process path produces); backends may
  // answer in their native layout.
  static cv::Mat to_bgr(const cv::Mat& view, pdfv1::PixelFormat format) {
    cv::Mat out;
    switch (format) {
      case pdfv1::PIXEL_FORMAT_BGR8:
        return view.clone();
      case pdfv1::PIXEL_FORMAT_RGB8:
        cv::cvtColor(view, out, cv::COLOR_RGB2BGR);
        return out;
      case pdfv1::PIXEL_FORMAT_RGBA8:
        cv::cvtColor(view, out, cv::COLOR_RGBA2BGR);
        return out;
      case pdfv1::PIXEL_FORMAT_BGRA8:
        cv::cvtColor(view, out, cv::COLOR_BGRA2BGR);
        return out;
      case pdfv1::PIXEL_FORMAT_GRAY8:
        cv::cvtColor(view, out, cv::COLOR_GRAY2BGR);
        return out;
      default:
        throw InvalidDocument("PDF backend answered with an unknown pixel format");
    }
  }

  std::shared_ptr<const std::string> bytes_;
  const double render_dpi_;
  const double render_scale_;
  std::unique_ptr<pdfv1::PdfBackendService::Stub> stub_;
  int pages_ = 0;
};

}  // namespace

std::optional<std::string> remote_pdf_backend_target() {
  const char* value = std::getenv("GRPARSE_PDF_BACKEND");
  if (value == nullptr) return std::nullopt;
  std::string target(value);
  if (target.empty() || target == "inprocess") return std::nullopt;
  return target;
}

std::shared_ptr<PageSource> open_remote_pdf_document(
    std::shared_ptr<const std::string> bytes, const std::string& target,
    double render_dpi) {
  return std::make_shared<RemotePdfPageSource>(std::move(bytes), target,
                                               render_dpi);
}

}  // namespace grparse
