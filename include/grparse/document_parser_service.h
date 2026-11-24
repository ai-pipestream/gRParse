#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/message_lite.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/call_executor.h"
#include "grparse/chart_derender.h"
#include "grparse/chart_extraction_policy.h"
#include "grparse/ocr_preset_policy.h"
#include "grparse/chunk_embeddings.h"
#include "grparse/document_repair.h"
#include "grparse/inflight_bytes.h"
#include "grparse/lighton_ocr.h"
#include "grparse/office_cv_enrichment.h"
#include "grparse/page_scheduler.h"
#include "grparse/remote_fetch_slots.h"
#include "grparse/vlm_convert.h"

namespace grparse {

class ParseCall;
class ServiceHealth;
struct SourceParse;
class TaskRegistry;
struct TaskRegistryOptions;

// GRPARSE_COLLECTOR_CHANNELS when unset: two HTTP/2 connections per
// collector, so a peer's SETTINGS_MAX_CONCURRENT_STREAMS (grpc-java's
// Netty default is 100) is not the coordinator's ceiling on that peer.
inline constexpr size_t kDefaultCollectorChannels = 2;
inline constexpr size_t kMaxCollectorChannels = 64;

// The client keepalive (GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS and
// GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS) when unset, and the range startup
// accepts. 330 s is above the default ping policy of every server in the
// fleet: grpc-core (GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
// 5 min, two strikes) and grpc-java (permitKeepAliveTime, 5 min) both send
// GOAWAY too_many_pings to a client that pings more often during a call on
// which the server writes nothing, which would kill a long libreoffice, asr
// or VLM leg. A shorter time is safe only against peers that permit it.
inline constexpr int kDefaultCollectorKeepaliveTimeMs = 330000;
inline constexpr int kMinCollectorKeepaliveTimeMs = 10000;
inline constexpr int kMaxCollectorKeepaliveTimeMs = 7200000;
inline constexpr int kDefaultCollectorKeepaliveTimeoutMs = 20000;
inline constexpr int kMinCollectorKeepaliveTimeoutMs = 1000;
inline constexpr int kMaxCollectorKeepaliveTimeoutMs = 600000;

// How every collector and backend channel is dialed: the pool size
// (GRPARSE_COLLECTOR_CHANNELS) and the keepalive timings
// (GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS, GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS).
// Tests set short timings here directly.
struct CollectorChannelOptions {
  // Channels per collector target, each its own HTTP/2 connection, handed
  // out round-robin.
  size_t channels = kDefaultCollectorChannels;
  // A ping after this much silence on a connection with calls in flight;
  // no ack within keepalive_timeout closes the connection and fails its
  // calls UNAVAILABLE, so a restarted collector or a dead NAT path ends a
  // leg then instead of at its deadline.
  std::chrono::milliseconds keepalive_time{kDefaultCollectorKeepaliveTimeMs};
  std::chrono::milliseconds keepalive_timeout{kDefaultCollectorKeepaliveTimeoutMs};
};

// The dial targets of the out-of-process collectors, one per wire-capable
// Collector value. An empty target means that collector is not configured;
// selecting it fails that collector, not the parse.
struct CollectorTargets {
  std::string libreoffice;
  std::string asr;
  // The whisper model grpc-asr must transcribe with; the asr wire requires
  // one and this coordinator never guesses.
  std::string asr_model;
  std::string email;
  std::string xml;
  std::string ebcdic;
  std::string epub;
  std::string markup;
  std::string lol_html;
  std::string fastwarc;
  std::string pdf;
  // The secondary office leg: grpc-calamine over workbooks, appended to a
  // routed office plan when configured (libreoffice stays the routed
  // default).
  std::string calamine;
  // The chart derender leg (grpc-enrich, GRPARSE_ENRICH_TARGET): not a
  // collector but a peer dialed after the merge; an empty target means the
  // leg does not exist in this deployment.
  ChartDerenderOptions derender;
  // The server-side chart-extraction policy (GRPARSE_*CHART_EXTRACTION*):
  // which presets and engines a Convert request may use and whether it may
  // carry its own chart_extraction_custom_config. Checked when the options
  // are validated, whether or not the enrich leg is configured.
  ChartExtractionPolicy chart_policy = default_chart_extraction_policy();
  // The server-side OCR preset policy (GRPARSE_*OCR_PRESET*, *OCR_KIND*):
  // which ocr_preset names a Convert request may use, custom presets
  // included (docling-serve #723).
  OcrPresetPolicy ocr_policy = default_ocr_preset_policy();
  // The VLM convert leg (grpc-vlm-convert, GRPARSE_VLM_CONVERT_TARGET): the
  // PROCESSING_PIPELINE_VLM body producer. Empty target means that pipeline
  // is unavailable.
  VlmConvertOptions vlm;
  // The LightOnOCR leg (grpc-lighton-ocr, GRPARSE_LIGHTON_TARGET): the
  // PROCESSING_PIPELINE_VLM body producer for a request that names the
  // LightOnOCR model. Empty target leaves that model on grpc-vlm-convert's
  // preset.
  LightonOcrOptions lighton;
  // GRPARSE_ENABLE_REMOTE_SERVICES: whether a request may name its own
  // remote model endpoint (picture_description_api.url, an http(s)
  // vlm_pipeline_model_api.url), which a peer then calls on the caller's
  // behalf. Off by default, as docling-serve's
  // DOCLING_SERVE_ENABLE_REMOTE_SERVICES is: an open endpoint field lets any
  // caller point a peer at an internal address. The operator's own
  // endpoints (GRPARSE_ENRICH_VLM_ENDPOINT, GRPARSE_VLM_CONVERT_ENDPOINT)
  // are not requests and are unaffected.
  bool enable_remote_services = false;
  // The collector pool size and the keepalive every collector, enrich, vlm
  // and PDF backend channel is created with (collector_channel_arguments).
  CollectorChannelOptions channel_options;
};

// The largest message this server accepts on its own port and the largest
// answer it accepts from a collector: one number, so a document the server
// let in can come back from a collector. gRPC's default of 4 MB refused a
// 15 MB HTML page's markup answer with RESOURCE_EXHAUSTED.
inline constexpr int kMaxMessageBytes = 520 * 1024 * 1024;

// The unary surfaces' answer to a response that would not fit on the wire.
// A unary call returns the whole conversion in one message (the Document,
// every requested export, the chunks), and nothing capped that: a 20 MB CSV
// came back as a 770 MB response that no client could receive, after the
// server had done all the work. `cap` is the largest serialized response the
// server sends (GRPARSE_MAX_RESPONSE_BYTES, default kMaxMessageBytes, the
// size it accepts itself); a larger one is refused with RESOURCE_EXHAUSTED,
// naming both sizes and the streaming surface that carries the same
// conversion as a sequence of messages. OK when the response fits.
grpc::Status refuse_oversized_response(const google::protobuf::MessageLite& response,
                                       uint64_t cap, const std::string& surface);

// The channel arguments every collector, enrich, vlm and PDF backend
// channel is created with: the server's message limit in both directions,
// keepalive pings while calls are in flight (none on an idle connection,
// and no cap on pings between data frames, since a long leg may go minutes
// without a byte), and gRPC's own retries off (the legs retry by policy).
// `channel_index` is set as the "grparse.channel_index" argument, so the
// channels of one pool differ in their arguments and grpc-core gives each
// its own subchannel, hence its own connection, instead of sharing one.
inline constexpr const char* kChannelIndexArgument = "grparse.channel_index";
// grpc-core's own ping timeout ("grpc.http2.ping_timeout_ms", an internal
// argument name with no public macro), set to the keepalive timeout on every
// channel and on the server. grpc-core 1.84 arms its keepalive watchdog only
// when one is already armed (writing.cc, grpc_chttp2_end_write), so the
// first unanswered keepalive ping is caught by this timeout alone, whose
// default is one minute.
inline constexpr const char* kPingTimeoutArgument = "grpc.http2.ping_timeout_ms";
grpc::ChannelArguments collector_channel_arguments(const CollectorChannelOptions& options = {},
                                                   size_t channel_index = 0);

// The keepalive policy this server's own port enforces on its clients, set
// by start_server: it pings idle clients itself and lets a client ping as
// often as every 20 s, calls or not, without being sent GOAWAY
// too_many_pings.
inline constexpr int kServerKeepaliveTimeMs = 30000;
inline constexpr int kServerKeepaliveTimeoutMs = 10000;
inline constexpr int kServerMinRecvPingIntervalMs = 20000;

// Shared handle to the out-of-process collectors the coordinator can fan
// out to: the targets plus a lazily created pool of channels per collector
// (CollectorChannelOptions::channels, each its own connection), shared by
// every parse and handed out round-robin.
class CollectorEndpoints {
 public:
  explicit CollectorEndpoints(CollectorTargets targets,
                              OfficeCvEnrichment cv_enrichment = {})
      : targets_(std::move(targets)), cv_enrichment_(cv_enrichment) {}

