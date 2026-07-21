#include <cstdlib>
#include <iostream>

#include <grpcpp/grpcpp.h>

#include "grparse/document_parser_service.h"

int main() {
  const char* models = std::getenv("GRPARSE_MODELS_DIR");
  const char* address = std::getenv("GRPARSE_LISTEN_ADDRESS");
  const std::string listen_address = address == nullptr ? "0.0.0.0:50051" : address;
  try {
    grparse::OcrEngine engine(models == nullptr ? "/models" : models);
    grparse::DocumentParserService service(engine);
    grparse::DocumentStreamingService streaming_service(engine);
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
