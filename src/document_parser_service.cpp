#include "grparse/document_parser_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <mutex>
#include <type_traits>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/alarm.h>

#include "chunking/chunker.h"
#include "conversion_outputs.h"
#include "grparse/collector_coordinator.h"
#include "grparse/prometheus_metrics.h"
#include "grparse/service_health.h"
#include "grparse/service_version.h"
#include "parse_support.h"
#include "source_parse.h"
#include "targets/target_step.h"
#include "task_registry.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

std::string_view request_doctype(
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources) {
  if (sources.size() == 1 && sources.Get(0).has_file()) {
    return classify_doctype(pipestream::parse::v1::INPUT_FORMAT_UNSPECIFIED,
                            sources.Get(0).file().filename());
  }
  return "other";
}

bool is_rejection_code(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::FAILED_PRECONDITION:
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::UNIMPLEMENTED:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return true;
    default:
      return false;
  }
}

void record_rpc_completion(const SourceParse& parsed, const grpc::Status& status,
                           bool partial_success,
                           std::chrono::steady_clock::time_point started) {
  const auto elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  DocumentCompletionStatus completion = DocumentCompletionStatus::kSuccess;
  if (!status.ok()) {
    completion = (!parsed.collectors_ran && is_rejection_code(status.error_code()))
                     ? DocumentCompletionStatus::kRejected
                     : DocumentCompletionStatus::kFailure;
  } else if (partial_success) {
    completion = DocumentCompletionStatus::kPartialSuccess;
  }
  record_document_completion(parsed.doctype, completion, elapsed_ns);
}

// The collector failures of a parse, as the error list every conversion
// surface reports them under.
void report_failures(const std::vector<CollectorFailureInfo>& failures,
                     google::protobuf::RepeatedPtrField<pipestream::parse::v1::ErrorItem>* errors) {
  for (const auto& failure : failures) {
    auto* error = errors->Add();
    error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
    error->set_module_name(std::string("collector:") + collector_name(failure.id));
    error->set_error_message(failure.error);
    error->set_category(failure_category_for(failure.code));
  }
}

// The converted document a chunk response carries when the caller asked for
// it. The chunks themselves never depend on it.
void attach_converted_document(const pipestream::parse::v1::ConvertDocumentOptions& options,
                               const fs::path& filename, CoordinatorResult* result,
                               pipestream::parse::v1::ChunkDocumentResponse* response) {
  auto* entry = response->add_documents();
  entry->set_kind("document");
  auto* content = entry->mutable_content();
  content->set_filename(filename.string());
  *content->mutable_doc() = std::move(result->document);
  render_exports(options, content->doc(), content->mutable_exports());
  entry->set_status(result->failures.empty()
                        ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                        : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  report_failures(result->failures, entry->mutable_errors());
}

// Without the converted document, a parse some collector failed still says
// so: the failures ride a documents entry with no content, the one place a
// chunk response can carry a status and errors.
void attach_failures(const std::vector<CollectorFailureInfo>& failures,
                     pipestream::parse::v1::ChunkDocumentResponse* response) {
  auto* entry = response->add_documents();
  entry->set_kind("document");
  entry->set_status(pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  report_failures(failures, entry->mutable_errors());
}

// The chunk responses have no target result to report a delivery in, so a
// target that asks for one is turned down rather than silently skipped.
grpc::Status refuse_target(const pipestream::parse::v1::Target& target,
                           const std::string& surface) {
  if (!targets::needs_delivery(target)) return grpc::Status::OK;
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      surface + " does not deliver to a target; its chunks return in the "
                                "response body only");
}

// What a copy of the request's sources holds: the chunk surfaces copy them
// into the ConvertDocumentRequest the shared parse reads.
uint64_t sources_bytes(
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources) {
  uint64_t total = 0;
  for (const auto& source : sources) total += source.ByteSizeLong();
  return total;
}

// The wire message, the sources copied into the parse request, and the
// copy their base64 decodes to.
uint64_t chunk_charge(
    const google::protobuf::Message& request,
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources) {
  return request.ByteSizeLong() + sources_bytes(sources) + decoded_source_bytes(sources);
}

// Everything a chunk response carries beside the chunks: the converted
// document or the collector failures, the chunker's options, and the time.
template <typename ChunkRequest>
void describe_chunks(const ChunkRequest& chunk_request,
                     std::chrono::steady_clock::time_point started, SourceParse* parsed,
                     pipestream::parse::v1::ChunkDocumentResponse* response) {
  if (chunk_request.include_converted_doc()) {
    attach_converted_document(chunk_request.convert_options(), parsed->filename,
                              &parsed->result, response);
  } else if (!parsed->result.failures.empty()) {
    attach_failures(parsed->result.failures, response);
  }
  chunking::fill_chunking_info(chunk_request.chunking_options(),
                               response->mutable_chunking_info());
  response->set_processing_time(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

// The reactor the blocking unary surfaces finish through. Construction hands
// `work` to the executor and returns immediately, so the event-manager thread
// that reacted to the call is free the moment the handler returns; the worker
// finishes the call. gRPC queues operations requested before the reactor is
// bound, so finishing from either thread is safe whichever wins the race.
// Cancellation needs no OnCancel here: the work polls the context, which is
// where it can act on the answer.
//
// The request's bytes are charged to the in-flight budget before anything
// queues, and returned when the task that owns the call is gone (run, or
// refused by the executor); a charge past the budget is refused outright.
// A remote source's fetched copy is charged later, by parse_source, once
// the fetch has said how large it is.
class ParseUnaryReactor final : public grpc::ServerUnaryReactor {
 public:
  ParseUnaryReactor(grpc::CallbackServerContext* context, CallExecutor& executor,
                    const std::shared_ptr<InflightBytes>& inflight, uint64_t request_bytes,
                    std::string_view initial_doctype, std::function<grpc::Status()> work) {
    if (inflight != nullptr && !inflight->try_acquire(request_bytes)) {
      record_document_completion(initial_doctype, DocumentCompletionStatus::kRejected, 0);
      Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "in-flight document bytes would exceed GRPARSE_MAX_INFLIGHT_BYTES"));
      return;
    }
    // Shared because std::function must stay copyable; the last copy of the
    // task going away is what returns the charge.
    std::shared_ptr<void> charge(nullptr, [inflight, request_bytes](void*) {
      if (inflight != nullptr) inflight->release(request_bytes);
    });
    const bool queued = executor.submit([this, context, initial_doctype, work = std::move(work),
                                         charge = std::move(charge)] {
      // A call can wait in the queue behind every conversion ahead of it, so
      // the first thing a worker does is ask whether anyone is still
      // listening. The answer costs one atomic read and saves the whole
      // parse behind it: the base64 decode of up to a few hundred megabytes,
      // and every collector leg after that.
      if (context->IsCancelled()) {
        record_document_completion(initial_doctype, DocumentCompletionStatus::kFailure, 0);
        Finish(grpc::Status(grpc::StatusCode::CANCELLED,
                            "request cancelled before conversion started"));
        return;
      }
      grpc::Status status;
      try {
        status = work();
      } catch (...) {
        record_document_completion(initial_doctype, DocumentCompletionStatus::kFailure, 0);
        status = status_from_exception(std::current_exception());
      }
      // Last statement on purpose: OnDone can delete this reactor from
      // another thread the instant the call completes.
      Finish(std::move(status));
    });
    if (queued) return;
    // The reactor holds the call and is the only thing that can answer it, so
    // a refused submission is a status, not a dropped RPC.
    record_document_completion(initial_doctype, DocumentCompletionStatus::kRejected, 0);
    Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "conversion executor is saturated"));
  }

  void OnDone() override { delete this; }
};

