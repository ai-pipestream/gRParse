#include "grparse/remote_page_source.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <opencv2/imgproc.hpp>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/pdf_page_frame.h"
#include "targets/sha256.h"

namespace grparse {
namespace {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

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

// ParseStream is the default wire: the document goes up once, in chunks,
// and every page request rides the same stream. GRPARSE_PDF_BACKEND_STREAM=
// off pins the unary Probe/Parse/Render calls; read per opened document,
// like the handshake switch.
bool stream_enabled() {
  const char* configured = std::getenv("GRPARSE_PDF_BACKEND_STREAM");
  return configured == nullptr || std::string_view(configured) != "off";
}

// Upload frame size: the collector streams' (collectors/collector_support.h),
// small enough to interleave with the page requests that share the stream's
// write side.
constexpr size_t kStreamChunkBytes = 256U * 1024U;

// How long closing a session waits for the backend's ParseStreamDone after
// the half-close before cancelling the call.
constexpr auto kStreamCloseGrace = std::chrono::seconds(5);

// Says once per target that a backend answered ParseStream UNIMPLEMENTED,
// so an operator sees the fleet still has a backend on the old contract;
// every source also records it in backend_protocol().
void report_stream_fallback(const std::string& target) {
  static std::mutex mutex;
  static std::set<std::string> reported;
  const std::lock_guard<std::mutex> lock(mutex);
  if (!reported.insert(target).second) return;
  std::println(stderr,
               "PDF backend {} does not serve ParseStream (UNIMPLEMENTED); using "
               "Probe/Parse/Render for its documents",
               target);
}

// The answer to one ParseStream request.
struct StreamAnswer {
  std::vector<pdfv1::ParseStreamResponse> messages;
  // How the request ended; unset when the stream ended first.
  std::optional<pdfv1::RequestDone> done;
  // The stream's status when it ended before the request did.
  grpc::Status stream_status;
};

// One open ParseStream: the document uploads on its own thread while a
// reader thread files every response under its request_id, so page requests
// from any thread share the stream and the bytes go up once. This is the
// ConcurrentUpload pattern of the collector clients, except that the upload
// does not half-close: the stream stays open for the page requests that
// follow, and Close() half-closes it.
class ParseStreamSession {
 public:
  ParseStreamSession(pdfv1::PdfBackendService::Stub* stub,
                     std::shared_ptr<const std::string> bytes,
                     const pdfv1::ParseStreamOpen& open,
                     std::chrono::system_clock::time_point deadline)
      : bytes_(std::move(bytes)) {
    context_.set_deadline(deadline);
    pending_[0];  // the open's own answer
    stream_ = stub->ParseStream(&context_);
    reader_ = std::thread([this] { read_loop(); });
    upload_ = std::thread([this, open] { upload(open); });
  }
  ParseStreamSession(const ParseStreamSession&) = delete;
  ParseStreamSession& operator=(const ParseStreamSession&) = delete;
  ~ParseStreamSession() { close(); }

  grpc::ClientContext* context() { return &context_; }

  uint32_t next_id() { return next_id_.fetch_add(1); }

  bool ended() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
  }

