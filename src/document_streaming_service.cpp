#include "grparse/document_parser_service.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/arena.h>

#include "grparse/collector_coordinator.h"
#include "grparse/content_sniff.h"
#include "grparse/data_totals.h"
#include "grparse/document_assembly.h"
#include "grparse/document_collectors.h"
#include "grparse/document_repair.h"
#include "grparse/page_previews.h"
#include "grparse/page_projection.h"
#include "parse_support.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// The largest document the streaming surface accepts, summed over the
// chunks of one stream.
constexpr size_t kMaximumDocumentBytes = 500U * 1024U * 1024U;

class ArenaEvent final {
 public:
  ArenaEvent() : message(google::protobuf::Arena::Create<pipestream::parse::v1::DocumentStreamEvent>(&arena)) {}

  google::protobuf::Arena arena;
  pipestream::parse::v1::DocumentStreamEvent* message;
};

class DocumentStreamReactor final
    : public grpc::ServerBidiReactor<pipestream::parse::v1::DocumentChunk,
                                     pipestream::parse::v1::DocumentStreamEvent> {
 public:
  DocumentStreamReactor(grpc::CallbackServerContext* context, PageScheduler& scheduler,
                        std::shared_ptr<CollectorEndpoints> endpoints,
                        std::optional<RepairOptions> repair)
      : context_(context),
        scheduler_(scheduler),
        endpoints_(std::move(endpoints)),
        repair_(std::move(repair)),
        // The scheduler never delivers more than one page window ahead of
        // the credits this reactor returns, so the configured window is the
        // exact buffer bound; any smaller cap would fail well-behaved
        // clients with RESOURCE_EXHAUSTED under a raised GRPARSE_PAGE_WINDOW.
        maximum_buffered_pages_(scheduler.page_window()),
        callback_gate_(std::make_shared<CallbackGate>()) {
    callback_gate_->reactor = this;
    StartRead(&incoming_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      if (ready_to_parse()) begin_processing();
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    accept_chunk_locked();
  }

  void OnWriteDone(bool ok) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INTERNAL, "write completed without an event"));
      return;
    }
    const bool page_written = events_.front()->message->has_page();
    if (page_written && buffered_pages_ > 0) --buffered_pages_;
    events_.pop_front();
    write_in_flight_ = false;
    if (!ok) {
      client_cancelled_ = true;
      events_.clear();
      buffered_pages_ = 0;
      request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "client stopped reading"));
      return;
    }
    if (page_written) ticket_.release();
    pump_locked();
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    client_cancelled_ = true;
    request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
  }

  void OnDone() override {
    // gRPC's callback API transfers ownership of the reactor; OnDone must delete
    // it.  Scheduler threads are not gRPC reactions and hold no call reference,
    // so the gate mutex, not just the null check, is what keeps this delete
    // ordered after any in-progress on_page/on_scheduler_finish call.
    const auto gate = callback_gate_;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->reactor = nullptr;
    }
    delete this;
  }

 private:
  struct CallbackGate {
    std::mutex mutex;
    DocumentStreamReactor* reactor = nullptr;
  };

  // The client half-closed. True when the stream ended with a document to
  // parse; a cancelled or truncated stream finishes the call instead.
  bool ready_to_parse() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) {
      request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
      return false;
    }
    if (!complete_seen_ || bytes_.empty()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "stream must end with a non-empty complete chunk"));
      return false;
    }
    return true;
  }

  // One chunk of the upload: identity and options resolve from the first
  // chunk that carries them, the payload accumulates behind the size limit,
  // and the next read starts. Anything a chunk contradicts finishes the call.
  void accept_chunk_locked() {
    if (finish_requested_) return;
    if (complete_seen_) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "received data after complete chunk"));
      return;
    }
    if (!resolve_identity_locked()) return;
    resolve_options_locked();
    if (incoming_.data().size() > kMaximumDocumentBytes - bytes_.size()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                         "document exceeds 500 MiB streaming limit"));
      return;
    }
    bytes_.append(incoming_.data());
    complete_seen_ = incoming_.complete();
    incoming_.Clear();
    StartRead(&incoming_);
  }

  // The document's identity: the first chunk names it and every later chunk
  // must agree. False when the call was finished over a disagreement.
  bool resolve_identity_locked() {
    if (document_id_.empty()) {
      document_id_ = incoming_.document_id();
      filename_ = fs::path(incoming_.filename()).filename();
      content_type_ = incoming_.content_type();
      if (document_id_.empty() || filename_.empty()) {
        request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "first chunk requires document_id and filename"));
        return false;
      }
      return true;
    }
    if ((!incoming_.document_id().empty() && incoming_.document_id() != document_id_) ||
        (!incoming_.filename().empty() &&
         fs::path(incoming_.filename()).filename() != filename_)) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "all chunks must describe the same document"));
      return false;
    }
    return true;
  }

  // The collector selection resolves like the identity fields: the first
  // chunk whose list is non-empty wins. Recognition tuning resolves under
  // the same doctrine: the first chunk that sets a field wins. Validation
  // waits for the complete stream so a contradiction split across chunks is
  // still caught.
  void resolve_options_locked() {
    if (requested_collectors_.empty() && !incoming_.collectors().empty()) {
      for (const int value : incoming_.collectors()) {
        requested_collectors_.push_back(static_cast<pipestream::parse::v1::Collector>(value));
      }
    }
    if (!do_ocr_.has_value() && incoming_.has_do_ocr()) do_ocr_ = incoming_.do_ocr();
    if (!force_ocr_.has_value() && incoming_.has_force_ocr()) {
      force_ocr_ = incoming_.force_ocr();
    }
    if (!render_scale_.has_value() && incoming_.has_render_scale()) {
      render_scale_ = incoming_.render_scale();
    }
  }

  // The plan one stream resolves to: the legs that will run and what the CV
  // leg runs with. `started` is false when the stream was finished instead,
  // either by an invalid tuning or by a plan with no runnable leg in it.
  struct StreamPlan {
    std::shared_ptr<const std::string> bytes;
    PageScheduler::OcrTuning tuning;
    std::vector<pipestream::parse::v1::Collector> remotes;
    std::vector<pipestream::parse::v1::Collector> locals;
    bool pdf = false;
    bool want_cv = false;
    bool pdf_routing = false;
    bool started = false;
  };

  void begin_processing() {
    const StreamPlan plan = resolve_plan();
    if (!plan.started) return;
    stamp_origin(*plan.bytes);
    for (const auto id : plan.locals) {
      spawn_local_collector(id, plan.bytes);
    }
    for (const auto id : plan.remotes) {
      if (id == pipestream::parse::v1::COLLECTOR_PDF && plan.pdf_routing) {
        spawn_pdf_router(plan.bytes, plan.pdf, plan.tuning);
      } else {
        spawn_remote_collector(id, plan.bytes);
      }
    }
    if (!plan.want_cv) return;
    submit_cv(plan.bytes, plan.pdf, plan.tuning);
  }

  // Scatter-gather routing: the plan resolves from the request's collector
  // selection or the document's format, unwired collectors fail immediately,
  // and each wired collector is one pending part of the stream. The parse
  // degrades collector by collector instead of failing while any part
  // succeeds. The default PDF route becomes the pdf inspector when one is
  // configured, exactly like the unary path.
  StreamPlan resolve_plan() {
    StreamPlan plan;
    std::lock_guard<std::mutex> lock(mutex_);
    // The resolved recognition fields validate exactly like the unary
    // options; an invalid value fails the stream naming the offender.
    const grpc::Status tuning_status = validate_ocr_tuning(
        do_ocr_.has_value(), do_ocr_.value_or(true), force_ocr_.value_or(false),
        render_scale_.has_value(), render_scale_.value_or(0.0));
    if (!tuning_status.ok()) {
      request_finish_locked(tuning_status);
      return plan;
    }
    plan.tuning = ocr_tuning(do_ocr_.has_value(), do_ocr_.value_or(true),
                             force_ocr_.value_or(false), render_scale_.has_value(),
                             render_scale_.value_or(0.0));
    plan.bytes = std::make_shared<const std::string>(std::move(bytes_));
    plan.pdf = content_type_ == "application/pdf" || is_pdf(*plan.bytes, filename_);
    pdf_ = plan.pdf;
    auto routed = route_document(filename_.string(), content_type_, *plan.bytes);
    if (requested_collectors_.empty() && plan.pdf &&
        routed == pipestream::parse::v1::COLLECTOR_GRPARSE_CV && endpoints_ != nullptr &&
        endpoints_->has(pipestream::parse::v1::COLLECTOR_PDF)) {
      routed = pipestream::parse::v1::COLLECTOR_PDF;
    }
    const auto plan_ids = resolve_collectors(requested_collectors_, routed);
    // The routing leg applies when the pdf collector is the whole plan;
    // shared with other collectors it runs as a plain Document leg.
    plan.pdf_routing = plan.pdf && plan_ids.size() == 1 &&
                       plan_ids[0] == pipestream::parse::v1::COLLECTOR_PDF &&
                       endpoints_ != nullptr &&
                       endpoints_->has(pipestream::parse::v1::COLLECTOR_PDF);
    count_parts_locked(plan_ids, &plan);
    if (pending_parts_ == 0) {
      request_finish_locked(first_failure_status_);
      return plan;
    }
    plan.started = true;
    return plan;
  }

  // Sorts the planned collectors into the legs that will run, counting one
  // pending part each; a collector this build cannot run fails on the spot.
  void count_parts_locked(const std::vector<pipestream::parse::v1::Collector>& plan_ids,
                          StreamPlan* plan) {
    for (const auto id : plan_ids) {
      if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
        plan->want_cv = true;
        ++pending_parts_;
      } else if (local_collector(id)) {
        plan->locals.push_back(id);
        ++pending_parts_;
      } else if (remote_collector(id)) {
        plan->remotes.push_back(id);
        ++pending_parts_;
      } else {
        record_part_failure_locked(
            id, grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                             std::string("collector '") + collector_name(id) +
                                 "' is not wired in yet"));
      }
    }
  }

  // Hash and sniff the request once, here, rather than under the reactor
  // lock at completion: the hash is a linear pass over up to 500 MiB, and
  // the bytes are gone by the time the completion event is built.
  void stamp_origin(const std::string& bytes) {
    const uint64_t hash = content_hash(bytes);
    const MimetypeResolution resolved = resolve_mimetype(content_type_, bytes, filename_);
    if (resolved.evidence == "magic") {
      data_counters().mimetypes_sniffed.fetch_add(1, std::memory_order_relaxed);
      data_log("origin " + filename_.string() + " mimetype " + resolved.mimetype +
               " from the bytes");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    document_bytes_hash_ = hash;
    origin_mimetype_ = resolved;
  }

  // Submits the document to the in-process CV pipeline and wires its
  // callbacks through the gate. Shared by the plain CV plan leg and by the
  // pdf routing leg once the inspector's classification has named the pages
  // needing OCR; a submission failure degrades like any CV failure.
  void submit_cv(std::shared_ptr<const std::string> bytes, bool pdf,
                 PageScheduler::OcrTuning tuning) {
    try {
      const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
      auto ticket = scheduler_.submit(
          std::move(bytes), pdf, tuning,
          PageScheduler::Callbacks{
              [weak_gate](int total_pages) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) gate->reactor->on_document(total_pages);
                }
              },
              [weak_gate](int page_number, std::shared_ptr<const OcrPage> page) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) {
                    return gate->reactor->on_page(page_number, std::move(page));
                  }
                }
                return PageScheduler::DeliveryResult::kCancelled;
              },
              [weak_gate](std::exception_ptr failure) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) gate->reactor->on_scheduler_finish(std::move(failure));
                }
              }});
      std::lock_guard<std::mutex> lock(mutex_);
      ticket_ = std::move(ticket);
      if (finish_requested_) cancel_ticket_locked();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      record_part_failure_locked(pipestream::parse::v1::COLLECTOR_GRPARSE_CV,
                                 status_from_exception(std::current_exception()));
      part_done_locked();
      pump_locked();
    }
  }

  // The pdf routing leg: the inspector's classification decides whether the
  // parse is the collector's own fast-path Document (text-based) or the
  // in-process CV pipeline restricted to the pages the inspector named as
  // needing OCR. A failed classification degrades to the unrouted CV path,
  // never to a failed parse. Runs on its own thread for the same reason
  // spawn_remote_collector does: the collector call is a blocking client.
  void spawn_pdf_router(std::shared_ptr<const std::string> bytes, bool pdf,
                        PageScheduler::OcrTuning tuning) {
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    auto endpoints = endpoints_;
    const CollectorDeadline inbound_deadline = context_->deadline();
    const bool previews = scheduler_.captures_page_images();
    std::thread([weak_gate, endpoints, bytes = std::move(bytes), pdf, inbound_deadline,
                 tuning = std::move(tuning), previews]() mutable {
      PdfParseResult parsed = collect_pdf(
          endpoints == nullptr
              ? nullptr
              : endpoints->channel(pipestream::parse::v1::COLLECTOR_PDF),
          *bytes, inbound_deadline);
      const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
      if (parsed.outcome.success && route.fast_path) {
        // Rendered before the reactor sees the document, on this thread,
        // where the blocking work already is.
        if (previews) attach_page_previews(bytes, &parsed.outcome.document);
        if (const auto gate = weak_gate.lock()) {
          std::lock_guard<std::mutex> lock(gate->mutex);
          if (gate->reactor != nullptr) {
            gate->reactor->on_collector_done(pipestream::parse::v1::COLLECTOR_PDF,
                                             std::move(parsed.outcome));
          }
        }
        return;
      }
      if (parsed.outcome.success) {
        tuning.ocr_pages.insert(route.ocr_pages.begin(), route.ocr_pages.end());
        if (route.force_ocr && tuning.mode == PageScheduler::OcrTuning::Mode::kSelective) {
          tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
        }
      } else if (const auto gate = weak_gate.lock()) {
        // The degradation stays visible: the pdf collector's failure rides
        // the complete event as a failure entry while the in-process CV path
        // parses the document, same as any collector failing beside a
        // surviving one. The pending part itself is settled by the CV run.
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) gate->reactor->note_pdf_fallback(parsed.outcome);
      }
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->submit_cv(std::move(bytes), pdf, std::move(tuning));
        }
      }
    }).detach();
  }

  void note_pdf_fallback(const CollectorOutcome& outcome) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_part_failure_locked(
        pipestream::parse::v1::COLLECTOR_PDF,
        grpc::Status(outcome.code, outcome.error + "; fell back to the in-process CV path"));
  }

  // An in-process collector runs on its own thread for the same reason a
  // remote one does: the fold is straight-line work on the request bytes,
  // not a gRPC reaction, and the gate keeps its completion safe against
  // reactor teardown.
  void spawn_local_collector(pipestream::parse::v1::Collector id,
                             std::shared_ptr<const std::string> bytes) {
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    std::thread([weak_gate, id, bytes]() {
      CollectorOutcome outcome = run_local_collector(id, *bytes);
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->on_collector_done(id, std::move(outcome));
        }
      }
    }).detach();
  }

  // A remote collector runs on its own thread: it is a blocking client
  // stream, not a gRPC reaction. The gate keeps its completion safe against
  // reactor teardown exactly like the scheduler callbacks. A client cancel
  // abandons the result; the leg's deadline, the sooner of this call's own
  // and the collector's cap, bounds the orphaned call, so a client that
  // walked away is not waited on past the deadline it set. The streaming
  // wire carries no ebcdic layout and no lol-html rules, so selecting either
  // collector here degrades to that collector's own INVALID_ARGUMENT.
  void spawn_remote_collector(pipestream::parse::v1::Collector id,
                              std::shared_ptr<const std::string> bytes) {
    std::string document_id;
    std::string filename;
    std::string content_type;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      document_id = document_id_;
      filename = filename_.string();
      content_type = content_type_;
    }
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    auto endpoints = endpoints_;
    const CollectorDeadline inbound_deadline = context_->deadline();
    std::thread([weak_gate, endpoints, id, bytes, document_id, filename, content_type,
                 inbound_deadline]() {
      CollectorOutcome outcome = run_remote_collector(
          id, endpoints, document_id, filename, content_type, *bytes,
          std::string(), std::string(), inbound_deadline);
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->on_collector_done(id, std::move(outcome));
        }
      }
    }).detach();
  }

  void on_document(int total_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_pages_ = total_pages;
  }

  PageScheduler::DeliveryResult on_page(int page_number, std::shared_ptr<const OcrPage> page) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return PageScheduler::DeliveryResult::kCancelled;
    if (buffered_pages_ >= maximum_buffered_pages_) {
      completed_pages_.clear();
      if (write_in_flight_ && !events_.empty()) {
        events_.erase(std::next(events_.begin()), events_.end());
        buffered_pages_ = events_.front()->message->has_page() ? 1 : 0;
      } else {
        events_.clear();
        buffered_pages_ = 0;
      }
      request_finish_locked(grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "client did not consume page events within the bounded stream buffer"));
      return PageScheduler::DeliveryResult::kCancelled;
    }
    ++buffered_pages_;
    completed_pages_.emplace(page_number, std::move(page));
    while (true) {
      auto page_it = completed_pages_.find(next_page_);
      if (page_it == completed_pages_.end()) break;
      auto event = std::make_unique<ArenaEvent>();
      event->message->set_document_id(document_id_);
      event->message->set_total_pages(total_pages_);
      append_page_data(*page_it->second, next_page_, &assembly_cursor_, event->message->mutable_page());
      // Heading depth needs every page's heights; the terminal event ships
      // the clustered result for the level-0 headers streamed here.
      collect_header_heights(event->message->page(), &header_heights_);
      completed_pages_.erase(page_it);
      events_.push_back(std::move(event));
      ++next_page_;
    }
    pump_locked();
    return PageScheduler::DeliveryResult::kAccepted;
  }

  void on_scheduler_finish(std::exception_ptr failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_ || context_->IsCancelled()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
      return;
    }
    grpc::Status status = status_from_exception(std::move(failure));
    if (status.ok() && (next_page_ != total_pages_ + 1 || !completed_pages_.empty())) {
      status = grpc::Status(grpc::StatusCode::INTERNAL,
                            "scheduler completed before every page was assembled");
    }
    if (status.ok()) {
      ++succeeded_parts_;
    } else {
      record_part_failure_locked(pipestream::parse::v1::COLLECTOR_GRPARSE_CV, status);
    }
    part_done_locked();
    pump_locked();
  }

  void on_collector_done(pipestream::parse::v1::Collector collector, CollectorOutcome outcome) {
    // A collector's Document is complete the moment it lands here, and this
    // is where it is projected and emitted, so the repair pass runs on it
    // first, on the caller's thread and outside the reactor lock: it is
    // straight-line work on the outcome alone.
    if (outcome.success && repair_.has_value()) run_repair_pass(&outcome.document, *repair_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return;
    if (outcome.success) {
      // A collector's finished document reaches the stream as page events
      // first, the same shape the CV pipeline emits page by page, so a
      // consumer that renders pages sees an inspector-routed text PDF
      // exactly as it sees a rasterized one. The whole document follows
      // as the collector-document event for consumers that want it intact.
      // The pdf collector's text is the document's own text layer; other
      // collectors' text has no OCR-or-digital story to tell.
      auto pages = project_page_data(
          outcome.document, collector == pipestream::parse::v1::COLLECTOR_PDF
                                ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                : pipestream::parse::v1::TEXT_SOURCE_UNSPECIFIED);
      if (!pages.empty()) {
        total_pages_ = std::max(total_pages_, static_cast<int>(pages.size()));
        for (auto& page : pages) {
          auto page_event = std::make_unique<ArenaEvent>();
          page_event->message->set_document_id(document_id_);
          page_event->message->set_total_pages(total_pages_);
          *page_event->message->mutable_page() = std::move(page);
          events_.push_back(std::move(page_event));
        }
      }
      auto event = std::make_unique<ArenaEvent>();
      event->message->set_document_id(document_id_);
      event->message->set_total_pages(total_pages_);
      auto* collector_document = event->message->mutable_collector_document();
      collector_document->set_collector(collector);
      *collector_document->mutable_document() = std::move(outcome.document);
      for (auto& warning : outcome.warnings) {
        collector_document->add_warnings(std::move(warning));
      }
      events_.push_back(std::move(event));
      ++succeeded_parts_;
    } else {
      record_part_failure_locked(collector,
                                 grpc::Status(outcome.code, outcome.error));
    }
    part_done_locked();
    pump_locked();
  }

  void record_part_failure_locked(pipestream::parse::v1::Collector collector,
                                  grpc::Status status) {
    auto* failure = collector_failures_.Add();
    failure->set_collector(collector);
    failure->set_error(status.error_message());
    if (first_failure_status_.ok()) first_failure_status_ = std::move(status);
  }

  // One collector part finished. When the last one lands, either every part
  // failed (the stream fails with the first failure's status) or the
  // terminal complete event carries the origin and the per-collector
  // failures of an otherwise successful parse.
  void part_done_locked() {
    if (pending_parts_ > 0) --pending_parts_;
    if (pending_parts_ > 0 || finish_requested_) return;
    if (succeeded_parts_ == 0) {
      request_finish_locked(first_failure_status_.ok()
                                ? grpc::Status(grpc::StatusCode::INTERNAL,
                                               "every collector failed")
                                : first_failure_status_);
      return;
    }
    auto event = std::make_unique<ArenaEvent>();
    event->message->set_document_id(document_id_);
    event->message->set_total_pages(total_pages_);
    auto* complete = event->message->mutable_complete();
    auto* origin = complete->mutable_origin();
    origin->set_filename(filename_.string());
    origin->set_mimetype(origin_mimetype_.mimetype);
    origin->set_mimetype_evidence(origin_mimetype_.evidence);
    origin->set_binary_hash(document_bytes_hash_);
    *complete->mutable_collector_failures() = collector_failures_;
    if (!header_heights_.empty()) {
      auto levels = section_header_levels(std::move(header_heights_));
      complete->mutable_section_header_levels()->insert(levels.begin(), levels.end());
    }
    events_.push_back(std::move(event));
    request_finish_locked(grpc::Status::OK);
  }

  void request_finish_locked(grpc::Status status) {
    if (finish_started_) return;
    if (finish_requested_) {
      if (finish_status_.ok() && !status.ok()) {
        finish_status_ = std::move(status);
        cancel_ticket_locked();
      }
      pump_locked();
      return;
    }
    finish_requested_ = true;
    finish_status_ = std::move(status);
    if (!finish_status_.ok()) cancel_ticket_locked();
    pump_locked();
  }

  void cancel_ticket_locked() {
    if (ticket_cancelled_ || !ticket_.valid()) return;
    ticket_cancelled_ = true;
    ticket_.cancel();
  }

  void pump_locked() {
    if (!write_in_flight_ && !events_.empty()) {
      write_in_flight_ = true;
      StartWrite(events_.front()->message);
      return;
    }
    if (finish_requested_ && !write_in_flight_ && events_.empty() && !finish_started_) {
      finish_started_ = true;
      Finish(finish_status_);
    }
  }

  grpc::CallbackServerContext* context_;
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  const std::optional<RepairOptions> repair_;
  const size_t maximum_buffered_pages_;
  std::shared_ptr<CallbackGate> callback_gate_;
  std::mutex mutex_;
  pipestream::parse::v1::DocumentChunk incoming_;
  std::string document_id_;
  fs::path filename_;
  std::string content_type_;
  std::string bytes_;
  uint64_t document_bytes_hash_ = 0;
  // The origin mimetype and its evidence, resolved while the bytes were
  // still in hand.
  MimetypeResolution origin_mimetype_;
  PageScheduler::Ticket ticket_;
  std::map<int, std::shared_ptr<const OcrPage>> completed_pages_;
  std::deque<std::unique_ptr<ArenaEvent>> events_;
  AssemblyCursor assembly_cursor_;
  // Level-less section headers streamed so far, clustered into depths for
  // the terminal event.
  std::vector<HeaderHeight> header_heights_;
  grpc::Status finish_status_;
  std::vector<pipestream::parse::v1::Collector> requested_collectors_;
  // Recognition fields resolved from the first chunk that set each one;
  // empty means the chunk stream never set it.
  std::optional<bool> do_ocr_;
  std::optional<bool> force_ocr_;
  std::optional<double> render_scale_;
  google::protobuf::RepeatedPtrField<pipestream::parse::v1::CollectorFailure>
      collector_failures_;
  grpc::Status first_failure_status_ = grpc::Status::OK;
  int pending_parts_ = 0;
  int succeeded_parts_ = 0;
  int total_pages_ = 0;
  int next_page_ = 1;
  size_t buffered_pages_ = 0;
  bool pdf_ = false;
  bool complete_seen_ = false;
  bool write_in_flight_ = false;
  bool client_cancelled_ = false;
  bool ticket_cancelled_ = false;
  bool finish_requested_ = false;
  bool finish_started_ = false;
};

}  // namespace

DocumentStreamingService::DocumentStreamingService(PageScheduler& scheduler,
                                                   std::shared_ptr<CollectorEndpoints> endpoints,
                                                   std::optional<RepairOptions> repair)
    : scheduler_(scheduler), endpoints_(std::move(endpoints)), repair_(std::move(repair)) {}

grpc::ServerBidiReactor<pipestream::parse::v1::DocumentChunk, pipestream::parse::v1::DocumentStreamEvent>*
DocumentStreamingService::StreamProcessDocument(grpc::CallbackServerContext* context) {
  return new DocumentStreamReactor(context, scheduler_, endpoints_, repair_);
}

}  // namespace grparse