// The reactor the streamed surfaces finish through. The work runs on the
// executor exactly as ParseUnaryReactor's does and fills every message the
// stream will carry. Only then do the writes start, one outstanding at a
// time, each OnWriteDone starting the next and releasing the message it
// sent. A work failure finishes the call before anything is written, so a
// reader never sees a partial stream end in OK.
//
// The in-flight charge is held by the reactor itself and returned in
// OnDone: the messages waiting to be written are as large as what the
// parse held, so the budget covers the call until its last write is done
// (or refused), not just until the work returns. The work hands a remote
// source's fetched charge to the reactor for the same reason.
template <typename Message>
class ParseWriteReactor final : public grpc::ServerWriteReactor<Message> {
 public:
  using Messages = std::vector<Message>;
  using Work = std::function<grpc::Status(Messages*, InflightCharge* fetched_charge)>;

  // A call refused before any work is queued.
  explicit ParseWriteReactor(grpc::Status refused) {
    record_document_completion("other", DocumentCompletionStatus::kRejected, 0);
    this->Finish(std::move(refused));
  }

  ParseWriteReactor(grpc::CallbackServerContext* context, CallExecutor& executor,
                    const std::shared_ptr<InflightBytes>& inflight, uint64_t request_bytes,
                    std::string_view initial_doctype, Work work) {
    if (inflight != nullptr && !inflight->try_acquire(request_bytes)) {
      record_document_completion(initial_doctype, DocumentCompletionStatus::kRejected, 0);
      this->Finish(
          grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                       "in-flight document bytes would exceed GRPARSE_MAX_INFLIGHT_BYTES"));
      return;
    }
    inflight_ = inflight;
    charge_ = request_bytes;
    const bool queued =
        executor.submit([this, context, initial_doctype, work = std::move(work)] {
          if (context->IsCancelled()) {
            record_document_completion(initial_doctype, DocumentCompletionStatus::kFailure, 0);
            this->Finish(grpc::Status(grpc::StatusCode::CANCELLED,
                                      "request cancelled before conversion started"));
            return;
          }
          grpc::Status status;
          try {
            status = work(&messages_, &fetched_charge_);
          } catch (...) {
            record_document_completion(initial_doctype, DocumentCompletionStatus::kFailure, 0);
            status = status_from_exception(std::current_exception());
          }
          if (!status.ok()) {
            messages_.clear();
            this->Finish(std::move(status));
            return;
          }
          // Last statement on purpose, as in ParseUnaryReactor: once the first
          // write is started the reactor can finish and be deleted elsewhere.
          write_next();
        });
    if (queued) return;
    record_document_completion(initial_doctype, DocumentCompletionStatus::kRejected, 0);
    this->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                              "conversion executor is saturated"));
  }

  void OnWriteDone(bool ok) override {
    if (!ok) {
      this->Finish(grpc::Status(grpc::StatusCode::CANCELLED,
                                "the stream broke before every message was written"));
      return;
    }
    messages_[next_ - 1].Clear();
    write_next();
  }

  void OnDone() override {
    if (inflight_ != nullptr) inflight_->release(charge_);
    delete this;
  }

 private:
  // The cursor moves before StartWrite because OnWriteDone can run on
  // another thread before StartWrite returns.
  void write_next() {
    if (next_ == messages_.size()) {
      this->Finish(grpc::Status::OK);
      return;
    }
    const auto& message = messages_[next_];
    ++next_;
    this->StartWrite(&message);
  }

  Messages messages_;
  std::size_t next_ = 0;
  // Set only once the charge is taken, so a refused call returns nothing.
  std::shared_ptr<InflightBytes> inflight_;
  uint64_t charge_ = 0;
  // A remote source's fetched copy, handed over by the work; returned when
  // OnDone deletes the reactor.
  InflightCharge fetched_charge_;
};

// A task's failure phase: admission while nothing was dialed, execution
// after.
pipestream::parse::v1::FailurePhase task_failure_phase(const SourceParse& parsed) {
  return parsed.collectors_ran ? pipestream::parse::v1::FAILURE_PHASE_EXECUTION
                               : pipestream::parse::v1::FAILURE_PHASE_ADMISSION;
}

// Where a status sits in a task's life; a watcher passes each rank once,
// in order, whichever of the registry's notification and its own
// subscription snapshot reaches it first.
int task_rank(pipestream::parse::v1::TaskStatus status) {
  switch (status) {
    case pipestream::parse::v1::TASK_STATUS_PENDING: return 1;
    case pipestream::parse::v1::TASK_STATUS_STARTED: return 2;
    case pipestream::parse::v1::TASK_STATUS_SUCCESS:
    case pipestream::parse::v1::TASK_STATUS_FAILURE: return 3;
    default: return 0;
  }
}

