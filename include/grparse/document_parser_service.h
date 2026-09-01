#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/call_executor.h"
#include "grparse/chart_derender.h"
#include "grparse/document_repair.h"
#include "grparse/office_cv_enrichment.h"
#include "grparse/page_scheduler.h"

namespace grparse {

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
  // The chart derender leg (grpc-enrich, GRPARSE_ENRICH_TARGET): not a
  // collector but a peer dialed after the merge; an empty target means the
  // leg does not exist in this deployment.
  ChartDerenderOptions derender;
};

// The largest message this server accepts on its own port and the largest
// answer it accepts from a collector: one number, so a document the server
// let in can come back from a collector. gRPC's default of 4 MB refused a
// 15 MB HTML page's markup answer with RESOURCE_EXHAUSTED.
inline constexpr int kMaxMessageBytes = 520 * 1024 * 1024;

// The channel arguments every collector channel is created with.
grpc::ChannelArguments collector_channel_arguments();

// Shared handle to the out-of-process collectors the coordinator can fan
// out to: the targets plus one lazily created channel per collector, shared
// by every parse.
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
  // The lazily created channel for `id`; one channel per collector serves
  // every parse. Null when the target is unconfigured.
  std::shared_ptr<grpc::Channel> channel(ai::pipestream::parse::v1::Collector id);

  const std::string& asr_model() const { return targets_.asr_model; }

  // The chart derender leg's options and its lazily created channel; null
  // when GRPARSE_ENRICH_TARGET is unset.
  const ChartDerenderOptions& derender() const { return targets_.derender; }
  bool has_derender() const { return targets_.derender.enabled(); }
  std::shared_ptr<grpc::Channel> enrich_channel();

  // The CV engines the office collector runs over LibreOffice page renders;
  // an all-null enrichment disables the hybrid leg.
  const OfficeCvEnrichment& cv_enrichment() const { return cv_enrichment_; }

 private:
  CollectorTargets targets_;
  OfficeCvEnrichment cv_enrichment_;
  std::mutex mutex_;
  std::map<ai::pipestream::parse::v1::Collector, std::shared_ptr<grpc::Channel>>
      channels_;
  std::shared_ptr<grpc::Channel> enrich_channel_;
};

// Every unary surface runs on gRPC's callback API. The parsing surfaces block
// for as long as the document takes, so they never run on the reaction thread:
// each hands its work to the executor and finishes the call from there, which
// is what keeps a slow parse from pinning an event-manager thread. The trivial
// surfaces (Health, GetServiceInfo) finish inline because they do no work.
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
                        std::optional<RepairOptions> repair = RepairOptions{});

  grpc::ServerUnaryReactor* ConvertSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceRequest* request,
      ai::pipestream::parse::v1::ConvertSourceResponse* response) override;
  // The two synchronous chunkers parse the source exactly the way
  // ConvertSource does and chunk the document that comes out of it. Their
  // async and watch variants stay unimplemented.
  grpc::ServerUnaryReactor* ChunkHierarchicalSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) override;
  grpc::ServerUnaryReactor* ChunkHybridSource(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::ChunkHybridSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHybridSourceResponse* response) override;
  grpc::ServerUnaryReactor* Health(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::HealthRequest* request,
      ai::pipestream::parse::v1::HealthResponse* response) override;
  grpc::ServerUnaryReactor* GetServiceInfo(
      grpc::CallbackServerContext* context,
      const ai::pipestream::parse::v1::GetServiceInfoRequest* request,
      ai::pipestream::parse::v1::GetServiceInfoResponse* response) override;

 private:
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  std::optional<RepairOptions> repair_;
  // Declared last so it is torn down first: joining the workers before the
  // endpoints and the scheduler reference go away is what keeps an in-flight
  // parse from outliving what it reads.
  CallExecutor executor_;
};

class DocumentStreamingService final : public ai::pipestream::parse::v1::ParseStreamingService::CallbackService {
 public:
  // The stream has no merged Document: each collector's finished Document
  // is what the repair pass runs on, before it is projected into page
  // events and emitted whole.
  DocumentStreamingService(PageScheduler& scheduler,
                           std::shared_ptr<CollectorEndpoints> endpoints,
                           std::optional<RepairOptions> repair = RepairOptions{});

  grpc::ServerBidiReactor<ai::pipestream::parse::v1::DocumentChunk,
                          ai::pipestream::parse::v1::DocumentStreamEvent>*
  StreamProcessDocument(grpc::CallbackServerContext* context) override;

 private:
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  std::optional<RepairOptions> repair_;
};

}  // namespace grparse
