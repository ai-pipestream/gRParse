#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "grparse/document_parser_service.h"

namespace {

size_t page_worker_count() {
  const char* configured = std::getenv("GRPARSE_PAGE_WORKERS");
  if (configured != nullptr) {
    const long parsed = std::strtol(configured, nullptr, 10);
    if (parsed > 0) return static_cast<size_t>(parsed);
  }
  const unsigned int hardware = std::thread::hardware_concurrency();
  return std::min<size_t>(2, hardware == 0 ? 1 : hardware);
}

}  // namespace

int main() {
  const char* models = std::getenv("GRPARSE_MODELS_DIR");
  const char* address = std::getenv("GRPARSE_LISTEN_ADDRESS");
  const std::string listen_address = address == nullptr ? "0.0.0.0:50051" : address;
  try {
    grparse::OcrEnginePool engines(models == nullptr ? "/models" : models, page_worker_count());
    grparse::DocumentParserService service(engines);
    grparse::DocumentStreamingService streaming_service(engines);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(50 * 1024 * 1024);
    builder.RegisterService(&service);
    builder.RegisterService(&streaming_service);
    const auto server = builder.BuildAndStart();
    if (!server) {
      std::cerr << "Unable to listen on " << listen_address << '\n';
      return 1;
    }
    std::cout << "DoclingServeService listening on " << listen_address
              << " with the RapidOCR CUDA execution provider\n";
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << "Startup failed: " << error.what() << '\n';
    return 1;
  }
}
