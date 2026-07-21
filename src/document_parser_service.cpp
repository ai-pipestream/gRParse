#include "grparse/document_parser_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <google/protobuf/arena.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace docling = ai::docling;

namespace grparse {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() : path_(fs::temp_directory_path() / ("grparse-" + std::to_string(++counter_))) {
    fs::create_directories(path_);
  }
  ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path_, ignored); }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
  static std::atomic_uint64_t counter_;
};
std::atomic_uint64_t TemporaryDirectory::counter_{0};

std::string decode_base64(const std::string& value) {
  static constexpr unsigned char kInvalid = 255;
  static const auto decode = [] {
    std::array<unsigned char, 256> table{};
    table.fill(kInvalid);
    for (unsigned char index = 0; index < 26; ++index) {
      table[static_cast<unsigned char>('A' + index)] = index;
      table[static_cast<unsigned char>('a' + index)] = 26 + index;
    }
    for (unsigned char index = 0; index < 10; ++index) table[static_cast<unsigned char>('0' + index)] = 52 + index;
    table[static_cast<unsigned char>('+')] = 62;
    table[static_cast<unsigned char>('/')] = 63;
    return table;
  }();

  std::string compact;
  compact.reserve(value.size());
  for (const char character : value) {
    if (!std::isspace(static_cast<unsigned char>(character))) compact.push_back(character);
  }
  if (compact.empty() || compact.size() % 4 != 0) throw std::invalid_argument("file.base64_string is not valid base64");

  std::string decoded;
  decoded.reserve(compact.size() / 4 * 3);
  for (size_t offset = 0; offset < compact.size(); offset += 4) {
    const char first = compact[offset];
    const char second = compact[offset + 1];
    const char third = compact[offset + 2];
    const char fourth = compact[offset + 3];
    if (first == '=' || second == '=' || decode[static_cast<unsigned char>(first)] == kInvalid ||
        decode[static_cast<unsigned char>(second)] == kInvalid ||
        (third != '=' && decode[static_cast<unsigned char>(third)] == kInvalid) ||
        (fourth != '=' && decode[static_cast<unsigned char>(fourth)] == kInvalid) ||
        (third == '=' && fourth != '=') || ((third == '=' || fourth == '=') && offset + 4 != compact.size())) {
      throw std::invalid_argument("file.base64_string is not valid base64");
    }
    const auto a = decode[static_cast<unsigned char>(first)];
    const auto b = decode[static_cast<unsigned char>(second)];
    const auto c = third == '=' ? 0 : decode[static_cast<unsigned char>(third)];
    const auto d = fourth == '=' ? 0 : decode[static_cast<unsigned char>(fourth)];
    decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (third != '=') decoded.push_back(static_cast<char>((b << 4) | (c >> 2)));
    if (fourth != '=') decoded.push_back(static_cast<char>((c << 6) | d));
  }
  return decoded;
}

uint64_t content_hash(const std::string& document) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : document) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string mimetype_for(const fs::path& path) {
  const auto extension = path.extension().string();
  if (extension == ".pdf") return "application/pdf";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".tif" || extension == ".tiff") return "image/tiff";
  return "image/png";
}

bool is_pdf(const std::string& content, const fs::path& filename) {
  return filename.extension() == ".pdf" ||
         (content.size() >= 5 && content.compare(0, 5, "%PDF-") == 0);
}

void write_document(const fs::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output) throw std::runtime_error("Could not persist request document");
}

std::vector<fs::path> render_pdf(const fs::path& pdf, const fs::path& output_directory) {
  const auto prefix = output_directory / "page";
  const auto command = "pdftoppm -png -r 200 '" + pdf.string() + "' '" + prefix.string() + "'";
  if (std::system(command.c_str()) != 0) throw std::runtime_error("pdftoppm could not render the PDF");
  std::vector<fs::path> pages;
  for (const auto& entry : fs::directory_iterator(output_directory)) {
    if (entry.path().extension() == ".png") pages.push_back(entry.path());
  }
  std::sort(pages.begin(), pages.end());
  if (pages.empty()) throw std::runtime_error("PDF did not produce renderable pages");
  return pages;
}

int pdf_page_count(const fs::path& pdf, const fs::path& output_directory) {
  const auto info = output_directory / "pdfinfo.txt";
  const auto command = "pdfinfo '" + pdf.string() + "' > '" + info.string() + "'";
  if (std::system(command.c_str()) != 0) throw std::runtime_error("pdfinfo could not inspect the PDF");
  std::ifstream input(info);
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("Pages:", 0) == 0) {
      const int pages = std::stoi(line.substr(6));
      if (pages > 0) return pages;
    }
  }
  throw std::runtime_error("pdfinfo did not report a positive page count");
}