  // True when `id` names a remote collector with a configured target.
  bool has(ai::pipestream::parse::v1::Collector id) const {
    return !target(id).empty();
  }
  // The configured dial target for `id`; empty when unconfigured or when
  // `id` is not a remote collector.
  const std::string& target(ai::pipestream::parse::v1::Collector id) const;
  // The next channel of `id`'s pool, round-robin; the pool is created on
  // first use and serves every parse. Null when the target is unconfigured.
  std::shared_ptr<grpc::Channel> channel(ai::pipestream::parse::v1::Collector id);
  // How many channels each pool holds (GRPARSE_COLLECTOR_CHANNELS).
  size_t channels_per_collector() const { return targets_.channel_options.channels; }

  const std::string& asr_model() const { return targets_.asr_model; }

  // The chart derender leg's options and its lazily created channel (one,
  // with the collector channels' arguments; enrich is not a collector); null
  // when GRPARSE_ENRICH_TARGET is unset.
  const ChartDerenderOptions& derender() const { return targets_.derender; }
  bool has_derender() const { return targets_.derender.enabled(); }
  std::shared_ptr<grpc::Channel> enrich_channel();
  const ChartExtractionPolicy& chart_policy() const { return targets_.chart_policy; }
  const OcrPresetPolicy& ocr_policy() const { return targets_.ocr_policy; }

