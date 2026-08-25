#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
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
};

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

  // The CV engines the office collector runs over LibreOffice page renders;
  // an all-null enrichment disables the hybrid leg.
  const OfficeCvEnrichment& cv_enrichment() const { return cv_enrichment_; }

 private:
  CollectorTargets targets_;
  OfficeCvEnrichment cv_enrichment_;
  std::mutex mutex_;
  std::map<ai::pipestream::parse::v1::Collector, std::shared_ptr<grpc::Channel>>
      channels_;
};

class DocumentParserService final : public ai::pipestream::parse::v1::ParseService::Service {
 public:
  DocumentParserService(PageScheduler& scheduler,
                        std::shared_ptr<CollectorEndpoints> endpoints);

  grpc::Status ConvertSource(
      grpc::ServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceRequest* request,
      ai::pipestream::parse::v1::ConvertSourceResponse* response) override;
  // The two synchronous chunkers parse the source exactly the way
  // ConvertSource does and chunk the document that comes out of it. Their
  // async and watch variants stay unimplemented.
  grpc::Status ChunkHierarchicalSource(
      grpc::ServerContext* context,
      const ai::pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) override;
  grpc::Status ChunkHybridSource(
      grpc::ServerContext* context,
      const ai::pipestream::parse::v1::ChunkHybridSourceRequest* request,
      ai::pipestream::parse::v1::ChunkHybridSourceResponse* response) override;
  grpc::Status Health(grpc::ServerContext* context,
                      const ai::pipestream::parse::v1::HealthRequest* request,
                      ai::pipestream::parse::v1::HealthResponse* response) override;
  grpc::Status GetServiceInfo(grpc::ServerContext* context,
                              const ai::pipestream::parse::v1::GetServiceInfoRequest* request,
                              ai::pipestream::parse::v1::GetServiceInfoResponse* response) override;

 private:
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
};

class DocumentStreamingService final : public ai::pipestream::parse::v1::ParseStreamingService::CallbackService {
 public:
  DocumentStreamingService(PageScheduler& scheduler,
                           std::shared_ptr<CollectorEndpoints> endpoints);

  grpc::ServerBidiReactor<ai::pipestream::parse::v1::DocumentChunk,
                          ai::pipestream::parse::v1::DocumentStreamEvent>*
  StreamProcessDocument(grpc::CallbackServerContext* context) override;

 private:
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
};

}  // namespace grparse
