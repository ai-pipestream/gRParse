#include "grparse/document_parser_service.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <google/protobuf/arena.h>
#include <google/protobuf/descriptor.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "grparse/base64.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_assembly.h"
#include "grparse/document_merge.h"
#include "grparse/in_memory_document.h"
#include "grparse/office_collector.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

uint64_t content_hash(const std::string& document) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : document) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string mimetype_for(const fs::path& path) {
  const auto extension = path.extension().string();
  if (extension == ".pdf") return "application/pdf";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".tif" || extension == ".tiff") return "image/tiff";
  if (extension == ".docx") {
    return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  }
  if (extension == ".xlsx") {
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  }
  if (extension == ".pptx") {
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  }
  if (extension == ".odt") return "application/vnd.oasis.opendocument.text";
  if (extension == ".ods") return "application/vnd.oasis.opendocument.spreadsheet";
  if (extension == ".odp") return "application/vnd.oasis.opendocument.presentation";
  if (extension == ".doc") return "application/msword";
  if (extension == ".xls") return "application/vnd.ms-excel";
  if (extension == ".ppt") return "application/vnd.ms-powerpoint";
  if (extension == ".csv") return "text/csv";
  if (extension == ".rtf") return "application/rtf";
  return "image/png";
}

bool is_pdf(const std::string& content, const fs::path& filename) {
  return filename.extension() == ".pdf" ||
         (content.size() >= 5 && content.compare(0, 5, "%PDF-") == 0);
}

const char* collector_name(pipestream::parse::v1::Collector collector) {
  switch (collector) {
    case pipestream::parse::v1::COLLECTOR_GRPARSE_CV: return "grparse-cv";
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return "libreoffice";
    case pipestream::parse::v1::COLLECTOR_POI: return "poi";
    case pipestream::parse::v1::COLLECTOR_CALAMINE: return "calamine";
    default: return "unspecified";
  }
}

std::vector<pipestream::parse::v1::Collector> requested_collectors(
    const google::protobuf::RepeatedField<int>& raw) {
  std::vector<pipestream::parse::v1::Collector> collectors;
  collectors.reserve(raw.size());
  for (const int value : raw) {
    collectors.push_back(static_cast<pipestream::parse::v1::Collector>(value));
  }
  return collectors;
}

// The document's plain text export: its text items in arena order, which is
// each collector's emission order.
std::string document_plain_text(const pipestream::document::v1::Document& document) {
  std::string text;
  for (const auto& item : document.texts()) {
    const pipestream::document::v1::TextItemBase* base = nullptr;
    switch (item.item_case()) {
      case pipestream::document::v1::BaseTextItem::kTitle: base = &item.title().base(); break;
      case pipestream::document::v1::BaseTextItem::kSectionHeader:
        base = &item.section_header().base();
        break;
      case pipestream::document::v1::BaseTextItem::kListItem:
        base = &item.list_item().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFormula: base = &item.formula().base(); break;
      case pipestream::document::v1::BaseTextItem::kText: base = &item.text().base(); break;
      case pipestream::document::v1::BaseTextItem::kCode:
        // CodeItem carries its fields inline instead of a nested base.
        if (!text.empty()) text.push_back('\n');
        text.append(item.code().text());
        break;
      case pipestream::document::v1::BaseTextItem::kFieldHeading:
        base = &item.field_heading().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFieldValue:
        base = &item.field_value().base();
        break;
      case pipestream::document::v1::BaseTextItem::ITEM_NOT_SET: break;
    }
    if (base == nullptr) continue;
    if (!text.empty()) text.push_back('\n');
    text.append(base->text());
  }
  return text;
}

bool requested(const pipestream::parse::v1::ConvertDocumentOptions& options,
               pipestream::parse::v1::OutputFormat format) {
  return options.to_formats().empty() ||
         std::find(options.to_formats().begin(), options.to_formats().end(), format) != options.to_formats().end();
}