  // The VLM convert leg's options and its lazily created channel (one, with
  // the collector channels' arguments); null when GRPARSE_VLM_CONVERT_TARGET
  // is unset.
  const VlmConvertOptions& vlm() const { return targets_.vlm; }
  bool has_vlm() const { return targets_.vlm.enabled(); }
  std::shared_ptr<grpc::Channel> vlm_channel();

  // The LightOnOCR leg's options and its lazily created channel (one, with
  // the collector channels' arguments); null when GRPARSE_LIGHTON_TARGET is
  // unset.
  const LightonOcrOptions& lighton() const { return targets_.lighton; }
  bool has_lighton() const { return targets_.lighton.enabled(); }
  std::shared_ptr<grpc::Channel> lighton_channel();

  // Whether requests may name their own remote model endpoints
  // (GRPARSE_ENABLE_REMOTE_SERVICES).
  bool remote_services_enabled() const { return targets_.enable_remote_services; }

  // The CV engines the office collector runs over LibreOffice page renders;
  // an all-null enrichment disables the hybrid leg.
  const OfficeCvEnrichment& cv_enrichment() const { return cv_enrichment_; }

  // What Health reads: the connectivity state of every channel that already
  // exists to a target, in pool order, and nothing while no call has needed
  // that target yet. Each state is read with GetState(false), so asking
  // never creates a channel and never makes one connect. COLLECTOR_VLM names
  // the vlm convert leg's channel.
  std::vector<grpc_connectivity_state> channel_states(
      ai::pipestream::parse::v1::Collector id) const;
  std::vector<grpc_connectivity_state> enrich_channel_states() const;
  std::vector<grpc_connectivity_state> lighton_channel_states() const;