  // Registers id's answer slot and sends the request. A request sent after
  // the stream ended is answered by Await with the stream's status.
  void send(uint32_t id, const pdfv1::ParseStreamRequest& request) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (ended_) return;
      pending_[id];
    }
    write(request);
  }

  // Waits for request id to end, or for the stream to. False when `until`
  // passed first; the caller then abandons the request.
  bool await(uint32_t id, std::chrono::system_clock::time_point until, StreamAnswer* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool settled = changed_.wait_until(lock, until, [&] {
      const auto it = pending_.find(id);
      return ended_ || (it != pending_.end() && it->second.done.has_value());
    });
    if (!settled) return false;
    auto it = pending_.find(id);
    if (it != pending_.end()) {
      *out = std::move(it->second);
      pending_.erase(it);
    }
    if (!out->done.has_value()) out->stream_status = status_;
    return true;
  }

  // Stops a request its caller gave up on; its late messages are dropped.
  void abandon(uint32_t id) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(id);
    }
    pdfv1::ParseStreamRequest cancel;
    cancel.mutable_cancel()->set_request_id(id);
    write(cancel);
  }

 private:
  bool write(const pdfv1::ParseStreamRequest& message) {
    const std::lock_guard<std::mutex> lock(write_mutex_);
    if (writes_closed_) return false;
    if (!stream_->Write(message)) writes_closed_ = true;
    return !writes_closed_;
  }

  void upload(const pdfv1::ParseStreamOpen& open) {
    pdfv1::ParseStreamRequest message;
    *message.mutable_open() = open;
    if (!write(message)) return;
    const std::string& bytes = *bytes_;
    size_t offset = 0;
    do {
      const size_t length = std::min(kStreamChunkBytes, bytes.size() - offset);
      message.Clear();
      auto* chunk = message.mutable_chunk();
      chunk->set_offset(offset);
      chunk->set_data(bytes.data() + offset, length);
      offset += length;
      chunk->set_last(offset >= bytes.size());
      if (!write(message)) return;
    } while (offset < bytes.size());
    uploaded_.store(true);
  }

  void read_loop() {
    pdfv1::ParseStreamResponse message;
    while (stream_->Read(&message)) {
      if (message.has_done()) continue;
      const std::lock_guard<std::mutex> lock(mutex_);
      auto it = pending_.find(message.request_id());
      if (it == pending_.end()) continue;
      if (message.has_request_done()) {
        it->second.done = message.request_done();
        changed_.notify_all();
      } else {
        it->second.messages.push_back(std::move(message));
      }
    }
    grpc::Status status;
    {
      // Finish must not race a Write: the write side closes first.
      const std::lock_guard<std::mutex> lock(write_mutex_);
      writes_closed_ = true;
      status = stream_->Finish();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    ended_ = true;
    status_ = status;
    changed_.notify_all();
  }

  // Half-closes once the upload is through and lets the backend end the
  // stream; an upload still running, or a backend that does not end the
  // stream in time, is cancelled.
  void close() {
    if (!uploaded_.load()) context_.TryCancel();
    if (upload_.joinable()) upload_.join();
    {
      const std::lock_guard<std::mutex> lock(write_mutex_);
      if (!writes_closed_) {
        stream_->WritesDone();
        writes_closed_ = true;
      }
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!changed_.wait_for(lock, kStreamCloseGrace, [this] { return ended_; })) {
        context_.TryCancel();
      }
    }
    if (reader_.joinable()) reader_.join();
  }

  std::shared_ptr<const std::string> bytes_;
  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderWriter<pdfv1::ParseStreamRequest, pdfv1::ParseStreamResponse>>
      stream_;
  std::mutex write_mutex_;
  bool writes_closed_ = false;
  std::atomic<bool> uploaded_{false};
  std::atomic<uint32_t> next_id_{1};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<uint32_t, StreamAnswer> pending_;
  bool ended_ = false;
  grpc::Status status_;
  std::thread reader_;
  std::thread upload_;
};

// What one page's Parse answer comes to, from the unary stream or a
// ParseStream request alike.
struct ParsedPage {
  std::optional<PdfPageFrame> frame;
  std::optional<pdfv1::LoadStatus> load_status;
  std::string load_detail;
  std::map<uint32_t, std::string> font_names;
  std::vector<pdfv1::TextCell> cells;
  std::vector<pdfv1::BoundingBox> images;

  void absorb(const pdfv1::ParseResponse& message, int page_number) {
    switch (message.payload_case()) {
      case pdfv1::ParseResponse::kHeader:
        load_status = message.header().capabilities().load_status();
        load_detail = message.header().capabilities().load_detail();
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
        for (const auto& image : message.page().images()) {
          images.push_back(image.bbox());
        }
        break;
      default:
        break;
    }
  }
};