// A Watch* stream: the task's status when the watch began, then every
// status it takes after, written one at a time; the stream closes OK once
// the terminal status is written. The registry calls the listener under
// its own lock, so unwatch in OnDone returning means no listener is
// running into this reactor or ever will again. Finish runs from the
// constructor or a reaction only, never from the listener.
template <typename Response>
class TaskWatchReactor final : public grpc::ServerWriteReactor<Response> {
 public:
  explicit TaskWatchReactor(grpc::Status refused) { this->Finish(std::move(refused)); }

  TaskWatchReactor(TaskRegistry& tasks, const std::string& task_id) {
    auto subscribed = tasks.watch(task_id, [this](const TaskRegistry::Status& status) {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_locked(status);
    });
    if (!subscribed.has_value()) {
      this->Finish(subscribed.error());
      return;
    }
    tasks_ = &tasks;
    token_ = subscribed->token;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_locked(subscribed->current);
  }

  void OnWriteDone(bool ok) override {
    std::lock_guard<std::mutex> lock(mutex_);
    write_in_flight_ = false;
    if (!pending_.empty()) pending_.pop_front();
    if (!ok) {
      finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "the watcher stopped reading"));
      return;
    }
    pump_locked();
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "watch cancelled"));
  }

  void OnDone() override {
    if (tasks_ != nullptr) tasks_->unwatch(token_);
    delete this;
  }

 private:
  void queue_locked(const TaskRegistry::Status& status) {
    const int rank = task_rank(status.task_status());
    if (rank <= last_rank_ || finished_) return;
    last_rank_ = rank;
    Response message;
    *message.mutable_response() = status;
    pending_.push_back(std::move(message));
    if (TaskRegistry::terminal(status.task_status())) terminal_queued_ = true;
    if (!write_in_flight_) {
      write_in_flight_ = true;
      this->StartWrite(&pending_.front());
    }
  }

  // Called from reactions only: the next write, or the close once the
  // terminal status went out.
  void pump_locked() {
    if (finished_ || write_in_flight_) return;
    if (pending_.empty()) {
      if (terminal_queued_) finish_locked(grpc::Status::OK);
      return;
    }
    write_in_flight_ = true;
    this->StartWrite(&pending_.front());
  }

  void finish_locked(grpc::Status status) {
    if (finished_) return;
    finished_ = true;
    this->Finish(std::move(status));
  }

  TaskRegistry* tasks_ = nullptr;
  uint64_t token_ = 0;
  std::mutex mutex_;
  std::deque<Response> pending_;
  bool write_in_flight_ = false;
  bool terminal_queued_ = false;
  bool finished_ = false;
  int last_rank_ = 0;
};

// PollTaskStatus with a wait_time: answers the moment the task finishes,
// or with its status when the wait runs out, holding no thread meanwhile
// (a registry listener and a callback alarm). Finish runs once, under the
// reactor's lock; OnDone first detaches the alarm and unwatches, which
// waits out a listener or an alarm callback already running.
class TaskPollReactor final : public grpc::ServerUnaryReactor {
 public:
  TaskPollReactor(TaskRegistry& tasks, const std::string& task_id,
                  std::chrono::milliseconds wait,
                  pipestream::parse::v1::PollTaskStatusResponse* response)
      : response_(response), timer_(std::make_shared<Timer>()) {
    auto subscribed = tasks.watch(task_id, [this](const TaskRegistry::Status& status) {
      if (TaskRegistry::terminal(status.task_status())) answer(status);
    });
    if (!subscribed.has_value()) {
      Finish(subscribed.error());
      return;
    }
    tasks_ = &tasks;
    token_ = subscribed->token;
    if (TaskRegistry::terminal(subscribed->current.task_status())) {
      answer(subscribed->current);
      return;
    }
    timer_->reactor = this;
    timer_->fallback = subscribed->current;
    alarm_.Set(std::chrono::system_clock::now() + wait,
               [timer = timer_, tasks = tasks_, task_id](bool fired) {
                 if (!fired) return;
                 // Read before the timer's lock: the registry's lock is
                 // never taken under it.
                 const auto status = tasks->status(task_id);
                 std::lock_guard<std::mutex> lock(timer->mutex);
                 if (timer->reactor == nullptr) return;
                 timer->reactor->answer(status.value_or(timer->fallback));
               });
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (answered_) return;
    answered_ = true;
    Finish(grpc::Status(grpc::StatusCode::CANCELLED, "poll cancelled"));
  }

  void OnDone() override {
    {
      std::lock_guard<std::mutex> lock(timer_->mutex);
      timer_->reactor = nullptr;
    }
    alarm_.Cancel();
    if (tasks_ != nullptr) tasks_->unwatch(token_);
    delete this;
  }

 private:
  struct Timer {
    std::mutex mutex;
    TaskPollReactor* reactor = nullptr;
    TaskRegistry::Status fallback;
  };

  void answer(const TaskRegistry::Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (answered_) return;
    answered_ = true;
    *response_->mutable_response() = status;
    Finish(grpc::Status::OK);
  }

  pipestream::parse::v1::PollTaskStatusResponse* response_;
  TaskRegistry* tasks_ = nullptr;
  uint64_t token_ = 0;
  std::shared_ptr<Timer> timer_;
  grpc::Alarm alarm_;
  std::mutex mutex_;
  bool answered_ = false;
};

// The trivial surfaces answer on the reaction thread through the context's own
// reactor, which allocates nothing.
grpc::ServerUnaryReactor* finish_inline(grpc::CallbackServerContext* context,
                                        grpc::Status status) {
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}

}  // namespace