 private:
  // One target's channels and the index the next call is handed.
  struct ChannelPool {
    std::vector<std::shared_ptr<grpc::Channel>> channels;
    size_t next = 0;
  };
  // The next channel of `pool`, creating the whole pool of `size` channels
  // against `target` on first use. Called with mutex_ held.
  std::shared_ptr<grpc::Channel> next_channel(ChannelPool& pool, const std::string& target,
                                              size_t size);
  // The states of `pool`'s channels. Called with mutex_ held.
  std::vector<grpc_connectivity_state> states_of(const ChannelPool& pool) const;

  CollectorTargets targets_;
  OfficeCvEnrichment cv_enrichment_;
  mutable std::mutex mutex_;
  std::map<ai::pipestream::parse::v1::Collector, ChannelPool> channels_;
  ChannelPool enrich_channel_;
  ChannelPool vlm_channel_;
  ChannelPool lighton_channel_;
};

// Every unary surface runs on gRPC's callback API. The parsing surfaces block
// for as long as the document takes, so they never run on the reaction thread:
// each hands its work to the executor and finishes the call from there, which
// is what keeps a slow parse from pinning an event-manager thread. The trivial
// surfaces (Health, GetServiceInfo) finish inline because they only read
// state the process already holds.
//
// `repair` is the post-merge repair pass every parsing surface runs on the
// merged Document before rendering (see document_repair.h); nullopt skips
// it, which is what GRPARSE_REPAIR=off selects at startup.
class DocumentParserService final
    : public ai::pipestream::parse::v1::ParseService::CallbackService {
 public:
  DocumentParserService(PageScheduler& scheduler,
                        std::shared_ptr<CollectorEndpoints> endpoints,
                        CallExecutor::Options executor_options = {},
                        std::optional<RepairOptions> repair = RepairOptions{},
                        std::shared_ptr<EmbeddingEngine> embedding_engine = {},
                        EmbeddingConfig embedding_config = {},
                        std::shared_ptr<InflightBytes> inflight = {});
  // Stops the task registry first, so queued and running async tasks end
  // CANCELLED before the executor joins them.
  ~DocumentParserService() override;

  // The async task registry's bounds (GRPARSE_MAX_TASKS,
  // GRPARSE_TASK_RESULT_TTL_SECONDS, GRPARSE_MAX_WATCHERS); the defaults
  // until set. Set before serving: it replaces the registry.
  void set_task_options(const TaskRegistryOptions& options);
  TaskRegistry& tasks() { return *tasks_; }

  // The largest serialized response a unary surface sends (see
  // refuse_oversized_response); kMaxMessageBytes until set.
  void set_response_byte_cap(uint64_t bytes) { response_byte_cap_ = bytes; }
  uint64_t response_byte_cap() const { return response_byte_cap_; }

  // The slots every remote source fetch holds while it runs
  // (GRPARSE_REMOTE_FETCH_CONCURRENCY, see RemoteFetchSlots);
  // kDefaultRemoteFetchConcurrency of them until set. Set before serving;
  // null leaves fetches unbounded.
  void set_remote_fetch_slots(std::shared_ptr<RemoteFetchSlots> slots) {
    fetch_slots_ = std::move(slots);
  }
  const std::shared_ptr<RemoteFetchSlots>& remote_fetch_slots() const { return fetch_slots_; }

  // What the Health RPC reports (see service_health.h): the engines startup
  // built, the targets it was given, and whether startup has finished. Set
  // before serving. Until one is set Health answers
  // SERVING_STATE_NOT_SERVING with no components: nothing has told this
  // service what it was started with, so it claims nothing.
  void set_health(std::shared_ptr<const ServiceHealth> health) { health_ = std::move(health); }

  grpc::ServerUnaryReactor* ConvertSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceRequest* request,
      ai::pipestream::parse::v1::ConvertSourceResponse* response) override;
  // The two synchronous chunkers parse the source exactly the way
  // ConvertSource does and chunk the document that comes out of it.
  grpc::ServerUnaryReactor* ChunkHierarchicalSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) override;
  grpc::ServerUnaryReactor* ChunkHybridSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHybridSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHybridSourceResponse* response) override;
  // Either chunker's result, one chunk per message and then a summary.
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::StreamChunksResponse>* StreamChunks(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::StreamChunksRequest* request) override;
  // ConvertSource's conversion as a server stream of one message, written
  // once the conversion is done (the contract's "single response when
  // complete"); kept for existing clients, StreamDocument is the envelope
  // feed.
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::ConvertSourceStreamResponse>*
  ConvertSourceStream(grpc::CallbackServerContext* context,
                      const ai::pipestream::parse::v1::ConvertSourceStreamRequest* request) override;

  // The async task surfaces, over a bounded in-memory registry (see
  // src/task_registry.h). The *Async RPCs validate the request, register
  // a task, queue the same work their synchronous RPC runs on the executor,
  // and answer at once with the pending status; the work records its
  // transitions and result in the registry. Task ids are per process.
  grpc::ServerUnaryReactor* ConvertSourceAsync(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceAsyncRequest* request,
      ai::pipestream::parse::v1::ConvertSourceAsyncResponse* response) override;
  grpc::ServerUnaryReactor* ChunkHierarchicalSourceAsync(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHierarchicalSourceAsyncRequest* request,
      ai::pipestream::parse::v1::ChunkHierarchicalSourceAsyncResponse* response) override;
  grpc::ServerUnaryReactor* ChunkHybridSourceAsync(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHybridSourceAsyncRequest* request,
      ai::pipestream::parse::v1::ChunkHybridSourceAsyncResponse* response) override;
  // The task's status; with wait_time, held until the task finishes or that
  // many seconds pass, whichever is first, without holding a thread.
  grpc::ServerUnaryReactor* PollTaskStatus(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::PollTaskStatusRequest* request,
      ai::pipestream::parse::v1::PollTaskStatusResponse* response) override;
  grpc::ServerUnaryReactor* GetConvertResult(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::GetConvertResultRequest* request,
      ai::pipestream::parse::v1::GetConvertResultResponse* response) override;
  grpc::ServerUnaryReactor* GetChunkResult(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::GetChunkResultRequest* request,
      ai::pipestream::parse::v1::GetChunkResultResponse* response) override;
  // This server keeps no per-task converters: acknowledged.
  grpc::ServerUnaryReactor* ClearConverters(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ClearConvertersRequest* request,
      ai::pipestream::parse::v1::ClearConvertersResponse* response) override;
  // Drops finished tasks older than older_than seconds (3600 when unset).
  grpc::ServerUnaryReactor* ClearResults(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ClearResultsRequest* request,
      ai::pipestream::parse::v1::ClearResultsResponse* response) override;
  // Submit as the *Async RPC does, then stream every status the task takes,
  // from the pending one to SUCCESS or FAILURE, and close OK; the result is
  // then read with GetConvertResult or GetChunkResult by the task_id every
  // message carries.
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::WatchConvertSourceResponse>*
  WatchConvertSource(grpc::CallbackServerContext* context,
                     const ai::pipestream::parse::v1::WatchConvertSourceRequest* request) override;
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::WatchChunkHierarchicalSourceResponse>*
  WatchChunkHierarchicalSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::WatchChunkHierarchicalSourceRequest* request) override;
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::WatchChunkHybridSourceResponse>*
  WatchChunkHybridSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::WatchChunkHybridSourceRequest* request) override;

  // The state of this process as ServiceHealth describes it, read at the
  // time of the call. Finishes inline: it reads what startup recorded and
  // the connectivity state of channels that already exist, and dials
  // nothing.
  grpc::ServerUnaryReactor* Health(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::HealthRequest* request,
      ai::pipestream::parse::v1::HealthResponse* response) override;
  grpc::ServerUnaryReactor* GetServiceInfo(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::GetServiceInfoRequest* request,
      ai::pipestream::parse::v1::GetServiceInfoResponse* response) override;

 private:
  // The conversion behind ConvertSource, ConvertSourceStream and the
  // convert tasks: parse, exports, chunks, target delivery and timings into
  // `converted`. `partial_success` is set when a collector or the delivery
  // failed beside a converted document. Blocking: executor workers only.
  grpc::Status convert_source(const ParseCall& call,
                              const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                              const std::string& surface, SourceParse* parsed,
                              ai::pipestream::parse::v1::ConvertDocumentResponse* converted,
                              bool* partial_success);
  // The parse, chunking and embedding behind the unary and streamed chunk
  // surfaces. `chunks` receives the chunks; `response` receives everything
  // else a ChunkDocumentResponse carries. Blocking: executor workers only.
  grpc::Status chunk_hierarchical_source(
      const ParseCall& call,
      const ai::pipestream::parse::v1::HierarchicalChunkRequest& chunk_request,
      const std::string& surface, SourceParse* parsed,
      std::vector<ai::pipestream::parse::v1::Chunk>* chunks,
      ai::pipestream::parse::v1::ChunkDocumentResponse* response);
  grpc::Status chunk_hybrid_source(
      const ParseCall& call,
      const ai::pipestream::parse::v1::HybridChunkRequest& chunk_request,
      const std::string& surface, SourceParse* parsed,
      std::vector<ai::pipestream::parse::v1::Chunk>* chunks,
      ai::pipestream::parse::v1::ChunkDocumentResponse* response);
  grpc::Status parse_chunk_source(
      const ParseCall& call,
      const google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::Source>& sources,
      const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
      const std::string& surface, SourceParse* parsed);
  // What the *Async and Watch* RPCs check before a task is registered,
  // the checks their synchronous RPC makes before it parses.
  grpc::Status validate_convert_task(const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                                     const std::string& surface);
  template <typename ChunkRequest>
  grpc::Status validate_chunk_task(const ChunkRequest& request, const std::string& surface);
  // Registers a task of `kind` and queues `work` for it: the call's
  // passwords are read now, `charge` bytes stay charged to the in-flight
  // budget until the work is done, and the work runs under a ParseCall
  // that the registry's stop flag cancels. The pending status, or the
  // status that refused the task.
  using TaskWork = std::function<void(const ParseCall& call, const std::string& task_id)>;
  std::expected<ai::pipestream::parse::v1::TaskStatusPollResponse, grpc::Status> submit_task(
      grpc::CallbackServerContext* context, ai::pipestream::parse::v1::TaskType kind,
      uint64_t charge, TaskWork work);
  // The queued work of each task kind, run on the executor.
  TaskWork convert_task(const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                        const std::string& surface);
  TaskWork hierarchical_task(const ai::pipestream::parse::v1::HierarchicalChunkRequest& request,
                             const std::string& surface);
  TaskWork hybrid_task(const ai::pipestream::parse::v1::HybridChunkRequest& request,
                       const std::string& surface);

  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  std::optional<RepairOptions> repair_;
  ChunkEmbedder embedder_;
  // The process-wide in-flight byte budget; null admits by call count alone.
  std::shared_ptr<InflightBytes> inflight_;
  uint64_t response_byte_cap_ = static_cast<uint64_t>(kMaxMessageBytes);
  std::shared_ptr<RemoteFetchSlots> fetch_slots_ =
      std::make_shared<RemoteFetchSlots>(kDefaultRemoteFetchConcurrency);
  // Null until set_health; see there.
  std::shared_ptr<const ServiceHealth> health_;
  // Outlives the executor below: a task's worker records into it to the
  // end.
  std::unique_ptr<TaskRegistry> tasks_;
  // Declared last so it is torn down first: joining the workers before the
  // endpoints and the scheduler reference go away is what keeps an in-flight
  // parse from outliving what it reads.
  CallExecutor executor_;
};