class RemotePdfPageSource final : public PageSource {
 public:
  RemotePdfPageSource(std::shared_ptr<const std::string> bytes,
                      const std::string& target, double render_dpi,
                      std::string precomputed_sha256)
      : bytes_(std::move(bytes)),
        handshake_(handshake_enabled()),
        sha256_(handshake_ ? (precomputed_sha256.empty()
                                  ? targets::sha256_hex(*bytes_)
                                  : std::move(precomputed_sha256))
                           : std::string()),
        render_dpi_(render_dpi),
        render_scale_(render_dpi / kPdfUserSpaceDpi),
        target_(target),
        protocol_(stream_enabled() ? PdfBackendProtocol::kCurrent
                                   : PdfBackendProtocol::kLegacyByConfig),
        stub_(pdfv1::PdfBackendService::NewStub(pdf_backend_channel(target))) {}

  ~RemotePdfPageSource() override { release_session(); }

  RemotePdfPageSource(const RemotePdfPageSource&) = delete;
  RemotePdfPageSource& operator=(const RemotePdfPageSource&) = delete;

  PdfBackendProtocol backend_protocol() const { return protocol_.load(); }

  int page_count() const override {
    open();
    return pages_;
  }

  std::string backend_name() const override {
    open();
    return backend_name_;
  }