DocumentParserService::DocumentParserService(PageScheduler& scheduler,
                                             std::shared_ptr<CollectorEndpoints> endpoints,
                                             CallExecutor::Options executor_options,
                                             std::optional<RepairOptions> repair,
                                             std::shared_ptr<EmbeddingEngine> embedding_engine,
                                             EmbeddingConfig embedding_config,
                                             std::shared_ptr<InflightBytes> inflight)
    : scheduler_(scheduler),
      endpoints_(std::move(endpoints)),
      repair_(std::move(repair)),
      embedder_(std::move(embedding_engine), std::move(embedding_config)),
      inflight_(std::move(inflight)),
      tasks_(std::make_unique<TaskRegistry>(TaskRegistryOptions{}, inflight_)),
      executor_(executor_options) {}

DocumentParserService::~DocumentParserService() {
  // Before the executor joins: a task still queued or running sees its
  // ParseCall cancelled and ends instead of running to completion.
  tasks_->stop();
}

void DocumentParserService::set_task_options(const TaskRegistryOptions& options) {
  tasks_ = std::make_unique<TaskRegistry>(options, inflight_);
}

grpc::ServerWriteReactor<pipestream::parse::v1::ConvertSourceStreamResponse>*
DocumentParserService::ConvertSourceStream(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ConvertSourceStreamRequest* request) {
  const uint64_t charge =
      request->ByteSizeLong() + decoded_source_bytes(request->request().sources());
  const std::string_view initial_doctype = request_doctype(request->request().sources());
  return new ParseWriteReactor<pipestream::parse::v1::ConvertSourceStreamResponse>(
      context, executor_, inflight_, charge, initial_doctype,
      [this, context, request, initial_doctype](
          std::vector<pipestream::parse::v1::ConvertSourceStreamResponse>* messages,
          InflightCharge* fetched_charge) {
        const auto started = std::chrono::steady_clock::now();
        SourceParse parsed;
        parsed.doctype = initial_doctype;
        bool partial_success = false;
        auto& message = messages->emplace_back();
        grpc::Status status =
            convert_source(ParseCall(context), request->request(), "ConvertSourceStream",
                           &parsed, message.mutable_response(), &partial_success);
        if (status.ok()) {
          status = refuse_oversized_response(message, response_byte_cap_, "ConvertSourceStream");
        }
        record_rpc_completion(parsed, status, partial_success, started);
        if (!status.ok()) return status;
        *fetched_charge = std::move(parsed.fetched_charge);
        return grpc::Status::OK;
      });
}

grpc::Status DocumentParserService::validate_convert_task(
    const pipestream::parse::v1::ConvertDocumentRequest& request, const std::string& surface) {
  if (request.sources_size() != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one source");
  }
  return validate_options(request.options(), surface);
}

template <typename ChunkRequest>
grpc::Status DocumentParserService::validate_chunk_task(const ChunkRequest& request,
                                                        const std::string& surface) {
  // In the order the synchronous chunk RPC checks, so both refuse a request
  // with the same status.
  if (grpc::Status target = refuse_target(request.target(), surface); !target.ok()) return target;
  if constexpr (std::is_same_v<ChunkRequest, pipestream::parse::v1::HybridChunkRequest>) {
    if (grpc::Status budget = chunking::validate_hybrid_options(request.chunking_options());
        !budget.ok()) {
      return budget;
    }
  }
  if (grpc::Status embedding = embedder_.validate(request.embedding_options()); !embedding.ok()) {
    return embedding;
  }
  if (request.sources_size() != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one source");
  }
  return validate_options(request.convert_options(), surface);
}

std::expected<pipestream::parse::v1::TaskStatusPollResponse, grpc::Status>
DocumentParserService::submit_task(grpc::CallbackServerContext* context,
                                   pipestream::parse::v1::TaskType kind, uint64_t charge,
                                   TaskWork work) {
  // The submitting call ends before the parse starts, so what the parse
  // would read off it is read now.
  DocumentPasswords passwords;
  if (grpc::Status read = read_document_passwords(*context, &passwords); !read.ok()) {
    return std::unexpected(read);
  }
  auto held = InflightCharge::acquire(inflight_, charge);
  if (!held.has_value()) {
    return std::unexpected(
        grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                     "in-flight document bytes would exceed GRPARSE_MAX_INFLIGHT_BYTES"));
  }
  auto created = tasks_->create(kind);
  if (!created.has_value()) return std::unexpected(created.error());
  const std::string task_id = created->task_id();
  // Shared because std::function must stay copyable; the task going away
  // returns the request's charge.
  auto request_charge = std::make_shared<InflightCharge>(std::move(*held));
  const bool queued = executor_.submit(
      [this, task_id, work = std::move(work), request_charge,
       call = ParseCall(tasks_->stop_flag(), std::move(passwords))] {
        tasks_->start(task_id);
        try {
          work(call, task_id);
        } catch (...) {
          tasks_->fail(task_id, status_from_exception(std::current_exception()),
                       pipestream::parse::v1::FAILURE_PHASE_EXECUTION);
        }
      });
  if (!queued) {
    tasks_->erase(task_id);
    return std::unexpected(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                        "conversion executor is saturated"));
  }
  return *created;
}

DocumentParserService::TaskWork DocumentParserService::convert_task(
    const pipestream::parse::v1::ConvertDocumentRequest& request, const std::string& surface) {
  return [this, request = std::make_shared<const pipestream::parse::v1::ConvertDocumentRequest>(
                    request),
          surface](const ParseCall& call, const std::string& task_id) {
    const auto started = std::chrono::steady_clock::now();
    SourceParse parsed;
    parsed.doctype = request_doctype(request->sources());
    bool partial_success = false;
    pipestream::parse::v1::ConvertDocumentResponse converted;
    const grpc::Status status =
        convert_source(call, *request, surface, &parsed, &converted, &partial_success);
    record_rpc_completion(parsed, status, partial_success, started);
    if (status.ok()) {
      tasks_->succeed(task_id, std::move(converted));
    } else {
      tasks_->fail(task_id, status, task_failure_phase(parsed));
    }
  };
}