// Optional synchronous instrumentation for the collector-to-event transfer.
// Both callbacks run on the collector thread under the reactor lock. They must
// not block or throw. The source Document remains alive through after_transfer.
struct CollectorDocumentTransferObserver {
  std::function<void()> before_transfer;
  std::function<void()> after_transfer;
};

// StreamProcessDocument's bounds (GRPARSE_MAX_ACTIVE_STREAMS,
// GRPARSE_STREAM_WORKERS, GRPARSE_STREAM_QUEUE; see read_streaming_limits).
struct StreamingLimits {
  // Calls open at once, server wide. One more finishes RESOURCE_EXHAUSTED
  // naming GRPARSE_MAX_ACTIVE_STREAMS before it reads a chunk.
  size_t max_active_streams = 64;
  // The pool every stream's plan and collector legs run on. A leg the full
  // queue refuses fails that collector with RESOURCE_EXHAUSTED, so the parse
  // degrades collector by collector as for any failed leg.
  CallExecutor::Options executor{.workers = 64, .queue_capacity = 256};
};

class DocumentStreamingService final : public ai::pipestream::parse::v1::ParseStreamingService::CallbackService {
 public:
  // The stream has no merged Document: each collector's finished Document
  // is what the repair pass runs on, before it is projected into page
  // events and emitted whole.
  // `inflight` is the budget every stream's chunks are charged to as they
  // arrive (see InflightBytes); null leaves the 500 MiB per-stream limit as
  // the only bound.
  // `executor_options` sizes the pool StreamDocument's source parses run
  // on, a pool of this service's own (the server passes the unary pool's
  // GRPARSE_UNARY_WORKERS and GRPARSE_UNARY_QUEUE).
  DocumentStreamingService(PageScheduler& scheduler,
                           std::shared_ptr<CollectorEndpoints> endpoints,
                           std::optional<RepairOptions> repair = RepairOptions{},
                           std::shared_ptr<InflightBytes> inflight = {},
                           std::shared_ptr<const CollectorDocumentTransferObserver> transfer_observer = {},
                           StreamingLimits limits = {});

