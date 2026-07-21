#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"

namespace fs = std::filesystem;
namespace docling = ai::docling;

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: grparse-stream-client PDF_PATH [HOST:PORT]\n";
    return 64;
  }
  const fs::path pdf = argv[1];
  std::ifstream input(pdf, std::ios::binary);
  if (!input) {
    std::cerr << "Could not open PDF: " << pdf << '\n';
    return 66;
  }

  const std::string endpoint = argc == 3 ? argv[2] : "localhost:50051";
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto client = docling::serve::v1::DoclingStreamingService::NewStub(channel);
  grpc::ClientContext context;
  auto stream = client->StreamProcessDocument(&context);
  const std::string document_id = pdf.filename().string();
  std::string buffer(1024 * 1024, '\0');
  while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
    docling::serve::v1::DocumentChunk chunk;
    chunk.set_document_id(document_id);
    chunk.set_filename(pdf.filename().string());
    chunk.set_content_type("application/pdf");
    chunk.set_data(buffer.data(), input.gcount());
    if (!stream->Write(chunk)) break;
  }
  docling::serve::v1::DocumentChunk final_chunk;
  final_chunk.set_document_id(document_id);
  final_chunk.set_filename(pdf.filename().string());
  final_chunk.set_content_type("application/pdf");
  final_chunk.set_complete(true);
  stream->Write(final_chunk);
  stream->WritesDone();

  int page_events = 0;
  docling::serve::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) {
    if (event.has_page()) {
      ++page_events;
      std::cout << "page=" << event.page().page_number() << " text_items=" << event.page().texts_size() << '\n';
    } else if (event.has_complete()) {
      std::cout << "complete total_pages=" << event.total_pages() << '\n';
    }
  }
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    std::cerr << "StreamProcessDocument failed: " << status.error_message() << '\n';
    return 1;
  }
  return page_events == 0 ? 1 : 0;
}
