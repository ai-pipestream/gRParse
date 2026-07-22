#pragma once

#include "ai/docling/serve/v1/docling_serve.grpc.pb.h"
#include "ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"
#include "grparse/page_scheduler.h"

namespace grparse {

class DocumentParserService final : public ai::docling::serve::v1::DoclingServeService::Service {
 public:
  explicit DocumentParserService(PageScheduler& scheduler);

  grpc::Status ConvertSource(
      grpc::ServerContext* context,
      const ai::docling::serve::v1::ConvertSourceRequest* request,
      ai::docling::serve::v1::ConvertSourceResponse* response) override;
  grpc::Status Health(grpc::ServerContext* context,
                      const ai::docling::serve::v1::HealthRequest* request,
                      ai::docling::serve::v1::HealthResponse* response) override;

 private:
  PageScheduler& scheduler_;
};

class DocumentStreamingService final : public ai::docling::serve::v1::DoclingStreamingService::CallbackService {
 public:
  explicit DocumentStreamingService(PageScheduler& scheduler);

  grpc::ServerBidiReactor<ai::docling::serve::v1::DocumentChunk,
                          ai::docling::serve::v1::DocumentStreamEvent>*
  StreamProcessDocument(grpc::CallbackServerContext* context) override;

 private:
  PageScheduler& scheduler_;
};

}  // namespace grparse