  // The slots StreamDocument's remote source fetches hold, as on
  // DocumentParserService; the server hands both services the same set, so
  // GRPARSE_REMOTE_FETCH_CONCURRENCY bounds the fetches of both. Set before
  // serving; null leaves fetches unbounded.
  void set_remote_fetch_slots(std::shared_ptr<RemoteFetchSlots> slots) {
    fetch_slots_ = std::move(slots);
  }
  const std::shared_ptr<RemoteFetchSlots>& remote_fetch_slots() const { return fetch_slots_; }

  // The source-based entry to the envelope feed: each source of the
  // ConvertDocumentRequest is fetched, parsed and exported exactly as
  // ConvertSource converts it, one after another on this service's
  // executor, and the stream carries status envelopes around each source's
  // final_document, source_outputs (the exports, chunks, findings,
  // confidence and offsets of the unary response) and source_result. Every
  // envelope echoes request_id and carries the next sequence_number, from 1.
  grpc::ServerWriteReactor<ai::pipestream::parse::v1::StreamDocumentResponse>* StreamDocument(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::StreamDocumentRequest* request) override;

  grpc::ServerBidiReactor<ai::pipestream::parse::v1::DocumentChunk,
                          ai::pipestream::parse::v1::DocumentStreamEvent>*
  StreamProcessDocument(grpc::CallbackServerContext* context) override;

