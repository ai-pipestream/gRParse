#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"

namespace fs = std::filesystem;
namespace docling = ai::docling;

// The server sniffs PDF magic when the content type is absent, so only name
// the types this tool knows; anything else streams untyped.
std::string content_type_for(const fs::path& document) {
  std::string extension = document.extension().string();
  for (char& letter : extension) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
  if (extension == ".pdf") return "application/pdf";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".tif" || extension == ".tiff") return "image/tiff";
  return "";
}

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: grparse-stream-client DOCUMENT_PATH [HOST:PORT]\n";
    return 64;
  }
  const fs::path pdf = argv[1];
  std::ifstream input(pdf, std::ios::binary);
  if (!input) {
    std::cerr << "Could not open document: " << pdf << '\n';
    return 66;
  }

  const std::string endpoint = argc == 3 ? argv[2] : "localhost:50051";
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto client = docling::serve::v1::DoclingStreamingService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(10));
  auto stream = client->StreamProcessDocument(&context);
  const std::string document_id = pdf.filename().string();
  const std::string content_type = content_type_for(pdf);
  std::string buffer(1024 * 1024, '\0');
  while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
    docling::serve::v1::DocumentChunk chunk;
    chunk.set_document_id(document_id);
    chunk.set_filename(pdf.filename().string());
    chunk.set_content_type(content_type);
    chunk.set_data(buffer.data(), input.gcount());
    if (!stream->Write(chunk)) break;
  }
  docling::serve::v1::DocumentChunk final_chunk;
  final_chunk.set_document_id(document_id);
  final_chunk.set_filename(pdf.filename().string());
  final_chunk.set_content_type(content_type);
  final_chunk.set_complete(true);
  stream->Write(final_chunk);
  stream->WritesDone();

  int page_events = 0;
  docling::serve::v1::DocumentStreamEvent event;
  while (stream->Read(&event)) {
    if (event.has_page()) {
      ++page_events;
      int digital_items = 0;
      int ocr_items = 0;
      for (const auto& offset : event.page().text_offsets()) {
        if (offset.source() == docling::serve::v1::TEXT_SOURCE_DIGITAL_PDF) ++digital_items;
        if (offset.source() == docling::serve::v1::TEXT_SOURCE_OCR) ++ocr_items;
      }
      int labelled_items = 0;
      for (const auto& text : event.page().texts()) {
        if (text.text().base().label() != docling::core::v1::DOC_ITEM_LABEL_TEXT) ++labelled_items;
      }
      std::cout << "page=" << event.page().page_number() << " text_items=" << event.page().texts_size()
                << " digital_items=" << digital_items << " ocr_items=" << ocr_items
                << " labelled=" << labelled_items << " tables=" << event.page().tables_size()
                << " pictures=" << event.page().pictures_size() << '\n';
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