fs::path render_pdf_page(const fs::path& pdf, int page_number, const fs::path& output_directory) {
  const auto page_directory = output_directory / ("page-" + std::to_string(page_number));
  fs::create_directories(page_directory);
  const auto prefix = page_directory / "render";
  const auto command = "pdftoppm -png -r 200 -f " + std::to_string(page_number) + " -l " +
                       std::to_string(page_number) + " '" + pdf.string() + "' '" + prefix.string() + "'";
  if (std::system(command.c_str()) != 0) throw std::runtime_error("pdftoppm could not render PDF page");
  for (const auto& entry : fs::directory_iterator(page_directory)) {
    if (entry.path().extension() == ".png") return entry.path();
  }
  throw std::runtime_error("PDF page did not produce a renderable image");
}

void append_page(const OcrEngine::Page& source, int page_number, docling::core::v1::DoclingDocument* document,
                 std::string* plain_text) {
  auto& page = (*document->mutable_pages())[page_number];
  page.set_page_no(page_number);
  page.mutable_size()->set_width(source.width);
  page.mutable_size()->set_height(source.height);

  for (const auto& line : source.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    const int text_index = document->texts_size();
    const std::string self_ref = "#/texts/" + std::to_string(text_index);
    document->mutable_body()->add_children()->set_ref(self_ref);

    auto* base = document->add_texts()->mutable_text()->mutable_base();
    base->set_self_ref(self_ref);
    base->mutable_parent()->set_ref("#/body");
    base->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
    base->set_label(docling::core::v1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(line.text);
    base->set_text(line.text);

    auto* provenance = base->add_prov();
    provenance->set_page_no(page_number);
    auto* box = provenance->mutable_bbox();
    int left = std::numeric_limits<int>::max();
    int top = std::numeric_limits<int>::max();
    int right = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    for (const auto& point : line.polygon) {
      left = std::min(left, point.x);
      top = std::min(top, point.y);
      right = std::max(right, point.x);
      bottom = std::max(bottom, point.y);
    }
    box->set_l(left);
    box->set_t(top);
    box->set_r(right);
    box->set_b(bottom);
    box->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);

    if (!plain_text->empty()) plain_text->append("\n");
    plain_text->append(line.text);
  }
}

bool requested(const docling::serve::v1::ConvertDocumentOptions& options,
               docling::serve::v1::OutputFormat format) {
  return options.to_formats().empty() ||
         std::find(options.to_formats().begin(), options.to_formats().end(), format) != options.to_formats().end();
}

int append_page_data(const OcrEngine::Page& source, int page_number, int text_offset,
                     docling::serve::v1::PageData* output) {
  output->set_page_number(page_number);
  output->mutable_page_meta()->set_page_no(page_number);
  output->mutable_page_meta()->mutable_size()->set_width(source.width);
  output->mutable_page_meta()->mutable_size()->set_height(source.height);
  for (const auto& line : source.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    auto* base = output->add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(text_offset++));
    base->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
    base->set_label(docling::core::v1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(line.text);
    base->set_text(line.text);
    auto* provenance = base->add_prov();
    provenance->set_page_no(page_number);
    auto* box = provenance->mutable_bbox();
    int left = std::numeric_limits<int>::max();
    int top = std::numeric_limits<int>::max();
    int right = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    for (const auto& point : line.polygon) {
      left = std::min(left, point.x);
      top = std::min(top, point.y);
      right = std::max(right, point.x);
      bottom = std::max(bottom, point.y);
    }
    box->set_l(left);
    box->set_t(top);
    box->set_r(right);
    box->set_b(bottom);
    box->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);
  }
  return text_offset;
}

}  // namespace

DocumentParserService::DocumentParserService(OcrEngine& engine) : engine_(engine) {}

