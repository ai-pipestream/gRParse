#include "grparse/remote_page_source.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/imgproc.hpp>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "targets/sha256.h"

namespace grparse {
namespace {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

// Same limits as the collector channels: a page raster at model DPI must
// fit in one message.
constexpr int kMaxMessageBytes = 520 * 1024 * 1024;
constexpr double kPdfUserSpaceDpi = 72.0;

// A hung backend must not hang the document; consensus mode multiplies
// the exposure, so each RPC carries a deadline sized to its work, or the
// request's own deadline when that comes sooner.
constexpr auto kProbeDeadline = std::chrono::seconds(30);
constexpr auto kParseDeadline = std::chrono::seconds(300);
constexpr auto kRenderDeadline = std::chrono::seconds(600);

// The content-addressed handshake: calls go out with PdfDocument.sha256 set
// and data empty (a cache lookup), and only a LOAD_STATUS_BYTES_REQUIRED
// verdict earns exactly one retry carrying the bytes. GRPARSE_PDF_BACKEND_
// HANDSHAKE=off pins the pre-handshake behavior of sending the bytes with
// every call; read per opened document, the way GRPARSE_PDF_BACKEND itself
// is read.
bool handshake_enabled() {
  const char* configured = std::getenv("GRPARSE_PDF_BACKEND_HANDSHAKE");
  return configured == nullptr || std::string_view(configured) != "off";
}

// Born-digital coverage gate: skip OCR only when the native text layer
// looks real.
constexpr size_t kMinDigitalNonWhitespace = 32;
constexpr size_t kMinDigitalLines = 4;
constexpr double kMinDigitalVerticalCoverage = 0.12;
constexpr size_t kStrongDigitalNonWhitespace = 128;

// A failed backend call. Outages, deadlines and cancellation keep their
// gRPC code (PdfBackendUnavailable); anything else is the backend refusing
// the document, which stays INVALID_ARGUMENT.
[[noreturn]] void throw_backend_failure(const std::string& what, const grpc::Status& status) {
  const std::string message = what + ": " + status.error_message();
  switch (status.error_code()) {
    case grpc::StatusCode::UNAVAILABLE:
      throw PdfBackendUnavailable(message, PdfBackendFailure::kUnavailable);
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      throw PdfBackendUnavailable(message, PdfBackendFailure::kDeadlineExceeded);
    case grpc::StatusCode::CANCELLED:
      throw PdfBackendUnavailable(message, PdfBackendFailure::kCancelled);
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      throw PdfBackendUnavailable(message, PdfBackendFailure::kResourceExhausted);
    default:
      throw InvalidDocument(message);
  }
}

// Maps contract page space into the top-left frame of the page as rendered.
// The contract's boxes are PDF user space, bottom-left origin, before the
// page's /Rotate; the rendered page is the CropBox with /Rotate applied, so
// a box shifts by the CropBox origin, flips to a top-left origin, and turns
// clockwise with the page.
class PageFrame {
 public:
  explicit PageFrame(const pdfv1::PageInfo& info)
      : rotation_(((info.rotation_degrees() % 360) + 360) % 360) {
    const bool quarter_turn = rotation_ == 90 || rotation_ == 270;
    // Unrotated extent: the visible box, which is the CropBox clipped to
    // the MediaBox (a CropBox reaching past the MediaBox is legal, and
    // renderers draw only the overlap), or the MediaBox alone. Without
    // either, the rendered size turned back.
    std::optional<pdfv1::BoundingBox> visible;
    if (valid(info.media_box())) visible = info.media_box();
    if (valid(info.crop_box())) {
      pdfv1::BoundingBox crop = info.crop_box();
      if (visible.has_value()) {
        crop.set_x0(std::max(crop.x0(), visible->x0()));
        crop.set_y0(std::max(crop.y0(), visible->y0()));
        crop.set_x1(std::min(crop.x1(), visible->x1()));
        crop.set_y1(std::min(crop.y1(), visible->y1()));
      }
      if (valid(crop)) visible = crop;
    }
    if (visible.has_value()) {
      origin_x_ = visible->x0();
      origin_y_ = visible->y0();
      width_ = visible->x1() - visible->x0();
      height_ = visible->y1() - visible->y0();
    } else {
      width_ = quarter_turn ? info.height_pts() : info.width_pts();
      height_ = quarter_turn ? info.width_pts() : info.height_pts();
    }
  }

