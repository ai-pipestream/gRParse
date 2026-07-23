#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/page_scheduler.h"

namespace grparse {

// Shared handle to the out-of-process collectors the coordinator can fan
// out to. Today that is the grpc-libreoffice collector; an empty target
// means the collector is not configured and selecting it fails that
// collector, not the parse.
class CollectorEndpoints {
 public:
  explicit CollectorEndpoints(std::string libreoffice_target)
      : libreoffice_target_(std::move(libreoffice_target)) {}

  bool has_libreoffice() const { return !libreoffice_target_.empty(); }
  const std::string& libreoffice_target() const { return libreoffice_target_; }

  // The lazily created channel to the libreoffice collector; one channel
  // serves every parse.
  std::shared_ptr<grpc::Channel> libreoffice_channel();

 private:
  std::string libreoffice_target_;
  std::mutex mutex_;
  std::shared_ptr<grpc::Channel> channel_;
};

class DocumentParserService final : public ai::pipestream::parse::v1::ParseService::Service {
 public:
  DocumentParserService(PageScheduler& scheduler,
                        std::shared_ptr<CollectorEndpoints> endpoints);

  grpc::Status ConvertSource(
      grpc::ServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceRequest* request,
      ai::pipestream::parse::v1::ConvertSourceResponse* response) override;
  grpc::Status Health(grpc::ServerContext* context,
                      const ai::pipestream::parse::v1::HealthRequest* request,
                      ai::pipestream::parse::v1::HealthResponse* response) override;

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
