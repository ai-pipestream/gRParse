// An in-process PdfBackendService for tests that send PDFs through the CV
// path. gRParse links no PDF engine: PDFs go to the backend that
// GRPARSE_PDF_BACKEND names, so a test registers the documents it will send
// (page geometry, rotation, text cells) and scopes the variable to this
// server. Rasters come back as blank white pages of the rendered size.
// Header only, and test-only: nothing under src/ or include/ may include it.
//
// ParseStream is off unless a test calls enable_parse_stream(); until then
// the fake answers it UNIMPLEMENTED, like a backend released before the RPC,
// so gRParse sources fall back to Probe/Parse/Render against it.
//
// The fake speaks the content-addressed handshake the way a real backend
// does: a hash-only request for bytes it has not seen is a cache miss
// (LOAD_STATUS_BYTES_REQUIRED), and bytes it does not know are CORRUPT.
//
// Tests describe geometry in unshifted PDF user space. By default the fake
// answers in that frame without stating one (a backend released before
// PageInfo.page_space existed); set_page_space() makes it state a frame,
// and PAGE_SPACE_CROP_BOX shifts every box by the CropBox origin the way a
// backend on the contract frame reports it.
#ifndef GRPARSE_TESTS_SUPPORT_FAKE_PDF_BACKEND_H
#define GRPARSE_TESTS_SUPPORT_FAKE_PDF_BACKEND_H

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

// What the engine says, in the trailer, about content it skipped or repaired
// on a page: a ParseWarning naming that page.
struct FakeParseWarning {
  std::string message;
  // PDF_FAMILY_UNSPECIFIED leaves ParseWarning.family unset, the way
  // grpc-qparse reports a page it could not decode at all.
  pdfv1::PdfFamily family = pdfv1::PDF_FAMILY_UNSPECIFIED;
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
  // AcroForm widgets, rects in user space; answered for
  // PDF_FAMILY_FORM_FIELDS.
  std::vector<pdfv1::FormField> form_fields = {};
  // Trailer warnings about this page, sent on every Parse whose range
  // covers it.
  std::vector<FakeParseWarning> warnings = {};
  // The engine failed on the page: no PageChunk is sent for it, only the
  // warnings above (grpc-qparse's "page skipped").
  bool no_chunk = false;
  // Text cells and form fields the trailer counts but the stream never
  // carried: items lost between the engine and the client.
  uint64_t lost_cells = 0;
  uint64_t lost_form_fields = 0;
  // What the engine says on the page's raster about content it skipped or
  // repaired while drawing: RenderResponse.warnings on every Render of it.
  std::vector<FakeParseWarning> render_warnings = {};
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