  // Width and height of the rendered page, in points.
  double display_width() const { return rotation_ % 180 == 0 ? width_ : height_; }
  double display_height() const { return rotation_ % 180 == 0 ? height_ : width_; }

  // The axis-aligned box in the rendered top-left frame: {left, top, right,
  // bottom} in points.
  std::array<double, 4> place(const pdfv1::BoundingBox& box) const {
    const auto a = to_display(box.x0(), box.y0());
    const auto b = to_display(box.x1(), box.y1());
    return {std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[0], b[0]),
            std::max(a[1], b[1])};
  }

 private:
  std::array<double, 2> to_display(double x, double y) const {
    const double u = x - origin_x_;
    const double down = height_ - (y - origin_y_);
    switch (rotation_) {
      case 90:
        return {height_ - down, u};
      case 180:
        return {width_ - u, height_ - down};
      case 270:
        return {down, width_ - u};
      default:
        return {u, down};
    }
  }

  static bool valid(const pdfv1::BoundingBox& box) {
    return box.x1() > box.x0() && box.y1() > box.y0();
  }

  int rotation_;
  double origin_x_ = 0.0;
  double origin_y_ = 0.0;
  double width_ = 0.0;
  double height_ = 0.0;
};

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
        handshake_(handshake_enabled()),
        sha256_(handshake_ ? targets::sha256_hex(*bytes_) : std::string()),
        render_dpi_(render_dpi),
        render_scale_(render_dpi / kPdfUserSpaceDpi),
        stub_(pdfv1::PdfBackendService::NewStub(channel_for(target))) {
    // A hash-only Probe is a cache lookup; a miss earns exactly one retry
    // with the bytes attached so the backend can cache them under the hash.
    pdfv1::ProbeResponse response;
    bool sent_bytes = !handshake_;
    for (;;) {
      Call call(*this, kProbeDeadline);
      pdfv1::ProbeRequest request;
      fill_document(request.mutable_document(), sent_bytes);
      response.Clear();
      const grpc::Status status = stub_->Probe(call.context(), request, &response);
      if (!status.ok()) {
        throw_backend_failure("PDF backend unreachable", status);
      }
      if (response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_BYTES_REQUIRED &&
          !sent_bytes) {
        sent_bytes = true;
        continue;
      }
      break;
    }
    const auto& caps = response.capabilities();
    if (caps.load_status() != pdfv1::LOAD_STATUS_OK) {
      // A BYTES_REQUIRED here means the backend kept asking for bytes after
      // receiving them; a HASH_MISMATCH means the bytes did not hash to the
      // value the client computed itself. Both are hard errors, as is every
      // other non-OK verdict.
      throw InvalidDocument(
          "PDF backend could not load the document: " +
          pdfv1::LoadStatus_Name(caps.load_status()) +
          (caps.has_load_detail() ? " (" + caps.load_detail() + ")" : ""));
    }
    pages_ = static_cast<int>(caps.page_count());
    if (pages_ <= 0) throw InvalidDocument("PDF does not contain a renderable page");
    backend_name_ = caps.backend_name();
  }

  int page_count() const override { return pages_; }