DocumentParserService::TaskWork DocumentParserService::hierarchical_task(
    const pipestream::parse::v1::HierarchicalChunkRequest& request, const std::string& surface) {
  return [this, request = std::make_shared<const pipestream::parse::v1::HierarchicalChunkRequest>(
                    request),
          surface](const ParseCall& call, const std::string& task_id) {
    const auto started = std::chrono::steady_clock::now();
    SourceParse parsed;
    parsed.doctype = request_doctype(request->sources());
    std::vector<pipestream::parse::v1::Chunk> chunks;
    pipestream::parse::v1::ChunkDocumentResponse chunked;
    const grpc::Status status =
        chunk_hierarchical_source(call, *request, surface, &parsed, &chunks, &chunked);
    record_rpc_completion(parsed, status, !parsed.result.failures.empty(), started);
    if (!status.ok()) {
      tasks_->fail(task_id, status, task_failure_phase(parsed));
      return;
    }
    for (auto& chunk : chunks) *chunked.add_chunks() = std::move(chunk);
    tasks_->succeed(task_id, std::move(chunked));
  };
}

DocumentParserService::TaskWork DocumentParserService::hybrid_task(
    const pipestream::parse::v1::HybridChunkRequest& request, const std::string& surface) {
  return [this, request = std::make_shared<const pipestream::parse::v1::HybridChunkRequest>(
                    request),
          surface](const ParseCall& call, const std::string& task_id) {
    const auto started = std::chrono::steady_clock::now();
    SourceParse parsed;
    parsed.doctype = request_doctype(request->sources());
    std::vector<pipestream::parse::v1::Chunk> chunks;
    pipestream::parse::v1::ChunkDocumentResponse chunked;
    const grpc::Status status =
        chunk_hybrid_source(call, *request, surface, &parsed, &chunks, &chunked);
    record_rpc_completion(parsed, status, !parsed.result.failures.empty(), started);
    if (!status.ok()) {
      tasks_->fail(task_id, status, task_failure_phase(parsed));
      return;
    }
    for (auto& chunk : chunks) *chunked.add_chunks() = std::move(chunk);
    tasks_->succeed(task_id, std::move(chunked));
  };
}

