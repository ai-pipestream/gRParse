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
#include <vector>

#include "grparse/base64.h"
#include "grparse/document_assembly.h"
#include "grparse/in_memory_document.h"

namespace fs = std::filesystem;
namespace docling = ai::docling;

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
  return "image/png";
}

bool is_pdf(const std::string& content, const fs::path& filename) {
  return filename.extension() == ".pdf" ||
         (content.size() >= 5 && content.compare(0, 5, "%PDF-") == 0);
}

bool requested(const docling::serve::v1::ConvertDocumentOptions& options,
               docling::serve::v1::OutputFormat format) {
  return options.to_formats().empty() ||
         std::find(options.to_formats().begin(), options.to_formats().end(), format) != options.to_formats().end();
}

grpc::Status validate_options(const docling::serve::v1::ConvertDocumentOptions& options) {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (field->name() != "to_formats") {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "ConvertSource does not implement option '" + std::string(field->name()) + "'");
    }
  }
  for (const auto format : options.to_formats()) {
    if (format != docling::serve::v1::OUTPUT_FORMAT_TEXT) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "ConvertSource currently supports only TEXT output");
    }
  }
  return grpc::Status::OK;
}

grpc::Status status_from_exception(std::exception_ptr failure);

}  // namespace

DocumentParserService::DocumentParserService(PageScheduler& scheduler) : scheduler_(scheduler) {}

grpc::Status DocumentParserService::ConvertSource(
    grpc::ServerContext* context, const docling::serve::v1::ConvertSourceRequest* request,
    docling::serve::v1::ConvertSourceResponse* response) {
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

    auto* converted = response->mutable_response();
    auto* result = converted->mutable_document();
    result->set_filename(requested_name.string());
    auto* document = result->mutable_doc();
    document->set_schema_name("docling_document");
    document->set_version("1.0.0");
    document->set_name(requested_name.filename().string());
    auto* origin = document->mutable_origin();
    origin->set_filename(requested_name.filename().string());
    origin->set_mimetype(pdf ? "application/pdf" : mimetype_for(requested_name));
    origin->set_binary_hash(content_hash(*bytes));
    document->mutable_body()->set_self_ref("#/body");
    document->mutable_body()->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);

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
      return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
    }
    const grpc::Status scheduler_status = status_from_exception(state.failure);
    if (!scheduler_status.ok()) return scheduler_status;
    if (state.total_pages <= 0 || state.pages.size() != static_cast<size_t>(state.total_pages)) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "scheduler completed before every page was available");
    }

    std::string plain_text;
    AssemblyCursor assembly_cursor;
    for (int page_number = 1; page_number <= state.total_pages; ++page_number) {
      const auto page = state.pages.find(page_number);
      if (page == state.pages.end()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "scheduler omitted a document page");
      }
      append_page_to_document(*page->second, page_number, &assembly_cursor, document, &plain_text);
    }
    lock.unlock();
    if (requested(request->request().options(), docling::serve::v1::OUTPUT_FORMAT_TEXT)) {
      result->mutable_exports()->set_text(plain_text);
    }
    converted->set_status(docling::serve::v1::CONVERSION_STATUS_SUCCESS);
    converted->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    (*converted->mutable_timings())["total"] = converted->processing_time();
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

grpc::Status DocumentParserService::Health(grpc::ServerContext*, const docling::serve::v1::HealthRequest*,
                                            docling::serve::v1::HealthResponse* response) {
  response->set_status("ready");
  response->set_version("grparse-0.1.0-cuda");
  return grpc::Status::OK;
}

namespace {

constexpr size_t kMaximumDocumentBytes = 50U * 1024U * 1024U;
constexpr size_t kMaximumBufferedPages = 4;

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
  ArenaEvent() : message(google::protobuf::Arena::Create<docling::serve::v1::DocumentStreamEvent>(&arena)) {}

  google::protobuf::Arena arena;
  docling::serve::v1::DocumentStreamEvent* message;
};

class DocumentStreamReactor final
    : public grpc::ServerBidiReactor<docling::serve::v1::DocumentChunk,
                                     docling::serve::v1::DocumentStreamEvent> {
 public:
  DocumentStreamReactor(grpc::CallbackServerContext* context, PageScheduler& scheduler)
      : context_(context), scheduler_(scheduler), callback_gate_(std::make_shared<CallbackGate>()) {
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
    // gRPC Callback API transfers ownership of the reactor; OnDone must delete it.
    // CallbackGate + weak_ptr keep scheduler callbacks from touching a dead reactor.
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bytes = std::make_shared<const std::string>(std::move(bytes_));
      document_bytes_ = bytes;
      pdf = content_type_ == "application/pdf" || is_pdf(*bytes, filename_);
      pdf_ = pdf;
    }

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
      document_started_ = false;
      request_finish_locked(status_from_exception(std::current_exception()));
    }
  }

  void on_document(int total_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_pages_ = total_pages;
  }

  PageScheduler::DeliveryResult on_page(int page_number, std::shared_ptr<const OcrPage> page) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return PageScheduler::DeliveryResult::kCancelled;
    if (buffered_pages_ >= kMaximumBufferedPages) {
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
    const grpc::Status status = status_from_exception(std::move(failure));
    if (!status.ok()) {
      request_finish_locked(status);
      return;
    }
    if (next_page_ != total_pages_ + 1 || !completed_pages_.empty()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INTERNAL,
                                         "scheduler completed before every page was assembled"));
      return;
    }
    auto event = std::make_unique<ArenaEvent>();
    event->message->set_document_id(document_id_);
    event->message->set_total_pages(total_pages_);
    auto* origin = event->message->mutable_complete()->mutable_origin();
    origin->set_filename(filename_.string());
    origin->set_mimetype(pdf_ ? "application/pdf" : mimetype_for(filename_));
    origin->set_binary_hash(content_hash(*document_bytes_));
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
  std::shared_ptr<CallbackGate> callback_gate_;
  std::mutex mutex_;
  docling::serve::v1::DocumentChunk incoming_;
  std::string document_id_;
  fs::path filename_;
  std::string content_type_;
  std::string bytes_;
  std::shared_ptr<const std::string> document_bytes_;
  PageScheduler::Ticket ticket_;
  std::map<int, std::shared_ptr<const OcrPage>> completed_pages_;
  std::deque<std::unique_ptr<ArenaEvent>> events_;
  AssemblyCursor assembly_cursor_;
  grpc::Status finish_status_;
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

DocumentStreamingService::DocumentStreamingService(PageScheduler& scheduler) : scheduler_(scheduler) {}

grpc::ServerBidiReactor<docling::serve::v1::DocumentChunk, docling::serve::v1::DocumentStreamEvent>*
DocumentStreamingService::StreamProcessDocument(grpc::CallbackServerContext* context) {
  return new DocumentStreamReactor(context, scheduler_);
}

}  // namespace grparse