  int probe_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return probe_calls_;
  }

  // Probes that carried the document's bytes (the handshake's one upload),
  // and the sha256 each of them carried beside the bytes, in arrival order.
  int probes_with_bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(probe_upload_hashes_.size());
  }
  std::vector<std::string> probe_upload_hashes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return probe_upload_hashes_;
  }

  // Holds every Render this long before answering, unless the client
  // cancels the call first: a backend stuck on a page.
  void stall_renders(std::chrono::milliseconds stall) {
    const std::lock_guard<std::mutex> lock(mutex_);
    render_stall_ = stall;
  }

  // Fails every Render that covers this page (0-based) with INTERNAL: a
  // backend that opened the document and cannot draw one of its pages.
  void fail_renders_of_page(uint32_t page_index) {
    const std::lock_guard<std::mutex> lock(mutex_);
    failing_renders_.insert(page_index);
  }

  // Encrypts a registered document: a load that carries no password answers
  // PASSWORD_REQUIRED and one that carries another answers
  // PASSWORD_INCORRECT, on every RPC, as a real backend does.
  void require_password(const std::string& bytes, std::string password) {
    const std::lock_guard<std::mutex> lock(mutex_);
    passwords_[bytes] = std::move(password);
  }

  // The frame Parse reports geometry in; PAGE_SPACE_UNSPECIFIED (the
  // default) leaves PageInfo.page_space unset.
  void set_page_space(pdfv1::PageSpace space) {
    const std::lock_guard<std::mutex> lock(mutex_);
    page_space_ = space;
  }

  // A Probe that carries these bytes holds until release_probes() or until
  // the caller cancels it (or its deadline passes).
  void block_probe(const std::string& bytes) {
    const std::lock_guard<std::mutex> lock(mutex_);
    blocked_.insert(bytes);
  }

  void release_probes() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      blocked_.clear();
    }
    probe_changed_.notify_all();
  }

  // Probes currently held by block_probe().
  int held_probes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return held_probes_;
  }

  grpc::Status Probe(grpc::ServerContext* context, const pdfv1::ProbeRequest* request,
                     pdfv1::ProbeResponse* response) override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ++probe_calls_;
      if (!request->document().data().empty()) {
        probe_upload_hashes_.push_back(request->document().sha256());
      }
      if (blocked_.contains(request->document().data())) {
        ++held_probes_;
        while (blocked_.contains(request->document().data()) && !context->IsCancelled()) {
          probe_changed_.wait_for(lock, std::chrono::milliseconds(10));
        }
        --held_probes_;
        if (context->IsCancelled()) return grpc::Status(grpc::StatusCode::CANCELLED, "probe cancelled");
      }
    }
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
    return parse_to(*request, [writer](const pdfv1::ParseResponse& m) { return writer->Write(m); });
  }

  grpc::Status Render(grpc::ServerContext* context, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    return render_to(context, *request,
                     [writer](const pdfv1::RenderResponse& m) { return writer->Write(m); });
  }

  // ParseStream the way a real backend serves it, in miniature: the open,
  // the chunked upload, then the open's parse (request 0) and every
  // ParsePages and RenderPages answered like Parse and Render on the
  // uploaded bytes, each closed by a RequestDone, and a ParseStreamDone at
  // the end. Requests are served one at a time on the handler thread, in
  // the order they arrive; those sent mid-upload wait for the last chunk.
  // Off by default (enable_parse_stream()): the fake then answers
  // UNIMPLEMENTED like a backend that predates the RPC, which is what the
  // tests built on Probe counts and blocked probes expect.
  grpc::Status ParseStream(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<pdfv1::ParseStreamResponse, pdfv1::ParseStreamRequest>* stream)
      override {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!serve_stream_) {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "fake backend predates ParseStream");
      }
      ++stream_opens_;
    }
    pdfv1::ParseStreamRequest message;
    if (!stream->Read(&message) || !message.has_open()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "the first message must be the open");
    }
    const pdfv1::ParseStreamOpen open = message.open();
    std::string bytes;
    bool complete = false;
    bool loaded = false;
    std::vector<pdfv1::ParseStreamRequest> waiting;
    std::set<uint32_t> cancelled;
    const auto document = [&] {
      pdfv1::PdfDocument out;
      out.set_data(bytes);
      if (open.has_sha256()) out.set_sha256(open.sha256());
      if (open.has_password()) out.set_password(open.password());
      return out;
    };
    const auto request_done = [&](uint32_t id, pdfv1::RequestOutcome outcome) {
      pdfv1::ParseStreamResponse out;
      out.set_request_id(id);
      out.mutable_request_done()->set_outcome(outcome);
      stream->Write(out);
    };
    const auto serve = [&](const pdfv1::ParseStreamRequest& request) {
      const uint32_t id =
          request.has_parse() ? request.parse().request_id() : request.render().request_id();
      if (cancelled.contains(id)) {
        request_done(id, pdfv1::REQUEST_OUTCOME_CANCELLED);
        return;
      }
      if (!loaded) {
        request_done(id, pdfv1::REQUEST_OUTCOME_NOT_LOADED);
        return;
      }
      grpc::Status status;
      if (request.has_parse()) {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          ++stream_parse_requests_;
        }
        pdfv1::ParseRequest parse;
        *parse.mutable_document() = document();
        *parse.mutable_families() = request.parse().families();
        if (request.parse().has_pages()) *parse.mutable_pages() = request.parse().pages();
        status = parse_to(parse, [&](const pdfv1::ParseResponse& m) {
          pdfv1::ParseStreamResponse out;
          out.set_request_id(id);
          *out.mutable_parse() = m;
          return stream->Write(out);
        });
      } else {
        pdfv1::RenderRequest render;
        *render.mutable_document() = document();
        if (request.render().has_pages()) *render.mutable_pages() = request.render().pages();
        render.set_dpi(request.render().dpi());
        render.set_pixel_format(request.render().pixel_format());
        status = render_to(context, render, [&](const pdfv1::RenderResponse& m) {
          pdfv1::ParseStreamResponse out;
          out.set_request_id(id);
          *out.mutable_render() = m;
          return stream->Write(out);
        });
      }
      request_done(id, status.ok() ? pdfv1::REQUEST_OUTCOME_OK
                                   : status.error_code() == grpc::StatusCode::CANCELLED
                                         ? pdfv1::REQUEST_OUTCOME_CANCELLED
                                         : pdfv1::REQUEST_OUTCOME_ENGINE_ERROR);
    };
    while (stream->Read(&message)) {
      if (message.has_chunk()) {
        if (complete || message.chunk().offset() != bytes.size()) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "chunk out of order");
        }
        bytes += message.chunk().data();
        if (!message.chunk().last()) continue;
        complete = true;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          stream_bytes_ += bytes.size();
        }
        // The open's own parse: the header alone when header_only.
        pdfv1::ParseRequest parse;
        *parse.mutable_document() = document();
        if (open.header_only()) {
          parse.add_families(pdfv1::PDF_FAMILY_PAGE_INVENTORY);
          parse.mutable_pages()->set_begin(0xFFFFFFFEu);
          parse.mutable_pages()->set_end(0xFFFFFFFFu);
        } else {
          *parse.mutable_families() = open.families();
          if (open.has_pages()) *parse.mutable_pages() = open.pages();
        }
        parse_to(parse, [&](const pdfv1::ParseResponse& m) {
          if (m.has_header()) {
            loaded = m.header().capabilities().load_status() == pdfv1::LOAD_STATUS_OK;
          } else if (open.header_only()) {
            return true;
          }
          pdfv1::ParseStreamResponse out;
          out.set_request_id(0);
          *out.mutable_parse() = m;
          return stream->Write(out);
        });
        request_done(0, pdfv1::REQUEST_OUTCOME_OK);
        for (const auto& request : waiting) serve(request);
        waiting.clear();
      } else if (message.has_parse() || message.has_render()) {
        if (complete) {
          serve(message);
        } else {
          waiting.push_back(message);
        }
      } else if (message.has_cancel()) {
        if (!message.cancel().has_request_id()) break;
        cancelled.insert(message.cancel().request_id());
      }
    }
    if (context->IsCancelled()) return grpc::Status::CANCELLED;
    pdfv1::ParseStreamResponse done;
    done.mutable_done()->set_bytes_received(bytes.size());
    stream->Write(done);
    return grpc::Status::OK;
  }

  // Serves ParseStream instead of answering UNIMPLEMENTED.
  void enable_parse_stream() {
    const std::lock_guard<std::mutex> lock(mutex_);
    serve_stream_ = true;
  }

  // ParseStream calls, the document bytes their uploads carried in all, and
  // the ParsePages requests they served.
  int stream_opens() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_opens_;
  }
  size_t stream_bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_bytes_;
  }
  int stream_parse_requests() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_parse_requests_;
  }

 private:
  grpc::Status parse_to(const pdfv1::ParseRequest& request_ref,
                        const std::function<bool(const pdfv1::ParseResponse&)>& emit) {
    const pdfv1::ParseRequest* request = &request_ref;
    const auto [status, pages] = load(request->document());
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_backend_name("fake-pdf-backend");
    caps->set_load_status(status);
    if (status != pdfv1::LOAD_STATUS_OK) {
      emit(header);
      return grpc::Status::OK;
    }
    caps->set_page_count(static_cast<uint32_t>(pages.size()));
    bool any_fields = false;
    for (const FakePdfPage& page : pages) any_fields = any_fields || !page.form_fields.empty();
    auto* verdict = caps->add_families();
    verdict->set_family(pdfv1::PDF_FAMILY_FORM_FIELDS);
    verdict->set_support(any_fields ? pdfv1::FAMILY_SUPPORT_SUPPORTED
                                    : pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT);
    // An empty family list asks for every family; form fields stay opt-in
    // here, as they were before the fake read the list for anything else.
    bool wants_fields = false;
    bool wants_cells = request->families().empty();
    for (const int family : request->families()) {
      wants_fields = wants_fields || family == pdfv1::PDF_FAMILY_FORM_FIELDS;
      wants_cells = wants_cells || family == pdfv1::PDF_FAMILY_TEXT_CELLS;
    }
    const pdfv1::PageSpace space = page_space();
    // Per page, what the frame subtracts from user space.
    std::vector<std::pair<double, double>> shifts;
    for (size_t index = 0; index < pages.size(); ++index) {
      const FakePdfPage& page = pages[index];
      shifts.emplace_back(0.0, 0.0);
      if (space == pdfv1::PAGE_SPACE_CROP_BOX && page.crop_box.has_value()) {
        shifts.back() = {(*page.crop_box)[0], (*page.crop_box)[1]};
      }
      auto* info = header.mutable_header()->add_pages();
      if (space != pdfv1::PAGE_SPACE_UNSPECIFIED) info->set_page_space(space);
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
    emit(header);

    const uint32_t begin = request->has_pages() ? request->pages().begin() : 0;
    const uint32_t end = request->has_pages() ? request->pages().end()
                                              : static_cast<uint32_t>(pages.size());
    std::map<std::string, uint32_t> fonts;
    // The trailer a real backend closes the answer with: how many items of
    // each family the stream carried, and what the engine skipped.
    pdfv1::ParseResponse trailer;
    uint64_t cells_counted = 0;
    uint64_t fields_counted = 0;
    for (uint32_t index = begin; index < end && index < pages.size(); ++index) {
      const FakePdfPage& page = pages[index];
      for (const FakeParseWarning& said : page.warnings) {
        auto* warning = trailer.mutable_trailer()->add_warnings();
        warning->set_page_index(index);
        if (said.family != pdfv1::PDF_FAMILY_UNSPECIFIED) warning->set_family(said.family);
        warning->set_message(said.message);
      }
      if (page.no_chunk) continue;
      const auto [dx, dy] = shifts[index];
      pdfv1::ParseResponse chunk;
      chunk.mutable_page()->set_page_index(index);
      if (wants_fields) {
        for (const pdfv1::FormField& source : page.form_fields) {
          auto* field = chunk.mutable_page()->add_form_fields();
          *field = source;
          shift(field->mutable_rect(), dx, dy);
        }
        fields_counted += page.form_fields.size() + page.lost_form_fields;
      }
      if (wants_cells) {
        for (const FakeTextCell& source : page.cells) {
          auto [font, added] = fonts.emplace(source.font, static_cast<uint32_t>(fonts.size()));
          if (added) {
            pdfv1::ParseResponse table;
            auto* ref = table.mutable_fonts()->add_fonts();
            ref->set_font_id(font->second);
            ref->set_base_name(source.font);
            emit(table);
          }
          auto* cell = chunk.mutable_page()->add_text_cells();
          cell->set_text(source.text);
          cell->mutable_bbox()->set_x0(source.x0);
          cell->mutable_bbox()->set_y0(source.y0);
          cell->mutable_bbox()->set_x1(source.x1);
          cell->mutable_bbox()->set_y1(source.y1);
          shift(cell->mutable_bbox(), dx, dy);
          cell->set_font_id(font->second);
          cell->set_font_size(source.font_size);
        }
        cells_counted += page.cells.size() + page.lost_cells;
      }
      emit(chunk);
    }
    const auto count = [&trailer](pdfv1::PdfFamily family, uint64_t items) {
      auto* entry = trailer.mutable_trailer()->add_counts();
      entry->set_family(family);
      entry->set_count(items);
    };
    count(pdfv1::PDF_FAMILY_PAGE_INVENTORY, pages.size());
    count(pdfv1::PDF_FAMILY_TEXT_CELLS, cells_counted);
    count(pdfv1::PDF_FAMILY_FONTS, fonts.size());
    count(pdfv1::PDF_FAMILY_FORM_FIELDS, fields_counted);
    emit(trailer);
    return grpc::Status::OK;
  }

  grpc::Status render_to(grpc::ServerContext* context, const pdfv1::RenderRequest& request_ref,
                         const std::function<bool(const pdfv1::RenderResponse&)>& emit) {
    const pdfv1::RenderRequest* request = &request_ref;
    const auto [status, pages] = load(request->document());
    std::chrono::milliseconds stall{0};
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++render_calls_;
      stall = render_stall_;
    }
    const auto release = std::chrono::steady_clock::now() + stall;
    while (std::chrono::steady_clock::now() < release && !context->IsCancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (context->IsCancelled()) return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled");
    if (status != pdfv1::LOAD_STATUS_OK) {
      pdfv1::RenderResponse head;
      head.mutable_head()->set_load_status(status);
      emit(head);
      return grpc::Status::OK;
    }
    const uint32_t begin = request->has_pages() ? request->pages().begin() : 0;
    const uint32_t end = request->has_pages() ? request->pages().end()
                                              : static_cast<uint32_t>(pages.size());
    for (uint32_t index = begin; index < end && index < pages.size(); ++index) {
      if (render_fails(index)) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "fake backend cannot draw page " + std::to_string(index + 1));
      }
    }
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
      for (const FakeParseWarning& said : page.render_warnings) {
        auto* warning = message.add_warnings();
        warning->set_page_index(index);
        if (said.family != pdfv1::PDF_FAMILY_UNSPECIFIED) warning->set_family(said.family);
        warning->set_message(said.message);
      }
      emit(message);
    }
    return grpc::Status::OK;
  }

  pdfv1::PageSpace page_space() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return page_space_;
  }

  bool render_fails(uint32_t page_index) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return failing_renders_.contains(page_index);
  }

  static void shift(pdfv1::BoundingBox* box, double dx, double dy) {
    box->set_x0(box->x0() - dx);
    box->set_y0(box->y0() - dy);
    box->set_x1(box->x1() - dx);
    box->set_y1(box->y1() - dy);
  }

  // The load verdict and, on OK, the registered pages: bytes on the wire are
  // looked up (and cached under the hash the client sent beside them), a
  // bare hash must already be cached.
  std::pair<pdfv1::LoadStatus, std::vector<FakePdfPage>> load(const pdfv1::PdfDocument& document) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (document.data().empty()) {
      const auto cached = by_hash_.find(document.sha256());
      if (cached == by_hash_.end()) return {pdfv1::LOAD_STATUS_BYTES_REQUIRED, {}};
      const auto locked = passwords_by_hash_.find(document.sha256());
      if (locked != passwords_by_hash_.end()) {
        const pdfv1::LoadStatus verdict = password_verdict(document, locked->second);
        if (verdict != pdfv1::LOAD_STATUS_OK) return {verdict, {}};
      }
      return {pdfv1::LOAD_STATUS_OK, cached->second};
    }
    const auto known = known_.find(document.data());
    if (known == known_.end()) return {pdfv1::LOAD_STATUS_CORRUPT, {}};
    const auto locked = passwords_.find(document.data());
    if (document.has_sha256()) {
      by_hash_[document.sha256()] = known->second;
      if (locked != passwords_.end()) passwords_by_hash_[document.sha256()] = locked->second;
    }
    if (locked != passwords_.end()) {
      const pdfv1::LoadStatus verdict = password_verdict(document, locked->second);
      if (verdict != pdfv1::LOAD_STATUS_OK) return {verdict, {}};
    }
    return {pdfv1::LOAD_STATUS_OK, known->second};
  }

  static pdfv1::LoadStatus password_verdict(const pdfv1::PdfDocument& document,
                                            const std::string& password) {
    if (!document.has_password()) return pdfv1::LOAD_STATUS_PASSWORD_REQUIRED;
    return document.password() == password ? pdfv1::LOAD_STATUS_OK
                                           : pdfv1::LOAD_STATUS_PASSWORD_INCORRECT;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::vector<FakePdfPage>> known_;
  std::map<std::string, std::vector<FakePdfPage>> by_hash_;
  std::set<uint32_t> failing_renders_;
  std::map<std::string, std::string> passwords_;
  std::map<std::string, std::string> passwords_by_hash_;
  int render_calls_ = 0;
  int probe_calls_ = 0;
  std::vector<std::string> probe_upload_hashes_;
  std::chrono::milliseconds render_stall_{0};
  pdfv1::PageSpace page_space_ = pdfv1::PAGE_SPACE_UNSPECIFIED;
  std::set<std::string> blocked_;
  int held_probes_ = 0;
  std::condition_variable probe_changed_;
  bool serve_stream_ = false;
  int stream_opens_ = 0;
  size_t stream_bytes_ = 0;
  int stream_parse_requests_ = 0;
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