grpc::Status DocumentParserService::ConvertSource(
    grpc::ServerContext*, const docling::serve::v1::ConvertSourceRequest* request,
    docling::serve::v1::ConvertSourceResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  const auto& sources = request->request().sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "ConvertSource currently accepts exactly one FileSource containing base64_string");
  }
  try {
    const auto& source = sources.Get(0).file();
    const std::string bytes = decode_base64(source.base64_string());
    const fs::path requested_name = source.filename().empty() ? "document.pdf" : fs::path(source.filename()).filename();
    TemporaryDirectory temporary;
    std::vector<fs::path> pages;
    if (is_pdf(bytes, requested_name)) {
      const auto pdf = temporary.path() / "document.pdf";
      write_document(pdf, bytes);
      pages = render_pdf(pdf, temporary.path());
    } else {
      const auto image = temporary.path() / (requested_name.extension().empty() ? "document.png" : requested_name);
      write_document(image, bytes);
      pages.push_back(image);
    }

    auto* converted = response->mutable_response();
    auto* result = converted->mutable_document();
    result->set_filename(requested_name.string());
    auto* document = result->mutable_doc();
    document->set_schema_name("docling_document");
    document->set_version("1.0.0");
    document->set_name(requested_name.filename().string());
    auto* origin = document->mutable_origin();
    origin->set_filename(requested_name.filename().string());
    origin->set_mimetype(mimetype_for(requested_name));
    origin->set_binary_hash(content_hash(bytes));
    document->mutable_body()->set_self_ref("#/body");
    document->mutable_body()->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);

    std::string plain_text;
    for (size_t index = 0; index < pages.size(); ++index) {
      append_page(engine_.extract_page(pages[index]), static_cast<int>(index + 1), document, &plain_text);
    }
    if (requested(request->request().options(), docling::serve::v1::OUTPUT_FORMAT_TEXT)) {
      result->mutable_exports()->set_text(plain_text);
    }
    if (requested(request->request().options(), docling::serve::v1::OUTPUT_FORMAT_MARKDOWN)) {
      result->mutable_exports()->set_md(plain_text);
    }
    converted->set_status(docling::serve::v1::CONVERSION_STATUS_SUCCESS);
    converted->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    (*converted->mutable_timings())["ocr"] = converted->processing_time();
    return grpc::Status::OK;
  } catch (const std::exception& error) {
    return grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  }
}

grpc::Status DocumentParserService::Health(grpc::ServerContext*, const docling::serve::v1::HealthRequest*,
                                            docling::serve::v1::HealthResponse* response) {
  response->set_status("ready");
  response->set_version("grparse-0.1.0-cuda");
  return grpc::Status::OK;
}

DocumentStreamingService::DocumentStreamingService(OcrEngine& engine) : engine_(engine) {}

grpc::Status DocumentStreamingService::StreamProcessDocument(
    grpc::ServerContext*,
    grpc::ServerReaderWriter<docling::serve::v1::DocumentStreamEvent, docling::serve::v1::DocumentChunk>* stream) {
  constexpr size_t kMaximumDocumentBytes = 50U * 1024U * 1024U;
  docling::serve::v1::DocumentChunk chunk;
  std::string document_id;
  fs::path filename;
  std::string content_type;
  std::string bytes;
  bool complete = false;
  while (stream->Read(&chunk)) {
    if (complete) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "received data after complete chunk");
    if (document_id.empty()) {
      document_id = chunk.document_id();
      filename = fs::path(chunk.filename()).filename();
      content_type = chunk.content_type();
      if (document_id.empty() || filename.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "first chunk requires document_id and filename");
      }
    } else if ((!chunk.document_id().empty() && chunk.document_id() != document_id) ||
               (!chunk.filename().empty() && fs::path(chunk.filename()).filename() != filename)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "all chunks must describe the same document");
    }
    if (bytes.size() + chunk.data().size() > kMaximumDocumentBytes) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "document exceeds 50 MiB streaming limit");
    }
    bytes.append(chunk.data());
    complete = chunk.complete();
  }
  if (!complete || bytes.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "stream must end with a non-empty complete chunk");
  }

  try {
    TemporaryDirectory temporary;
    const bool pdf = content_type == "application/pdf" || is_pdf(bytes, filename);
    const auto source_path = temporary.path() / (pdf ? "document.pdf" : filename);
    write_document(source_path, bytes);
    const int total_pages = pdf ? pdf_page_count(source_path, temporary.path()) : 1;
    int text_offset = 0;
    for (int page_number = 1; page_number <= total_pages; ++page_number) {
      const auto image = pdf ? render_pdf_page(source_path, page_number, temporary.path()) : source_path;
      google::protobuf::Arena page_arena;
      auto* event = google::protobuf::Arena::Create<docling::serve::v1::DocumentStreamEvent>(&page_arena);
      event->set_document_id(document_id);
      event->set_total_pages(total_pages);
      text_offset = append_page_data(engine_.extract_page(image), page_number, text_offset, event->mutable_page());
      if (!stream->Write(*event)) return grpc::Status::OK;
    }
    google::protobuf::Arena final_arena;
    auto* final_event = google::protobuf::Arena::Create<docling::serve::v1::DocumentStreamEvent>(&final_arena);
    final_event->set_document_id(document_id);
    final_event->set_total_pages(total_pages);
    auto* origin = final_event->mutable_complete()->mutable_origin();
    origin->set_filename(filename.string());
    origin->set_mimetype(pdf ? "application/pdf" : mimetype_for(filename));
    origin->set_binary_hash(content_hash(bytes));
    stream->Write(*final_event);
    return grpc::Status::OK;
  } catch (const std::exception& error) {
    return grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  }
}

}  // namespace grparse