  std::string backend_name() const override { return backend_name_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    check_page(page_number);
    bool sent_bytes = !handshake_;
    for (;;) {
      Call call(*this, kParseDeadline);
      pdfv1::ParseRequest request;
      fill_document(request.mutable_document(), sent_bytes);
      request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
      request.add_families(pdfv1::PDF_FAMILY_FONTS);
      auto* range = request.mutable_pages();
      range->set_begin(static_cast<uint32_t>(page_number - 1));
      range->set_end(static_cast<uint32_t>(page_number));
      auto reader = stub_->Parse(call.context(), request);

      pdfv1::ParseResponse message;
      std::optional<PageFrame> frame;
      std::optional<pdfv1::LoadStatus> header_load_status;
      std::string header_load_detail;
      std::map<uint32_t, std::string> font_names;
      std::vector<pdfv1::TextCell> cells;
      while (reader->Read(&message)) {
        switch (message.payload_case()) {
          case pdfv1::ParseResponse::kHeader:
            header_load_status = message.header().capabilities().load_status();
            header_load_detail = message.header().capabilities().load_detail();
            for (const auto& info : message.header().pages()) {
              if (info.page_index() == static_cast<uint32_t>(page_number - 1)) {
                frame.emplace(info);
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
        throw_backend_failure("PDF backend parse failed", status);
      }
      if (header_load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED &&
          !sent_bytes) {
        // Cache miss on a hash-only request: upload the bytes, once.
        sent_bytes = true;
        continue;
      }
      if (header_load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED ||
          header_load_status == pdfv1::LOAD_STATUS_HASH_MISMATCH) {
        // Bytes were already on the wire, or the bytes did not hash to the
        // value the client computed itself: a hard error, not a retry.
        throw InvalidDocument(
            "PDF backend could not load the document: " +
            pdfv1::LoadStatus_Name(*header_load_status) +
            (header_load_detail.empty() ? "" : " (" + header_load_detail + ")"));
      }
      if (!frame.has_value() || frame->display_height() <= 0.0) return std::nullopt;

      OcrPage result;
      result.width = scaled(frame->display_width());
      result.height = scaled(frame->display_height());
      result.source = OcrPage::Source::kDigitalPdf;
      size_t non_whitespace_bytes = 0;
      double text_top = frame->display_height();
      double text_bottom = 0.0;
      result.lines.reserve(cells.size());
      for (const auto& cell : cells) {
        if (cell.text().empty()) continue;
        for (const unsigned char byte : cell.text()) {
          if (std::isspace(byte) == 0) ++non_whitespace_bytes;
        }
        // The fold works in the top-left frame of the rendered page.
        const auto [left_pts, top_pts, right_pts, bottom_pts] = frame->place(cell.bbox());
        text_top = std::min(text_top, top_pts);
        text_bottom = std::max(text_bottom, bottom_pts);
        const int left = scaled(left_pts);
        const int top = scaled(top_pts);
        const int right = scaled(right_pts);
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
          (text_bottom - text_top) / frame->display_height();
      result.skip_ocr = non_whitespace_bytes >= kMinDigitalNonWhitespace &&
                        result.lines.size() >= kMinDigitalLines &&
                        (vertical_coverage >= kMinDigitalVerticalCoverage ||
                         non_whitespace_bytes >= kStrongDigitalNonWhitespace);
      return result;
    }
  }

  void set_deadline(std::chrono::system_clock::time_point deadline) override {
    const std::lock_guard<std::mutex> lock(calls_mutex_);
    deadline_ = deadline;
  }

  void cancel() override {
    const std::lock_guard<std::mutex> lock(calls_mutex_);
    cancelled_ = true;
    for (grpc::ClientContext* context : calls_) context->TryCancel();
  }

  cv::Mat render_page(int page_number) const override {
    check_page(page_number);
    bool sent_bytes = !handshake_;
    for (;;) {
      Call call(*this, kRenderDeadline);
      pdfv1::RenderRequest request;
      fill_document(request.mutable_document(), sent_bytes);
      request.set_dpi(render_dpi_);
      request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
      auto* range = request.mutable_pages();
      range->set_begin(static_cast<uint32_t>(page_number - 1));
      range->set_end(static_cast<uint32_t>(page_number));
      auto reader = stub_->Render(call.context(), request);

      pdfv1::RenderResponse message;
      cv::Mat rendered;
      std::optional<pdfv1::LoadStatus> head_load_status;
      std::string head_load_detail;
      while (reader->Read(&message)) {
        // A load verdict arrives as the one-message head of the stream;
        // on success a server sends rasters only.
        if (message.has_head()) {
          head_load_status = message.head().load_status();
          head_load_detail = message.head().load_detail();
          continue;
        }
        const auto& raster = message.raster();
        if (raster.width_px() == 0 || raster.height_px() == 0) continue;
        if (raster.page_index() != static_cast<uint32_t>(page_number - 1)) {
          throw InvalidDocument("PDF backend answered with a raster for page " +
                                std::to_string(raster.page_index() + 1) + " instead of page " +
                                std::to_string(page_number));
        }
        rendered = at_render_dpi(to_bgr(raster_view(raster), raster.pixel_format()),
                                 raster.dpi());
      }
      const grpc::Status status = reader->Finish();
      if (!status.ok()) {
        throw_backend_failure("PDF backend render failed", status);
      }
      if (head_load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED &&
          !sent_bytes) {
        // Cache miss on a hash-only request: upload the bytes, once.
        sent_bytes = true;
        continue;
      }
      if (head_load_status.has_value() &&
          *head_load_status != pdfv1::LOAD_STATUS_OK) {
        // A typed load failure (a second BYTES_REQUIRED, a HASH_MISMATCH,
        // or any engine verdict) fails the page the way every other Render
        // failure does.
        throw InvalidDocument(
            "PDF backend could not load the document: " +
            pdfv1::LoadStatus_Name(*head_load_status) +
            (head_load_detail.empty() ? "" : " (" + head_load_detail + ")"));
      }
      if (rendered.empty()) {
        throw InvalidDocument("PDF page could not be rendered by the backend");
      }
      return rendered;
    }
  }

 private:
  // One backend call's context, registered with the source so cancel()
  // reaches it in flight. Its deadline is the sooner of the call's own
  // budget and the request's; after cancel() no new call goes out.
  class Call {
   public:
    Call(const RemotePdfPageSource& source, std::chrono::seconds budget) : source_(source) {
      const std::lock_guard<std::mutex> lock(source_.calls_mutex_);
      if (source_.cancelled_) {
        throw PdfBackendUnavailable("PDF backend call cancelled", PdfBackendFailure::kCancelled);
      }
      context_.set_deadline(std::min(source_.deadline_, std::chrono::system_clock::now() + budget));
      source_.calls_.insert(&context_);
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    ~Call() {
      const std::lock_guard<std::mutex> lock(source_.calls_mutex_);
      source_.calls_.erase(&context_);
    }
    grpc::ClientContext* context() { return &context_; }

   private:
    const RemotePdfPageSource& source_;
    grpc::ClientContext context_;
  };

  void check_page(int page_number) const {
    if (page_number < 1 || page_number > pages_) {
      throw InvalidDocument("PDF page number is out of range");
    }
  }

  // Fills the request's document slot. With the handshake on, `sent_bytes`
  // false addresses the document by hash only (a cache lookup); true
  // attaches the bytes once so the backend can cache them under the hash.
  void fill_document(pdfv1::PdfDocument* document, bool sent_bytes) const {
    if (handshake_) document->set_sha256(sha256_);
    if (!handshake_ || sent_bytes) document->set_data(*bytes_);
  }

  int scaled(double user_space_units) const {
    return static_cast<int>(std::lround(user_space_units * render_scale_));
  }

  // The raster is the backend's word on its own layout; a short or
  // inconsistent one is a failed page (InvalidDocument, so consensus falls
  // over to the next leg), never an out-of-bounds read.
  static cv::Mat raster_view(const pdfv1::PageRaster& raster) {
    int channels = 0;
    switch (raster.pixel_format()) {
      case pdfv1::PIXEL_FORMAT_GRAY8:
        channels = 1;
        break;
      case pdfv1::PIXEL_FORMAT_RGB8:
      case pdfv1::PIXEL_FORMAT_BGR8:
        channels = 3;
        break;
      case pdfv1::PIXEL_FORMAT_RGBA8:
      case pdfv1::PIXEL_FORMAT_BGRA8:
        channels = 4;
        break;
      default:
        throw InvalidDocument("PDF backend answered with an unknown pixel format");
    }
    constexpr uint64_t kMaxDim = static_cast<uint64_t>(std::numeric_limits<int>::max());
    const uint64_t width = raster.width_px();
    const uint64_t height = raster.height_px();
    const uint64_t stride = raster.stride_bytes();
    if (width > kMaxDim || height > kMaxDim || stride < width * channels ||
        raster.pixels().size() < height * stride) {
      throw InvalidDocument("PDF backend answered with a malformed raster (" +
                            std::to_string(width) + "x" + std::to_string(height) + ", stride " +
                            std::to_string(stride) + ", " +
                            std::to_string(raster.pixels().size()) + " bytes)");
    }
    return cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC(channels),
                   const_cast<char*>(raster.pixels().data()), stride);
  }

  // Digital-text boxes scale by render_dpi/72, so a raster a backend
  // rendered at another DPI (one that clamps it, say) is resized to the
  // requested one; boxes and pixels then share one frame. A raster without
  // a stated DPI is taken as rendered at the requested one.
  cv::Mat at_render_dpi(cv::Mat image, double raster_dpi) const {
    if (!(raster_dpi > 0.0) || !std::isfinite(raster_dpi)) return image;
    const double factor = render_dpi_ / raster_dpi;
    const double width = std::round(image.cols * factor);
    const double height = std::round(image.rows * factor);
    if (std::abs(width - image.cols) <= 1.0 && std::abs(height - image.rows) <= 1.0) return image;
    // A raster's stated DPI is the backend's word; a tiny one would scale a
    // small raster to an allocation the size of the whole budget, so the
    // resized page is held to the same pixel cap as a decoded image.
    if (width < 1.0 || height < 1.0 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max() ||
        width * height > static_cast<double>(max_image_pixels())) {
      throw InvalidDocument("PDF backend raster DPI is out of range");
    }
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(static_cast<int>(width), static_cast<int>(height)), 0, 0,
               factor < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return resized;
  }

  // The fold consumes BGR (what the raster path decodes to); backends may
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
  const bool handshake_;
  const std::string sha256_;
  const double render_dpi_;
  const double render_scale_;
  std::unique_ptr<pdfv1::PdfBackendService::Stub> stub_;
  int pages_ = 0;
  std::string backend_name_;
  // Guards the request ties: set_deadline() and cancel() arrive from other
  // threads than the page calls.
  mutable std::mutex calls_mutex_;
  std::chrono::system_clock::time_point deadline_ = std::chrono::system_clock::time_point::max();
  bool cancelled_ = false;
  mutable std::set<grpc::ClientContext*> calls_;
};

}  // namespace

std::optional<std::string> remote_pdf_backend_target() {
  const char* value = std::getenv("GRPARSE_PDF_BACKEND");
  if (value == nullptr) return std::nullopt;
  std::string target(value);
  const auto begin = target.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    // The empty value means no backend is configured; a whitespace-only
    // value is a typo that must fail loudly, not dial the literal string.
    if (target.empty()) return std::nullopt;
    throw std::invalid_argument(
        "GRPARSE_PDF_BACKEND is whitespace-only; unset it or name a backend "
        "target");
  }
  const auto end = target.find_last_not_of(" \t");
  target = target.substr(begin, end - begin + 1);
  // The in-process poppler path is gone; dialing a host by that name would
  // fail every PDF at request time instead of at startup. A consensus list
  // is checked entry by entry.
  size_t start = 0;
  bool inprocess = false;
  while (start <= target.size()) {
    const size_t comma = std::min(target.find(',', start), target.size());
    std::string_view entry(target.data() + start, comma - start);
    while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.remove_prefix(1);
    while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.remove_suffix(1);
    if (entry == "inprocess") inprocess = true;
    start = comma + 1;
  }
  if (inprocess) {
    throw std::invalid_argument(
        "GRPARSE_PDF_BACKEND=inprocess is no longer supported: gRParse links "
        "no PDF engine; name a PdfBackendService target such as pdfium:50069");
  }
  return target;
}

std::shared_ptr<PageSource> open_remote_pdf_document(
    std::shared_ptr<const std::string> bytes, const std::string& target,
    double render_dpi) {
  return std::make_shared<RemotePdfPageSource>(std::move(bytes), target,
                                               render_dpi);
}

}  // namespace grparse
