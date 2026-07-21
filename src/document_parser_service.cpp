#include "grparse/document_parser_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <google/protobuf/arena.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

namespace fs = std::filesystem;
namespace docling = ai::docling;

namespace grparse {
namespace {

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

class PdfRenderer {
 public:
  explicit PdfRenderer(const std::string& bytes) : bytes_(bytes) {}

  int page_count() const {
    auto document = load();
    const int pages = document->pages();
    if (pages <= 0) throw std::runtime_error("PDF does not contain a renderable page");
    return pages;
  }

  cv::Mat render_page(int page_number) const {
    auto document = load();
    std::unique_ptr<poppler::page> page(document->create_page(page_number - 1));
    if (!page) throw std::runtime_error("PDF page could not be opened");
    poppler::page_renderer renderer;
    renderer.set_image_format(poppler::image::format_bgr24);
    const poppler::image image = renderer.render_page(page.get(), 200.0, 200.0);
    if (!image.is_valid()) throw std::runtime_error("PDF page could not be rendered in memory");
    return cv::Mat(image.height(), image.width(), CV_8UC3, const_cast<char*>(image.const_data()),
                   static_cast<size_t>(image.bytes_per_row()))
        .clone();
  }

 private:
  std::unique_ptr<poppler::document> load() const {
    auto* raw = poppler::document::load_from_raw_data(bytes_.data(), static_cast<int>(bytes_.size()));
    if (raw == nullptr) throw std::runtime_error("PDF could not be opened from memory");
    return std::unique_ptr<poppler::document>(raw);
  }

  const std::string& bytes_;
};

cv::Mat decode_image(const std::string& bytes) {
  const auto* begin = reinterpret_cast<const unsigned char*>(bytes.data());
  std::vector<unsigned char> encoded(begin, begin + bytes.size());
  const cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
  if (image.empty()) throw std::runtime_error("Raster image could not be decoded from memory");
  return image;
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

DocumentParserService::DocumentParserService(OcrEnginePool& engines) : engines_(engines) {}

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
    const bool pdf = is_pdf(bytes, requested_name);

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
    if (pdf) {
      PdfRenderer renderer(bytes);
      const int pages = renderer.page_count();
      for (int page_number = 1; page_number <= pages; ++page_number) {
        append_page(engines_.extract_page(renderer.render_page(page_number)), page_number, document, &plain_text);
      }
    } else {
      append_page(engines_.extract_page(decode_image(bytes)), 1, document, &plain_text);
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

DocumentStreamingService::DocumentStreamingService(OcrEnginePool& engines) : engines_(engines) {}

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
    const bool pdf = content_type == "application/pdf" || is_pdf(bytes, filename);
    const int total_pages = pdf ? PdfRenderer(bytes).page_count() : 1;
    struct Completion {
      int page_number;
      std::unique_ptr<OcrEngine::Page> page;
      std::exception_ptr error;
    };
    std::mutex completed_mutex;
    std::condition_variable completed_cv;
    std::deque<Completion> completed;
    std::atomic<int> next_page{1};
    std::atomic<bool> stop{false};
    const size_t worker_count = std::min<size_t>(total_pages, engines_.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&] {
        try {
          auto lease = engines_.acquire();
          std::unique_ptr<PdfRenderer> renderer;
          if (pdf) renderer = std::make_unique<PdfRenderer>(bytes);
          while (!stop.load()) {
            const int page_number = next_page.fetch_add(1);
            if (page_number > total_pages) return;
            cv::Mat image = pdf ? renderer->render_page(page_number) : decode_image(bytes);
            auto result = std::make_unique<OcrEngine::Page>(lease.engine().extract_page(image));
            {
              std::lock_guard<std::mutex> lock(completed_mutex);
              completed.push_back(Completion{page_number, std::move(result), nullptr});
            }
            completed_cv.notify_one();
          }
        } catch (...) {
          {
            std::lock_guard<std::mutex> lock(completed_mutex);
            completed.push_back(Completion{0, nullptr, std::current_exception()});
          }
          stop.store(true);
          completed_cv.notify_one();
        }
      });
    }

    int text_offset = 0;
    int pages_sent = 0;
    std::exception_ptr failure;
    while (pages_sent < total_pages) {
      Completion completion;
      {
        std::unique_lock<std::mutex> lock(completed_mutex);
        completed_cv.wait(lock, [&] { return !completed.empty(); });
        completion = std::move(completed.front());
        completed.pop_front();
      }
      if (completion.error) {
        failure = completion.error;
        stop.store(true);
        break;
      }
      google::protobuf::Arena page_arena;
      auto* event = google::protobuf::Arena::Create<docling::serve::v1::DocumentStreamEvent>(&page_arena);
      event->set_document_id(document_id);
      event->set_total_pages(total_pages);
      text_offset = append_page_data(*completion.page, completion.page_number, text_offset, event->mutable_page());
      ++pages_sent;
      if (!stream->Write(*event)) {
        stop.store(true);
        break;
      }
    }
    for (auto& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
    if (pages_sent != total_pages) return grpc::Status::OK;

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