  // Streams admitted and not yet done. Sampled; for tests and gauges.
  size_t active_streams() const { return active_streams_->load(); }
  // Tasks waiting for a stream worker. Sampled, like CallExecutor::queued.
  size_t queued_stream_tasks() const { return executor_.queued(); }

  // Page events this service's streams folded server side to compute their
  // repair events, summed over every stream since construction. A stream
  // that set DocumentChunk.repair_delta to false adds nothing. For tests.
  uint64_t folded_page_events() const { return folded_pages_->load(); }

 private:
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  std::optional<RepairOptions> repair_;
  std::shared_ptr<InflightBytes> inflight_;
  std::shared_ptr<const CollectorDocumentTransferObserver> transfer_observer_;
  std::shared_ptr<std::atomic<uint64_t>> folded_pages_ =
      std::make_shared<std::atomic<uint64_t>>(0);
  const size_t max_active_streams_;
  std::shared_ptr<std::atomic<size_t>> active_streams_ =
      std::make_shared<std::atomic<size_t>>(0);
  std::shared_ptr<RemoteFetchSlots> fetch_slots_ =
      std::make_shared<RemoteFetchSlots>(kDefaultRemoteFetchConcurrency);
  // Declared last so it is torn down first, as on DocumentParserService:
  // StreamDocument's parses are joined before what they read goes away.
  CallExecutor executor_;
};

}  // namespace grparse