grpc::ServerUnaryReactor* DocumentParserService::ConvertSourceAsync(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ConvertSourceAsyncRequest* request,
    pipestream::parse::v1::ConvertSourceAsyncResponse* response) {
  const std::string surface = "ConvertSourceAsync";
  if (grpc::Status valid = validate_convert_task(request->request(), surface); !valid.ok()) {
    return finish_inline(context, std::move(valid));
  }
  auto submitted = submit_task(context, pipestream::parse::v1::TASK_TYPE_CONVERT,
                               request->ByteSizeLong() +
                                   decoded_source_bytes(request->request().sources()),
                               convert_task(request->request(), surface));
  if (!submitted.has_value()) return finish_inline(context, submitted.error());
  *response->mutable_response() = std::move(*submitted);
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHierarchicalSourceAsync(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHierarchicalSourceAsyncRequest* request,
    pipestream::parse::v1::ChunkHierarchicalSourceAsyncResponse* response) {
  const std::string surface = "ChunkHierarchicalSourceAsync";
  if (grpc::Status valid = validate_chunk_task(request->request(), surface); !valid.ok()) {
    return finish_inline(context, std::move(valid));
  }
  auto submitted =
      submit_task(context, pipestream::parse::v1::TASK_TYPE_CHUNK,
                  chunk_charge(*request, request->request().sources()),
                  hierarchical_task(request->request(), surface));
  if (!submitted.has_value()) return finish_inline(context, submitted.error());
  *response->mutable_response() = std::move(*submitted);
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHybridSourceAsync(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHybridSourceAsyncRequest* request,
    pipestream::parse::v1::ChunkHybridSourceAsyncResponse* response) {
  const std::string surface = "ChunkHybridSourceAsync";
  if (grpc::Status valid = validate_chunk_task(request->request(), surface); !valid.ok()) {
    return finish_inline(context, std::move(valid));
  }
  auto submitted = submit_task(context, pipestream::parse::v1::TASK_TYPE_CHUNK,
                               chunk_charge(*request, request->request().sources()),
                               hybrid_task(request->request(), surface));
  if (!submitted.has_value()) return finish_inline(context, submitted.error());
  *response->mutable_response() = std::move(*submitted);
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::PollTaskStatus(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::PollTaskStatusRequest* request,
    pipestream::parse::v1::PollTaskStatusResponse* response) {
  const auto& poll = request->request();
  const double wait_seconds = poll.wait_time();
  if (!std::isfinite(wait_seconds) || wait_seconds < 0) {
    return finish_inline(context,
                         grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                      "PollTaskStatus wait_time must be a non-negative number of "
                                      "seconds"));
  }
  if (wait_seconds > 0) {
    // At most a day: the call's own deadline ends a shorter wait anyway.
    const auto wait = std::chrono::milliseconds(
        static_cast<int64_t>(std::min(wait_seconds, 86400.0) * 1000.0));
    return new TaskPollReactor(*tasks_, poll.task_id(), wait, response);
  }
  const auto status = tasks_->status(poll.task_id());
  if (!status.has_value()) {
    return finish_inline(context, grpc::Status(grpc::StatusCode::NOT_FOUND,
                                               "no task " + poll.task_id() +
                                                   " (unknown, expired, or cleared; task ids "
                                                   "are per process and do not survive a "
                                                   "restart)"));
  }
  *response->mutable_response() = *status;
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::GetConvertResult(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::GetConvertResultRequest* request,
    pipestream::parse::v1::GetConvertResultResponse* response) {
  auto result = tasks_->convert_result(request->request().task_id());
  if (!result.has_value()) return finish_inline(context, result.error());
  // Checked on the stored result before it is copied into the response.
  if (grpc::Status fits = refuse_oversized_response(**result, response_byte_cap_,
                                                    "GetConvertResult");
      !fits.ok()) {
    return finish_inline(context, std::move(fits));
  }
  *response->mutable_response() = **result;
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::GetChunkResult(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::GetChunkResultRequest* request,
    pipestream::parse::v1::GetChunkResultResponse* response) {
  auto result = tasks_->chunk_result(request->request().task_id());
  if (!result.has_value()) return finish_inline(context, result.error());
  if (grpc::Status fits =
          refuse_oversized_response(**result, response_byte_cap_, "GetChunkResult");
      !fits.ok()) {
    return finish_inline(context, std::move(fits));
  }
  *response->mutable_response() = **result;
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::ClearConverters(
    grpc::CallbackServerContext* context, const pipestream::parse::v1::ClearConvertersRequest*,
    pipestream::parse::v1::ClearConvertersResponse* response) {
  response->mutable_response()->set_status("ok");
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::ClearResults(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ClearResultsRequest* request,
    pipestream::parse::v1::ClearResultsResponse* response) {
  const double older_than = request->has_older_than() ? request->older_than() : 3600.0;
  if (!std::isfinite(older_than) || older_than < 0) {
    return finish_inline(context,
                         grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                      "ClearResults older_than must be a non-negative number of "
                                      "seconds"));
  }
  tasks_->clear(std::chrono::milliseconds(
      static_cast<int64_t>(std::min(older_than, 1e12) * 1000.0)));
  response->mutable_response()->set_status("ok");
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerWriteReactor<pipestream::parse::v1::WatchConvertSourceResponse>*
DocumentParserService::WatchConvertSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::WatchConvertSourceRequest* request) {
  using Reactor = TaskWatchReactor<pipestream::parse::v1::WatchConvertSourceResponse>;
  const std::string surface = "WatchConvertSource";
  if (grpc::Status valid = validate_convert_task(request->request(), surface); !valid.ok()) {
    return new Reactor(std::move(valid));
  }
  auto submitted = submit_task(context, pipestream::parse::v1::TASK_TYPE_CONVERT,
                               request->ByteSizeLong() +
                                   decoded_source_bytes(request->request().sources()),
                               convert_task(request->request(), surface));
  if (!submitted.has_value()) return new Reactor(submitted.error());
  return new Reactor(*tasks_, submitted->task_id());
}

grpc::ServerWriteReactor<pipestream::parse::v1::WatchChunkHierarchicalSourceResponse>*
DocumentParserService::WatchChunkHierarchicalSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::WatchChunkHierarchicalSourceRequest* request) {
  using Reactor = TaskWatchReactor<pipestream::parse::v1::WatchChunkHierarchicalSourceResponse>;
  const std::string surface = "WatchChunkHierarchicalSource";
  if (grpc::Status valid = validate_chunk_task(request->request(), surface); !valid.ok()) {
    return new Reactor(std::move(valid));
  }
  auto submitted = submit_task(context, pipestream::parse::v1::TASK_TYPE_CHUNK,
                               chunk_charge(*request, request->request().sources()),
                               hierarchical_task(request->request(), surface));
  if (!submitted.has_value()) return new Reactor(submitted.error());
  return new Reactor(*tasks_, submitted->task_id());
}

grpc::ServerWriteReactor<pipestream::parse::v1::WatchChunkHybridSourceResponse>*
DocumentParserService::WatchChunkHybridSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::WatchChunkHybridSourceRequest* request) {
  using Reactor = TaskWatchReactor<pipestream::parse::v1::WatchChunkHybridSourceResponse>;
  const std::string surface = "WatchChunkHybridSource";
  if (grpc::Status valid = validate_chunk_task(request->request(), surface); !valid.ok()) {
    return new Reactor(std::move(valid));
  }
  auto submitted = submit_task(context, pipestream::parse::v1::TASK_TYPE_CHUNK,
                               chunk_charge(*request, request->request().sources()),
                               hybrid_task(request->request(), surface));
  if (!submitted.has_value()) return new Reactor(submitted.error());
  return new Reactor(*tasks_, submitted->task_id());
}

grpc::Status refuse_oversized_response(const google::protobuf::MessageLite& response,
                                       uint64_t cap, const std::string& surface) {
  const uint64_t bytes = response.ByteSizeLong();
  if (bytes <= cap) return grpc::Status::OK;
  return grpc::Status(
      grpc::StatusCode::RESOURCE_EXHAUSTED,
      surface + ": the response is " + std::to_string(bytes) + " bytes, over the " +
          std::to_string(cap) +
          " byte unary response limit (GRPARSE_MAX_RESPONSE_BYTES); convert this document "
          "over StreamProcessDocument, which delivers the document page by page and names "
          "on its complete event the steps it does not run (DocumentComplete.omitted_steps), "
          "or request fewer to_formats");
}

grpc::Status DocumentParserService::convert_source(
    const ParseCall& call, const pipestream::parse::v1::ConvertDocumentRequest& request,
    const std::string& surface, SourceParse* parsed,
    pipestream::parse::v1::ConvertDocumentResponse* converted, bool* partial_success) {
  const auto started = std::chrono::steady_clock::now();
  const grpc::Status parse_status =
      parse_source(call, request, scheduler_, endpoints_, repair_,
                   SourceAdmission{inflight_, fetch_slots_}, surface, parsed);
  if (!parse_status.ok()) return parse_status;
  auto& result = parsed->result;
  *partial_success = !result.failures.empty();
  auto* document_response = converted->mutable_document();
  document_response->set_filename(parsed->filename.string());
  auto* document = document_response->mutable_doc();
  *document = std::move(result.document);
  report_failures(result.failures, converted->mutable_errors());
  // The same function fills StreamDocument's source_outputs, so the two
  // surfaces answer a request with the same conversion.
  if (grpc::Status outputs =
          fill_conversion_outputs(request.options(), *document, parsed,
                                  document_response->mutable_exports(), converted);
      !outputs.ok()) {
    return outputs;
  }
  // The target delivers the same conversion somewhere else; the response
  // body above keeps everything it already carries either way. It runs
  // here on the conversion's own worker because it compresses and uploads,
  // which is not work a gRPC event thread may be handed.
  const auto& target = request.target();
  bool delivery_failed = false;
  if (targets::needs_delivery(target)) {
    const grpc::Status delivered =
        targets::deliver(target, *document, document_response->exports(),
                         converted->mutable_target_result());
    if (!delivered.ok()) {
      // Delivery is additive, never a replacement: the conversion the
      // response already carries survives a store that would not take it.
      // The failure lands as an error item and a partial status, except a
      // misconfigured target itself, which the caller must fix and gets
      // told about at the RPC layer.
      if (delivered.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
          delivered.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
        return delivered;
      }
      delivery_failed = true;
      *partial_success = true;
      auto* error = converted->add_errors();
      error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
      error->set_module_name("target-delivery");
      error->set_error_message(delivered.error_message());
      error->set_category(pipestream::parse::v1::FAILURE_CATEGORY_TARGET_UNAVAILABLE);
    }
  }
  converted->set_status(result.failures.empty() && !delivery_failed
                            ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                            : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  converted->set_processing_time(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  (*converted->mutable_timings())["total"] = converted->processing_time();
  // Full ProfilingItem sample matching serve's additive profiling map; totals
  // stay on timings for older clients.
  auto& profile = (*converted->mutable_profiling())["total"];
  profile.set_scope(pipestream::parse::v1::PROFILING_SCOPE_DOCUMENT);
  profile.set_count(1);
  profile.add_times(converted->processing_time());
  return grpc::Status::OK;
}

grpc::ServerUnaryReactor* DocumentParserService::ConvertSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ConvertSourceRequest* request,
    pipestream::parse::v1::ConvertSourceResponse* response) {
  // The wire message and the copy its base64 source decodes to; a remote
  // source's fetched copy is charged after the fetch (parse_source).
  const uint64_t charge =
      request->ByteSizeLong() + decoded_source_bytes(request->request().sources());
  const std::string_view initial_doctype = request_doctype(request->request().sources());
  return new ParseUnaryReactor(
      context, executor_, inflight_, charge, initial_doctype,
      [this, context, request, response, initial_doctype] {
        const auto started = std::chrono::steady_clock::now();
        SourceParse parsed;
        parsed.doctype = initial_doctype;
        bool partial_success = false;
        grpc::Status status =
            convert_source(ParseCall(context), request->request(), "ConvertSource", &parsed,
                           response->mutable_response(), &partial_success);
        // Last, once the response is whole: a response the wire will not carry
        // is refused here, with a status that says what to do instead, rather
        // than failing in the transport after the call has done all its work.
        if (status.ok()) {
          status = refuse_oversized_response(*response, response_byte_cap_, "ConvertSource");
          if (!status.ok()) response->Clear();
        }
        record_rpc_completion(parsed, status, partial_success, started);
        return status;
      });
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHierarchicalSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
    pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) {
  const uint64_t charge = chunk_charge(*request, request->request().sources());
  const std::string_view initial_doctype = request_doctype(request->request().sources());
  return new ParseUnaryReactor(
      context, executor_, inflight_, charge, initial_doctype,
      [this, context, request, response, initial_doctype] {
        const auto started = std::chrono::steady_clock::now();
        SourceParse parsed;
        parsed.doctype = initial_doctype;
        std::vector<pipestream::parse::v1::Chunk> chunks;
        auto* chunked = response->mutable_response();
        grpc::Status status =
            chunk_hierarchical_source(ParseCall(context), request->request(),
                                      "ChunkHierarchicalSource", &parsed, &chunks, chunked);
        const bool partial_success = !parsed.result.failures.empty();
        if (status.ok()) {
          for (auto& chunk : chunks) *chunked->add_chunks() = std::move(chunk);
          status = refuse_oversized_response(*response, response_byte_cap_,
                                             "ChunkHierarchicalSource");
          if (!status.ok()) response->Clear();
        }
        record_rpc_completion(parsed, status, partial_success, started);
        return status;
      });
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHybridSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHybridSourceRequest* request,
    pipestream::parse::v1::ChunkHybridSourceResponse* response) {
  const uint64_t charge = chunk_charge(*request, request->request().sources());
  const std::string_view initial_doctype = request_doctype(request->request().sources());
  return new ParseUnaryReactor(
      context, executor_, inflight_, charge, initial_doctype,
      [this, context, request, response, initial_doctype] {
        const auto started = std::chrono::steady_clock::now();
        SourceParse parsed;
        parsed.doctype = initial_doctype;
        std::vector<pipestream::parse::v1::Chunk> chunks;
        auto* chunked = response->mutable_response();
        grpc::Status status = chunk_hybrid_source(ParseCall(context), request->request(),
                                                  "ChunkHybridSource", &parsed, &chunks, chunked);
        const bool partial_success = !parsed.result.failures.empty();
        if (status.ok()) {
          for (auto& chunk : chunks) *chunked->add_chunks() = std::move(chunk);
          status = refuse_oversized_response(*response, response_byte_cap_, "ChunkHybridSource");
          if (!status.ok()) response->Clear();
        }
        record_rpc_completion(parsed, status, partial_success, started);
        return status;
      });
}

grpc::ServerWriteReactor<pipestream::parse::v1::StreamChunksResponse>*
DocumentParserService::StreamChunks(grpc::CallbackServerContext* context,
                                    const pipestream::parse::v1::StreamChunksRequest* request) {
  if (request->chunker_case() == pipestream::parse::v1::StreamChunksRequest::CHUNKER_NOT_SET) {
    return new ParseWriteReactor<pipestream::parse::v1::StreamChunksResponse>(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                     "StreamChunks needs a chunker: set hierarchical or hybrid"));
  }
  const bool hybrid =
      request->chunker_case() == pipestream::parse::v1::StreamChunksRequest::kHybrid;
  const uint64_t charge = hybrid ? chunk_charge(*request, request->hybrid().sources())
                                 : chunk_charge(*request, request->hierarchical().sources());
  const std::string_view initial_doctype =
      hybrid ? request_doctype(request->hybrid().sources())
             : request_doctype(request->hierarchical().sources());
  return new ParseWriteReactor<pipestream::parse::v1::StreamChunksResponse>(
      context, executor_, inflight_, charge, initial_doctype,
      [this, context, request, hybrid, initial_doctype](
          std::vector<pipestream::parse::v1::StreamChunksResponse>* messages,
          InflightCharge* fetched_charge) {
        const auto started = std::chrono::steady_clock::now();
        SourceParse parsed;
        parsed.doctype = initial_doctype;
        std::vector<pipestream::parse::v1::Chunk> chunks;
        pipestream::parse::v1::ChunkDocumentResponse rest;
        const grpc::Status status =
            hybrid ? chunk_hybrid_source(ParseCall(context), request->hybrid(), "StreamChunks",
                                         &parsed, &chunks, &rest)
                   : chunk_hierarchical_source(ParseCall(context), request->hierarchical(),
                                               "StreamChunks", &parsed, &chunks, &rest);
        const bool partial_success = !parsed.result.failures.empty();
        record_rpc_completion(parsed, status, partial_success, started);
        if (!status.ok()) return status;
        // The messages below outlive this work until the last write, so the
        // fetched copy's charge goes with them to the reactor.
        *fetched_charge = std::move(parsed.fetched_charge);
        messages->reserve(chunks.size() + 1);
        for (auto& chunk : chunks) {
          *messages->emplace_back().mutable_chunk() = std::move(chunk);
        }
        auto* summary = messages->emplace_back().mutable_summary();
        summary->set_chunk_count(static_cast<int32_t>(chunks.size()));
        summary->mutable_documents()->Swap(rest.mutable_documents());
        summary->set_processing_time(rest.processing_time());
        summary->mutable_chunking_info()->swap(*rest.mutable_chunking_info());
        return grpc::Status::OK;
      });
}

grpc::Status DocumentParserService::chunk_hierarchical_source(
    const ParseCall& call,
    const pipestream::parse::v1::HierarchicalChunkRequest& chunk_request,
    const std::string& surface, SourceParse* parsed,
    std::vector<pipestream::parse::v1::Chunk>* chunks,
    pipestream::parse::v1::ChunkDocumentResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  const grpc::Status target_status = refuse_target(chunk_request.target(), surface);
  if (!target_status.ok()) return target_status;
  const auto embedding_status = embedder_.validate(chunk_request.embedding_options());
  if (!embedding_status.ok()) return embedding_status;
  const grpc::Status parse_status =
      parse_chunk_source(call, chunk_request.sources(), chunk_request.convert_options(),
                         surface, parsed);
  if (!parse_status.ok()) return parse_status;
  const chunking::ChunkOptions options =
      chunking::chunk_options_from(chunk_request.chunking_options());
  *chunks = chunking::chunk_hierarchical(parsed->result.document, parsed->offsets, options,
                                         parsed->filename.string());
  const auto embedded = embedder_.embed(chunk_request.embedding_options(),
                                        [&call] { return call.cancelled(); }, chunks);
  if (!embedded.ok()) return embedded;
  describe_chunks(chunk_request, started, parsed, response);
  return grpc::Status::OK;
}

grpc::Status DocumentParserService::chunk_hybrid_source(
    const ParseCall& call,
    const pipestream::parse::v1::HybridChunkRequest& chunk_request, const std::string& surface,
    SourceParse* parsed, std::vector<pipestream::parse::v1::Chunk>* chunks,
    pipestream::parse::v1::ChunkDocumentResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  const grpc::Status target_status = refuse_target(chunk_request.target(), surface);
  if (!target_status.ok()) return target_status;
  // The budget decides every boundary, so it is validated before any work
  // starts rather than defaulted to a number nobody asked for.
  const grpc::Status option_status =
      chunking::validate_hybrid_options(chunk_request.chunking_options());
  if (!option_status.ok()) return option_status;
  const auto embedding_status = embedder_.validate(chunk_request.embedding_options());
  if (!embedding_status.ok()) return embedding_status;
  const grpc::Status parse_status =
      parse_chunk_source(call, chunk_request.sources(), chunk_request.convert_options(),
                         surface, parsed);
  if (!parse_status.ok()) return parse_status;
  const grpc::Status chunk_status =
      chunking::chunk_hybrid(parsed->result.document, parsed->offsets,
                             chunk_request.chunking_options(), parsed->filename.string(), chunks);
  if (!chunk_status.ok()) return chunk_status;
  const auto embedded = embedder_.embed(chunk_request.embedding_options(),
                                        [&call] { return call.cancelled(); }, chunks);
  if (!embedded.ok()) return embedded;
  describe_chunks(chunk_request, started, parsed, response);
  return grpc::Status::OK;
}

grpc::Status DocumentParserService::parse_chunk_source(
    const ParseCall& call,
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources,
    const pipestream::parse::v1::ConvertDocumentOptions& options, const std::string& surface,
    SourceParse* parsed) {
  pipestream::parse::v1::ConvertDocumentRequest convert;
  *convert.mutable_sources() = sources;
  *convert.mutable_options() = options;
  return parse_source(call, convert, scheduler_, endpoints_, repair_,
                      SourceAdmission{inflight_, fetch_slots_}, surface, parsed);
}

grpc::ServerUnaryReactor* DocumentParserService::Health(
    grpc::CallbackServerContext* context, const pipestream::parse::v1::HealthRequest*,
    pipestream::parse::v1::HealthResponse* response) {
  if (health_ != nullptr) {
    health_->describe(response);
  } else {
    // Nobody told this service what it was started with (set_health).
    response->set_state(pipestream::parse::v1::SERVING_STATE_NOT_SERVING);
    response->set_status(serving_status_text(pipestream::parse::v1::SERVING_STATE_NOT_SERVING));
  }
  response->set_version(std::string(kServiceVersion));
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::GetServiceInfo(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::GetServiceInfoRequest*,
    pipestream::parse::v1::GetServiceInfoResponse* response) {
  response->set_name("gRParse");
  response->set_version(std::string(kServiceVersion));
  auto* ui = response->mutable_ui();
  ui->set_title("gRParse");
  ui->set_path("/ui/grparse");
  ui->set_description("Diskless PDF/image to page-streamed protobuf with OCR and layout");
  const auto embeddings = embedder_.capabilities();
  if (embeddings.available()) *response->mutable_embeddings() = embeddings;
  return finish_inline(context, grpc::Status::OK);
}

}  // namespace grparse
