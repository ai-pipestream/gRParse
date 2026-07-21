#pragma once

#include <filesystem>

#include "ai/docling/serve/v1/docling_serve.grpc.pb.h"
#include "ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"
#include "grparse/ocr_engine.h"

namespace grparse {

class DocumentParserService final : public ai::docling::serve::v1::DoclingServeService::Service {
 public:
  explicit DocumentParserService(OcrEnginePool& engines);

  grpc::Status ConvertSource(
      grpc::ServerContext* context,
      const ai::docling::serve::v1::ConvertSourceRequest* request,
      ai::docling::serve::v1::ConvertSourceResponse* response) override;
  grpc::Status Health(grpc::ServerContext* context,
                      const ai::docling::serve::v1::HealthRequest* request,
                      ai::docling::serve::v1::HealthResponse* response) override;

 private:
  OcrEnginePool& engines_;
};

class DocumentStreamingService final : public ai::docling::serve::v1::DoclingStreamingService::Service {
 public:
  explicit DocumentStreamingService(OcrEnginePool& engines);

  grpc::Status StreamProcessDocument(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<ai::docling::serve::v1::DocumentStreamEvent,
                               ai::docling::serve::v1::DocumentChunk>* stream) override;

 private:
  OcrEnginePool& engines_;
};

}  // namespace grparse
