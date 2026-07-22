#pragma once

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/page_scheduler.h"

namespace grparse {

class DocumentParserService final : public ai::pipestream::parse::v1::ParseService::Service {
 public:
  explicit DocumentParserService(PageScheduler& scheduler);

  grpc::Status ConvertSource(
      grpc::ServerContext* context,
      const ai::pipestream::parse::v1::ConvertSourceRequest* request,
      ai::pipestream::parse::v1::ConvertSourceResponse* response) override;
  grpc::Status Health(grpc::ServerContext* context,
                      const ai::pipestream::parse::v1::HealthRequest* request,
                      ai::pipestream::parse::v1::HealthResponse* response) override;

 private:
  PageScheduler& scheduler_;
};

class DocumentStreamingService final : public ai::pipestream::parse::v1::ParseStreamingService::CallbackService {
 public:
  explicit DocumentStreamingService(PageScheduler& scheduler);

  grpc::ServerBidiReactor<ai::pipestream::parse::v1::DocumentChunk,
                          ai::pipestream::parse::v1::DocumentStreamEvent>*
  StreamProcessDocument(grpc::CallbackServerContext* context) override;

 private:
  PageScheduler& scheduler_;
};

}  // namespace grparse