grpc::Status validate_options(const pipestream::parse::v1::ConvertDocumentOptions& options) {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (field->name() != "to_formats" && field->name() != "collectors") {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "ConvertSource does not implement option '" + std::string(field->name()) + "'");
    }
  }
  for (const auto format : options.to_formats()) {
    if (format != pipestream::parse::v1::OUTPUT_FORMAT_TEXT) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "ConvertSource currently supports only TEXT output");
    }
  }
  return grpc::Status::OK;
}

grpc::Status status_from_exception(std::exception_ptr failure);

}  // namespace

std::shared_ptr<grpc::Channel> CollectorEndpoints::libreoffice_channel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_ == nullptr && !libreoffice_target_.empty()) {
    channel_ = grpc::CreateChannel(libreoffice_target_, grpc::InsecureChannelCredentials());
  }
  return channel_;
}

DocumentParserService::DocumentParserService(PageScheduler& scheduler,
                                             std::shared_ptr<CollectorEndpoints> endpoints)
    : scheduler_(scheduler), endpoints_(std::move(endpoints)) {}

grpc::Status DocumentParserService::ConvertSource(
    grpc::ServerContext* context, const pipestream::parse::v1::ConvertSourceRequest* request,
    pipestream::parse::v1::ConvertSourceResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  const auto& sources = request->request().sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "ConvertSource currently accepts exactly one FileSource containing base64_string");
  }
  const grpc::Status option_status = validate_options(request->request().options());
  if (!option_status.ok()) return option_status;
  try {
    const auto& source = sources.Get(0).file();
    auto bytes = std::make_shared<const std::string>(decode_base64(source.base64_string()));
    const fs::path requested_name = source.filename().empty() ? "document.pdf" : fs::path(source.filename()).filename();
    const bool pdf = is_pdf(*bytes, requested_name);
    const bool office = office_format(requested_name.string(), "");

    // The base document carries identity; every collector's output merges
    // into it additively, in plan order.
    pipestream::document::v1::Document base;
    base.set_schema_name("docling_document");
    base.set_version("1.0.0");
    base.set_name(requested_name.filename().string());
    auto* origin = base.mutable_origin();
    origin->set_filename(requested_name.filename().string());
    origin->set_mimetype(pdf ? "application/pdf" : mimetype_for(requested_name));
    origin->set_binary_hash(content_hash(*bytes));
    base.mutable_body()->set_self_ref("#/body");
    base.mutable_body()->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
    base.mutable_furniture()->set_self_ref("#/furniture");
    base.mutable_furniture()->set_content_layer(
        pipestream::document::v1::CONTENT_LAYER_FURNITURE);

    // The in-process CV collector: the page scheduler's layout, OCR, and
    // model pipeline over rendered pages, assembled into a document
    // fragment. Never throws; failures become the outcome.
    auto run_cv = [&]() -> CollectorOutcome {
      CollectorOutcome outcome;
      try {
        struct UnaryResult {
          std::mutex mutex;
          std::condition_variable changed;
          std::map<int, std::shared_ptr<const OcrPage>> pages;
          std::exception_ptr failure;
          int total_pages = 0;
          bool finished = false;
        } state;

        const auto ticket = scheduler_.submit(
            bytes, pdf,
            PageScheduler::Callbacks{
                [&state](int total_pages) {
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.total_pages = total_pages;
                  state.changed.notify_all();
                },
                [&state, context](int page_number, std::shared_ptr<const OcrPage> page) {
                  if (context->IsCancelled()) return PageScheduler::DeliveryResult::kCancelled;
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.pages.emplace(page_number, std::move(page));
                  return PageScheduler::DeliveryResult::kAcceptedAndRelease;
                },
                [&state](std::exception_ptr failure) {
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.failure = std::move(failure);
                  state.finished = true;
                  state.changed.notify_all();
                }});

        std::unique_lock<std::mutex> lock(state.mutex);
        while (!state.finished) {
          state.changed.wait_for(lock, std::chrono::milliseconds(25));
          if (context->IsCancelled()) ticket.cancel();
        }
        if (context->IsCancelled()) {
          outcome.error = "request cancelled";
          outcome.code = grpc::StatusCode::CANCELLED;
          return outcome;
        }
        const grpc::Status scheduler_status = status_from_exception(state.failure);
        if (!scheduler_status.ok()) {
          outcome.error = scheduler_status.error_message();
          outcome.code = scheduler_status.error_code();
          return outcome;
        }
        if (state.total_pages <= 0 || state.pages.size() != static_cast<size_t>(state.total_pages)) {
          outcome.error = "scheduler completed before every page was available";
          return outcome;
        }
        std::string plain_text;
        AssemblyCursor assembly_cursor;
        for (int page_number = 1; page_number <= state.total_pages; ++page_number) {
          const auto page = state.pages.find(page_number);
          if (page == state.pages.end()) {
            outcome.error = "scheduler omitted a document page";
            return outcome;
          }
          append_page_to_document(*page->second, page_number, &assembly_cursor,
                                  &outcome.document, &plain_text);
        }
        outcome.success = true;
        return outcome;
      } catch (...) {
        const grpc::Status status = status_from_exception(std::current_exception());
        outcome.error = status.error_message();
        outcome.code = status.error_code();
        return outcome;
      }
    };

    auto endpoints = endpoints_;
    auto run_libreoffice = [endpoints, bytes, requested_name]() -> CollectorOutcome {
      if (endpoints == nullptr || !endpoints->has_libreoffice()) {
        CollectorOutcome outcome;
        outcome.error =
            "libreoffice collector is not configured (GRPARSE_LIBREOFFICE_TARGET)";
        outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
        return outcome;
      }
      return collect_office_document(endpoints->libreoffice_channel(),
                                     requested_name.string(), requested_name.string(),
                                     std::string(), *bytes);
    };

    std::vector<PlannedCollector> plan;
    for (const auto id : resolve_collectors(
             requested_collectors(request->request().options().collectors()), office)) {
      PlannedCollector collector;
      collector.id = id;
      switch (id) {
        case pipestream::parse::v1::COLLECTOR_GRPARSE_CV:
          collector.run = run_cv;
          break;
        case pipestream::parse::v1::COLLECTOR_LIBREOFFICE:
          collector.run = run_libreoffice;
          break;
        default:
          collector.run = [id]() {
            CollectorOutcome outcome;
            outcome.error = std::string("collector '") + collector_name(id) +
                            "' is not wired in yet";
            outcome.code = grpc::StatusCode::UNIMPLEMENTED;
            return outcome;
          };
          break;
      }
      plan.push_back(std::move(collector));
    }

    CoordinatorResult result = run_collectors(std::move(plan), std::move(base));
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
    }
    if (result.succeeded == 0) {
      const auto& first = result.failures.front();
      std::string message;
      for (const auto& failure : result.failures) {
        if (!message.empty()) message += "; ";
        message += std::string(collector_name(failure.id)) + ": " + failure.error;
      }
      return grpc::Status(first.code, message);
    }

    auto* converted = response->mutable_response();
    auto* document_response = converted->mutable_document();
    document_response->set_filename(requested_name.string());
    auto* document = document_response->mutable_doc();
    *document = std::move(result.document);
    // Collector warnings are not failures; they stay on the document, keyed
    // by collector, so nothing the collectors reported is dropped.
    for (const auto& warning : result.warnings) {
      auto& fields = *document->mutable_body()->mutable_meta()->mutable_custom_fields();
      *fields[std::string("collector_warnings:") + collector_name(warning.first)]
           .mutable_list_value()
           ->add_values()
           ->mutable_string_value() = warning.second;
    }
    for (const auto& failure : result.failures) {
      auto* error = converted->add_errors();
      error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
      error->set_module_name(std::string("collector:") + collector_name(failure.id));
      error->set_error_message(failure.error);
    }
    if (requested(request->request().options(), pipestream::parse::v1::OUTPUT_FORMAT_TEXT)) {
      document_response->mutable_exports()->set_text(document_plain_text(*document));
    }
    converted->set_status(result.failures.empty()
                              ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                              : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
    converted->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    (*converted->mutable_timings())["total"] = converted->processing_time();
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

grpc::Status DocumentParserService::Health(grpc::ServerContext*, const pipestream::parse::v1::HealthRequest*,
                                            pipestream::parse::v1::HealthResponse* response) {
  response->set_status("ready");
  response->set_version("grparse-0.1.0-cuda");
  return grpc::Status::OK;
}

namespace {

constexpr size_t kMaximumDocumentBytes = 50U * 1024U * 1024U;

grpc::Status status_from_exception(std::exception_ptr failure) {
  try {
    if (failure) std::rethrow_exception(failure);
  } catch (const InvalidDocument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const SchedulerSaturated& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::bad_alloc& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::invalid_argument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::exception& error) {
    return grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  }
  return grpc::Status::OK;
}

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
                        std::shared_ptr<CollectorEndpoints> endpoints)
      : context_(context),
        scheduler_(scheduler),
        endpoints_(std::move(endpoints)),
        // The scheduler will not deliver more than one page window ahead of the
        // credits this reactor returns, so the window *is* the buffer bound.
        // A constant here silently capped GRPARSE_PAGE_WINDOW and killed
        // well-behaved clients with RESOURCE_EXHAUSTED once it was raised.
        maximum_buffered_pages_(scheduler.page_window()),
        callback_gate_(std::make_shared<CallbackGate>()) {
    callback_gate_->reactor = this;
    StartRead(&incoming_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      bool should_begin = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        reads_done_ = true;
        if (client_cancelled_) {
          request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
          return;
        }
        if (!complete_seen_ || bytes_.empty()) {
          request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                             "stream must end with a non-empty complete chunk"));
          return;
        }
        should_begin = true;
        document_started_ = true;
      }
      if (should_begin) begin_processing();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finish_requested_) return;
      if (complete_seen_) {
        request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "received data after complete chunk"));
        return;
      }
      if (document_id_.empty()) {
        document_id_ = incoming_.document_id();
        filename_ = fs::path(incoming_.filename()).filename();
        content_type_ = incoming_.content_type();
        if (document_id_.empty() || filename_.empty()) {
          request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                             "first chunk requires document_id and filename"));
          return;
        }
      } else if ((!incoming_.document_id().empty() && incoming_.document_id() != document_id_) ||
                 (!incoming_.filename().empty() && fs::path(incoming_.filename()).filename() != filename_)) {
        request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "all chunks must describe the same document"));
        return;
      }
      // The collector selection resolves like the identity fields: the
      // first chunk whose list is non-empty wins.
      if (requested_collectors_.empty() && !incoming_.collectors().empty()) {
        for (const int value : incoming_.collectors()) {
          requested_collectors_.push_back(static_cast<pipestream::parse::v1::Collector>(value));
        }
      }
      if (incoming_.data().size() > kMaximumDocumentBytes - bytes_.size()) {
        request_finish_locked(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                           "document exceeds 50 MiB streaming limit"));
        return;
      }
      bytes_.append(incoming_.data());
      complete_seen_ = incoming_.complete();
      incoming_.Clear();
      StartRead(&incoming_);
    }
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
    // so the gate mutex — not just the null check — is what keeps this delete
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

  void begin_processing() {
    std::shared_ptr<const std::string> bytes;
    bool pdf = false;
    bool want_cv = false;
    bool want_libreoffice = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bytes = std::make_shared<const std::string>(std::move(bytes_));
      pdf = content_type_ == "application/pdf" || is_pdf(*bytes, filename_);
      pdf_ = pdf;
      // Scatter-gather routing: the plan resolves from the request's
      // collector selection or the document's format, unwired collectors
      // fail immediately, and each wired collector is one pending part of
      // the stream. The parse degrades collector by collector instead of
      // failing while any part succeeds.
      const auto plan = resolve_collectors(
          requested_collectors_, office_format(filename_.string(), content_type_));
      for (const auto id : plan) {
        if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
          want_cv = true;
          ++pending_parts_;
        } else if (id == pipestream::parse::v1::COLLECTOR_LIBREOFFICE) {
          want_libreoffice = true;
          ++pending_parts_;
        } else {
          record_part_failure_locked(
              id, grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               std::string("collector '") + collector_name(id) +
                                   "' is not wired in yet"));
        }
      }
      if (pending_parts_ == 0) {
        request_finish_locked(first_failure_status_);
        return;
      }
    }
    // Hash the request once, here, rather than under the reactor lock at
    // completion: it is a linear pass over up to 50 MiB.
    const uint64_t hash = content_hash(*bytes);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      document_bytes_hash_ = hash;
    }

    if (want_libreoffice) spawn_office_collector(bytes);
    if (!want_cv) return;

    try {
      const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
      auto ticket = scheduler_.submit(
          bytes, pdf,
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

  // The libreoffice collector runs on its own thread: it is a blocking
  // client stream, not a gRPC reaction. The gate keeps its completion safe
  // against reactor teardown exactly like the scheduler callbacks. A client
  // cancel abandons the result; the collector's own deadline bounds the
  // orphaned call.
  void spawn_office_collector(std::shared_ptr<const std::string> bytes) {
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
    std::thread([weak_gate, endpoints, bytes, document_id, filename, content_type]() {
      CollectorOutcome outcome;
      if (endpoints == nullptr || !endpoints->has_libreoffice()) {
        outcome.error =
            "libreoffice collector is not configured (GRPARSE_LIBREOFFICE_TARGET)";
        outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
      } else {
        outcome = collect_office_document(endpoints->libreoffice_channel(), document_id,
                                          filename, content_type, *bytes);
      }
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->on_collector_done(pipestream::parse::v1::COLLECTOR_LIBREOFFICE,
                                           std::move(outcome));
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return;
    if (outcome.success) {
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
    origin->set_mimetype(pdf_ ? "application/pdf" : mimetype_for(filename_));
    origin->set_binary_hash(document_bytes_hash_);
    *complete->mutable_collector_failures() = collector_failures_;
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
  const size_t maximum_buffered_pages_;
  std::shared_ptr<CallbackGate> callback_gate_;
  std::mutex mutex_;
  pipestream::parse::v1::DocumentChunk incoming_;
  std::string document_id_;
  fs::path filename_;
  std::string content_type_;
  std::string bytes_;
  uint64_t document_bytes_hash_ = 0;
  PageScheduler::Ticket ticket_;
  std::map<int, std::shared_ptr<const OcrPage>> completed_pages_;
  std::deque<std::unique_ptr<ArenaEvent>> events_;
  AssemblyCursor assembly_cursor_;
  grpc::Status finish_status_;
  std::vector<pipestream::parse::v1::Collector> requested_collectors_;
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
  bool reads_done_ = false;
  bool document_started_ = false;
  bool write_in_flight_ = false;
  bool client_cancelled_ = false;
  bool ticket_cancelled_ = false;
  bool finish_requested_ = false;
  bool finish_started_ = false;
};

}  // namespace

DocumentStreamingService::DocumentStreamingService(PageScheduler& scheduler,
                                                   std::shared_ptr<CollectorEndpoints> endpoints)
    : scheduler_(scheduler), endpoints_(std::move(endpoints)) {}

grpc::ServerBidiReactor<pipestream::parse::v1::DocumentChunk, pipestream::parse::v1::DocumentStreamEvent>*
DocumentStreamingService::StreamProcessDocument(grpc::CallbackServerContext* context) {
  return new DocumentStreamReactor(context, scheduler_, endpoints_);
}

}  // namespace grparse