  void set_passwords(const DocumentPasswords& passwords) override {
    const std::lock_guard<std::mutex> lock(open_mutex_);
    candidates_ = passwords;
  }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    check_page(page_number);
    if (on_stream()) {
      pdfv1::ParseStreamRequest request;
      auto* parse = request.mutable_parse();
      add_page_families(parse->mutable_families());
      parse->mutable_pages()->set_begin(static_cast<uint32_t>(page_number - 1));
      parse->mutable_pages()->set_end(static_cast<uint32_t>(page_number));
      const StreamAnswer answer =
          stream_call(std::move(request), kParseDeadline, "PDF backend parse failed");
      ParsedPage parsed;
      for (const auto& message : answer.messages) {
        if (message.has_parse()) parsed.absorb(message.parse(), page_number);
      }
      check_parse_load(parsed);
      return digital_page(std::move(parsed));
    }
    bool sent_bytes = !handshake_;
    for (;;) {
      Call call(*this, kParseDeadline);
      pdfv1::ParseRequest request;
      fill_document(request.mutable_document(), sent_bytes);
      add_page_families(request.mutable_families());
      auto* range = request.mutable_pages();
      range->set_begin(static_cast<uint32_t>(page_number - 1));
      range->set_end(static_cast<uint32_t>(page_number));
      auto reader = stub_->Parse(call.context(), request);

      pdfv1::ParseResponse message;
      ParsedPage parsed;
      while (reader->Read(&message)) parsed.absorb(message, page_number);
      const grpc::Status status = reader->Finish();
      if (!status.ok()) {
        throw_backend_failure("PDF backend parse failed", status);
      }
      if (parsed.load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED &&
          !sent_bytes) {
        // Cache miss on a hash-only request: upload the bytes, once.
        sent_bytes = true;
        continue;
      }
      check_parse_load(parsed);
      return digital_page(std::move(parsed));
    }
  }

  // Families a page's digital text needs: cells, the font table, and image
  // placements only (no pixels): where the page's rasters sit, for OCR.
  static void add_page_families(google::protobuf::RepeatedField<int>* families) {
    families->Add(pdfv1::PDF_FAMILY_TEXT_CELLS);
    families->Add(pdfv1::PDF_FAMILY_FONTS);
    families->Add(pdfv1::PDF_FAMILY_PLACED_IMAGES);
  }

  // A BYTES_REQUIRED that survived its one retry (or that a stream, which
  // always carries the bytes, should never see) and a HASH_MISMATCH are
  // hard errors.
  static void check_parse_load(const ParsedPage& parsed) {
    if (parsed.load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED ||
        parsed.load_status == pdfv1::LOAD_STATUS_HASH_MISMATCH) {
      // Bytes were already on the wire, or the bytes did not hash to the
      // value the client computed itself: a hard error, not a retry.
      throw InvalidDocument(
          "PDF backend could not load the document: " +
          pdfv1::LoadStatus_Name(*parsed.load_status) +
          (parsed.load_detail.empty() ? "" : " (" + parsed.load_detail + ")"));
    }
  }

  std::optional<OcrPage> digital_page(ParsedPage parsed) const {
    {
      auto& frame = parsed.frame;
      auto& cells = parsed.cells;
      auto& images = parsed.images;
      auto& font_names = parsed.font_names;
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
        // The fold works in the top-left frame of the rendered page. A cell
        // the file put wholly off the page (a slide's footer placeholder
        // below the sheet, a title above it) is seen by no reader and is
        // left out; one across the page edge is cut at the edge, which is
        // where it is seen.
        auto [left_pts, top_pts, right_pts, bottom_pts] = frame->place(cell.bbox());
        const double page_width = frame->display_width();
        const double page_height = frame->display_height();
        if (right_pts <= 0.0 || left_pts >= page_width || bottom_pts <= 0.0 ||
            top_pts >= page_height) {
          continue;
        }
        left_pts = std::clamp(left_pts, 0.0, page_width);
        right_pts = std::clamp(right_pts, 0.0, page_width);
        top_pts = std::clamp(top_pts, 0.0, page_height);
        bottom_pts = std::clamp(bottom_pts, 0.0, page_height);
        for (const unsigned char byte : cell.text()) {
          if (std::isspace(byte) == 0) ++non_whitespace_bytes;
        }
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
      for (const auto& bbox : images) {
        auto [left_pts, top_pts, right_pts, bottom_pts] = frame->place(bbox);
        const int left = scaled(std::clamp(left_pts, 0.0, frame->display_width()));
        const int top = scaled(std::clamp(top_pts, 0.0, frame->display_height()));
        const int right = scaled(std::clamp(right_pts, 0.0, frame->display_width()));
        const int bottom = scaled(std::clamp(bottom_pts, 0.0, frame->display_height()));
        if (right > left && bottom > top) result.image_placements.emplace_back(left, top, right - left, bottom - top);
      }

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
    if (on_stream()) {
      pdfv1::ParseStreamRequest request;
      auto* render = request.mutable_render();
      render->set_dpi(render_dpi_);
      render->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
      render->mutable_pages()->set_begin(static_cast<uint32_t>(page_number - 1));
      render->mutable_pages()->set_end(static_cast<uint32_t>(page_number));
      const StreamAnswer answer =
          stream_call(std::move(request), kRenderDeadline, "PDF backend render failed");
      cv::Mat rendered;
      for (const auto& message : answer.messages) {
        if (!message.has_render()) continue;
        const auto& frame = message.render();
        if (frame.has_head() && frame.head().load_status() != pdfv1::LOAD_STATUS_OK) {
          throw InvalidDocument(
              "PDF backend could not load the document: " +
              pdfv1::LoadStatus_Name(frame.head().load_status()) +
              (frame.head().load_detail().empty() ? "" : " (" + frame.head().load_detail() + ")"));
        }
        if (frame.has_raster()) rendered = page_raster(frame.raster(), page_number);
      }
      if (rendered.empty()) {
        throw InvalidDocument("PDF page could not be rendered by the backend");
      }
      return rendered;
    }
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
        cv::Mat page = page_raster(message.raster(), page_number);
        if (!page.empty()) rendered = std::move(page);
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

  // The opening Probe, made once on first use rather than in the
  // constructor, so a caller can tie the source to its request
  // (set_deadline(), cancel()) before any backend call: the Probe then
  // honors the request's deadline and cancellation like every later call.
  // A failed open is remembered and every later use fails the same way.
  void open() const {
    const std::lock_guard<std::mutex> lock(open_mutex_);
    if (open_failure_) std::rethrow_exception(open_failure_);
    if (pages_ > 0) return;
    try {
      probe();
    } catch (...) {
      open_failure_ = std::current_exception();
      throw;
    }
  }

  void probe() const {
    pdfv1::ProbeResponse response = open_with(nullptr);
    // A document that will not open without a password tries the request's
    // candidates in order; the first that opens it is sent on every later
    // call, since the backend loads the document afresh each time.
    if (password_status(response.capabilities().load_status()) && !candidates_.empty()) {
      for (const std::string& candidate : candidates_.candidates) {
        response = open_with(&candidate);
        if (response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK) {
          password_ = candidate;
          break;
        }
        if (!password_status(response.capabilities().load_status())) break;
      }
    }
    const auto& caps = response.capabilities();
    if (password_status(caps.load_status())) {
      // One verdict whatever was tried, so a client branches on a single
      // token; how many candidates failed is said, never which, and the
      // backend's detail on a failed attempt is left out.
      std::string why;
      if (!candidates_.empty()) {
        why = " (" + password_attempt_clause(candidates_) + ")";
      } else if (caps.has_load_detail()) {
        why = " (" + caps.load_detail() + ")";
      }
      throw InvalidDocument("PDF backend could not load the document: " +
                            pdfv1::LoadStatus_Name(pdfv1::LOAD_STATUS_PASSWORD_REQUIRED) + why);
    }
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

  // The opening call, with `password` when one is given: a ParseStream
  // whose open answers with the header alone, which uploads the document
  // and keeps the stream for every later page call; or, when the backend
  // predates ParseStream (UNIMPLEMENTED) or GRPARSE_PDF_BACKEND_STREAM=off,
  // the Probe of the unary calls. A fallback is recorded in
  // backend_protocol() and reported once per target, never silent.
  pdfv1::ProbeResponse open_with(const std::string* password) const {
    if (protocol_.load() == PdfBackendProtocol::kCurrent) {
      std::shared_ptr<ParseStreamSession> session;
      if (const std::optional<pdfv1::ParseHeader> header = start_session(password, &session)) {
        pdfv1::ProbeResponse response;
        *response.mutable_capabilities() = header->capabilities();
        auto* caps = response.mutable_capabilities();
        if (caps->load_status() == pdfv1::LOAD_STATUS_OK && caps->page_count() == 0) {
          caps->set_page_count(static_cast<uint32_t>(header->pages_size()));
        }
        if (session != nullptr) {
          const std::lock_guard<std::mutex> lock(session_mutex_);
          retire(std::move(session_));
          session_ = std::move(session);
        }
        return response;
      }
      protocol_.store(PdfBackendProtocol::kLegacyFallback);
      report_stream_fallback(target_);
    }
    return probe_with(password);
  }

  // Opens a ParseStream session and waits for the open's header. Returns
  // nullopt when the backend does not serve ParseStream. On a header that
  // loaded the document, *out holds the live session, registered for
  // cancel(); otherwise the session is closed.
  std::optional<pdfv1::ParseHeader> start_session(
      const std::string* password, std::shared_ptr<ParseStreamSession>* out) const {
    pdfv1::ParseStreamOpen open;
    open.set_header_only(true);
    if (!sha256_.empty()) open.set_sha256(sha256_);
    open.set_declared_size(bytes_->size());
    if (password != nullptr) open.set_password(*password);
    std::chrono::system_clock::time_point deadline;
    {
      const std::lock_guard<std::mutex> lock(calls_mutex_);
      if (cancelled_) {
        throw PdfBackendUnavailable("PDF backend call cancelled", PdfBackendFailure::kCancelled);
      }
      deadline = deadline_;
    }
    // The session lives as long as the request may, so its own deadline is
    // the request's; each page call bounds its wait by its own budget.
    auto session = std::make_shared<ParseStreamSession>(stub_.get(), bytes_, open, deadline);
    {
      const std::lock_guard<std::mutex> lock(calls_mutex_);
      if (cancelled_) session->context()->TryCancel();
      calls_.insert(session->context());
    }
    // The open carries the upload, so it gets the Parse budget.
    StreamAnswer answer;
    const auto until = std::min(deadline, std::chrono::system_clock::now() + kParseDeadline);
    if (!session->await(0, until, &answer)) {
      retire(std::move(session));
      throw PdfBackendUnavailable("PDF backend open timed out", PdfBackendFailure::kDeadlineExceeded);
    }
    if (!answer.done.has_value()) {
      retire(std::move(session));
      if (answer.stream_status.error_code() == grpc::StatusCode::UNIMPLEMENTED) return std::nullopt;
      throw_backend_failure("PDF backend unreachable", answer.stream_status);
    }
    std::optional<pdfv1::ParseHeader> header;
    for (const auto& message : answer.messages) {
      if (message.has_parse() && message.parse().has_header()) {
        header = message.parse().header();
        break;
      }
    }
    if (!header.has_value()) {
      retire(std::move(session));
      check_outcome(*answer.done, "PDF backend open failed");
      throw InvalidDocument("PDF backend opened the stream without a header");
    }
    if (header->capabilities().load_status() == pdfv1::LOAD_STATUS_OK) {
      *out = std::move(session);
    } else {
      retire(std::move(session));
    }
    return header;
  }

  // Unregisters a session from cancel() and lets it close.
  void retire(std::shared_ptr<ParseStreamSession> session) const {
    if (session == nullptr) return;
    {
      const std::lock_guard<std::mutex> lock(calls_mutex_);
      calls_.erase(session->context());
    }
    session.reset();
  }

  void release_session() const {
    std::shared_ptr<ParseStreamSession> session;
    {
      const std::lock_guard<std::mutex> lock(session_mutex_);
      session = std::move(session_);
    }
    retire(std::move(session));
  }

  // Whether page calls go over the ParseStream session; true once open()
  // opened one.
  bool on_stream() const { return protocol_.load() == PdfBackendProtocol::kCurrent; }

  // The live session, opened afresh (one more upload) when the last one
  // ended, so one lost stream does not fail the rest of the document.
  std::shared_ptr<ParseStreamSession> current_session() const {
    const std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_ != nullptr && !session_->ended()) return session_;
    retire(std::move(session_));
    std::shared_ptr<ParseStreamSession> session;
    const std::optional<pdfv1::ParseHeader> header =
        start_session(password_.has_value() ? &*password_ : nullptr, &session);
    if (!header.has_value()) {
      throw PdfBackendUnavailable("PDF backend stopped serving ParseStream",
                                  PdfBackendFailure::kUnavailable);
    }
    if (session == nullptr) {
      throw InvalidDocument("PDF backend could not load the document: " +
                            pdfv1::LoadStatus_Name(header->capabilities().load_status()));
    }
    session_ = session;
    return session;
  }

  // A request that did not end OK fails the call the way the unary RPC's
  // status would have.
  static void check_outcome(const pdfv1::RequestDone& done, const std::string& what) {
    const std::string message =
        what + ": " + pdfv1::RequestOutcome_Name(done.outcome()) +
        (done.has_detail() && !done.detail().empty() ? " (" + done.detail() + ")" : "");
    switch (done.outcome()) {
      case pdfv1::REQUEST_OUTCOME_OK:
        return;
      case pdfv1::REQUEST_OUTCOME_CANCELLED:
        throw PdfBackendUnavailable(message, PdfBackendFailure::kCancelled);
      case pdfv1::REQUEST_OUTCOME_RESOURCE_EXHAUSTED:
        throw PdfBackendUnavailable(message, PdfBackendFailure::kResourceExhausted);
      case pdfv1::REQUEST_OUTCOME_DEADLINE_EXCEEDED:
        throw PdfBackendUnavailable(message, PdfBackendFailure::kDeadlineExceeded);
      default:
        throw InvalidDocument(message);
    }
  }

  // One page request over the session: sent with a fresh request_id and
  // waited for no longer than the call's budget or the request's deadline;
  // a request given up on is cancelled on the stream, which stays open.
  StreamAnswer stream_call(pdfv1::ParseStreamRequest request, std::chrono::seconds budget,
                           const std::string& what) const {
    std::chrono::system_clock::time_point until;
    {
      const std::lock_guard<std::mutex> lock(calls_mutex_);
      if (cancelled_) {
        throw PdfBackendUnavailable("PDF backend call cancelled", PdfBackendFailure::kCancelled);
      }
      until = std::min(deadline_, std::chrono::system_clock::now() + budget);
    }
    const std::shared_ptr<ParseStreamSession> session = current_session();
    const uint32_t id = session->next_id();
    if (request.has_parse()) {
      request.mutable_parse()->set_request_id(id);
    } else {
      request.mutable_render()->set_request_id(id);
    }
    session->send(id, request);
    StreamAnswer answer;
    if (!session->await(id, until, &answer)) {
      session->abandon(id);
      throw PdfBackendUnavailable(what + ": deadline exceeded",
                                  PdfBackendFailure::kDeadlineExceeded);
    }
    if (!answer.done.has_value()) throw_backend_failure(what, answer.stream_status);
    check_outcome(*answer.done, what);
    return answer;
  }

  static bool password_status(pdfv1::LoadStatus status) {
    return status == pdfv1::LOAD_STATUS_PASSWORD_REQUIRED ||
           status == pdfv1::LOAD_STATUS_PASSWORD_INCORRECT;
  }

  // One Probe, opening with `password` when one is given. A hash-only Probe
  // is a cache lookup; a miss earns exactly one retry with the bytes
  // attached so the backend can cache them under the hash.
  pdfv1::ProbeResponse probe_with(const std::string* password) const {
    pdfv1::ProbeResponse response;
    bool sent_bytes = !handshake_;
    for (;;) {
      Call call(*this, kProbeDeadline);
      pdfv1::ProbeRequest request;
      fill_document(request.mutable_document(), sent_bytes);
      if (password != nullptr) request.mutable_document()->set_password(*password);
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
    return response;
  }

  void check_page(int page_number) const {
    open();
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
    if (password_.has_value()) document->set_password(*password_);
  }

  int scaled(double user_space_units) const {
    return static_cast<int>(std::lround(user_space_units * render_scale_));
  }

  // One raster of the requested page in the fold's BGR at render_dpi; empty
  // for a zero-sized one, which the caller skips.
  cv::Mat page_raster(const pdfv1::PageRaster& raster, int page_number) const {
    if (raster.width_px() == 0 || raster.height_px() == 0) return {};
    if (raster.page_index() != static_cast<uint32_t>(page_number - 1)) {
      throw InvalidDocument("PDF backend answered with a raster for page " +
                            std::to_string(raster.page_index() + 1) + " instead of page " +
                            std::to_string(page_number));
    }
    return at_render_dpi(to_bgr(raster_view(raster), raster.pixel_format()), raster.dpi());
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
    // The wire fields are 32-bit, so these 64-bit products cannot wrap
    // today; the checked multiply keeps that true if a field ever widens.
    uint64_t row_bytes = 0;
    uint64_t total_bytes = 0;
    const bool wraps = __builtin_mul_overflow(width, static_cast<uint64_t>(channels), &row_bytes) ||
                       __builtin_mul_overflow(height, stride, &total_bytes);
    if (wraps || width > kMaxDim || height > kMaxDim || stride < row_bytes ||
        raster.pixels().size() < total_bytes) {
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
  const std::string target_;
  // kCurrent until the opening call finds a backend without ParseStream.
  mutable std::atomic<PdfBackendProtocol> protocol_;
  std::unique_ptr<pdfv1::PdfBackendService::Stub> stub_;
  // The ParseStream session page calls share; replaced when it ends.
  mutable std::mutex session_mutex_;
  mutable std::shared_ptr<ParseStreamSession> session_;
  // Set once by open(), under open_mutex_.
  mutable std::mutex open_mutex_;
  mutable std::exception_ptr open_failure_;
  mutable int pages_ = 0;
  mutable std::string backend_name_;
  // The request's candidates (set_passwords(), before the first use) and
  // the one that opened the document, chosen once by open() under
  // open_mutex_; every later call happens after open() returned.
  DocumentPasswords candidates_;
  mutable std::optional<std::string> password_;
  // Guards the request ties: set_deadline() and cancel() arrive from other
  // threads than the page calls.
  mutable std::mutex calls_mutex_;
  std::chrono::system_clock::time_point deadline_ = std::chrono::system_clock::time_point::max();
  bool cancelled_ = false;
  mutable std::set<grpc::ClientContext*> calls_;
};

}  // namespace

std::optional<PdfBackendProtocol> pdf_backend_protocol(const PageSource& source) {
  const auto* remote = dynamic_cast<const RemotePdfPageSource*>(&source);
  if (remote == nullptr) return std::nullopt;
  return remote->backend_protocol();
}

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

namespace {

std::mutex& pdf_backend_channel_mutex() {
  static std::mutex mutex;
  return mutex;
}

// Guarded by pdf_backend_channel_mutex().
CollectorChannelOptions& pdf_backend_channel_options() {
  static CollectorChannelOptions options;
  return options;
}

// One channel per target, made on first use and kept for the process.
// Guarded by pdf_backend_channel_mutex().
std::map<std::string, std::shared_ptr<grpc::Channel>>& pdf_backend_channels() {
  static std::map<std::string, std::shared_ptr<grpc::Channel>> channels;
  return channels;
}

}  // namespace

void set_pdf_backend_channel_options(const CollectorChannelOptions& options) {
  const std::lock_guard<std::mutex> lock(pdf_backend_channel_mutex());
  pdf_backend_channel_options() = options;
}

std::optional<grpc_connectivity_state> pdf_backend_channel_state(const std::string& target) {
  const std::lock_guard<std::mutex> lock(pdf_backend_channel_mutex());
  const auto& channels = pdf_backend_channels();
  const auto it = channels.find(target);
  if (it == channels.end()) return std::nullopt;
  // false: report the state, do not ask an idle channel to connect.
  return it->second->GetState(false);
}

std::shared_ptr<grpc::Channel> pdf_backend_channel(const std::string& target) {
  const std::lock_guard<std::mutex> lock(pdf_backend_channel_mutex());
  auto& channels = pdf_backend_channels();
  auto it = channels.find(target);
  if (it != channels.end()) return it->second;
  // The collector channels' arguments: their message limit (a page raster
  // at model DPI must fit in one message) and their keepalive, so a backend
  // that went silent mid-render fails the page instead of holding it to its
  // ten-minute deadline.
  auto channel = grpc::CreateCustomChannel(
      target, grpc::InsecureChannelCredentials(),
      collector_channel_arguments(pdf_backend_channel_options()));
  channels.emplace(target, channel);
  return channel;
}

std::shared_ptr<PageSource> open_remote_pdf_document(
    std::shared_ptr<const std::string> bytes, const std::string& target,
    double render_dpi, SourceOpening opening, std::string precomputed_sha256) {
  auto source = std::make_shared<RemotePdfPageSource>(
      std::move(bytes), target, render_dpi, std::move(precomputed_sha256));
  if (opening == SourceOpening::kNow) static_cast<void>(source->page_count());
  return source;
}

}  // namespace grparse
