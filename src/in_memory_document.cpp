#include "grparse/in_memory_document.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

// The standalone fuzz project compiles this file without the gRPC-backed
// sources (fuzz/CMakeLists.txt); a fuzzer must never dial a backend, so
// the whole PDF dispatch compiles out there.
#ifndef GRPARSE_FUZZ_STANDALONE
#include "grparse/consensus_page_source.h"
#include "grparse/remote_page_source.h"
#endif

namespace grparse {
namespace {

// The service advertises PNG, JPEG, and TIFF raster input.  cv::imdecode,
// left to sniff on its own, accepts far more - WebP, BMP, and, where the
// build links GDAL, JPEG2000/NITF/GeoTIFF, some of which spill the buffer to
// a temp file (breaking the diskless guarantee) and can leak descriptors on
// malformed input.  Gate on the container magic before imdecode ever runs so
// only the three advertised formats reach a decoder.
bool is_supported_raster(const std::string& bytes) {
  auto starts_with = [&bytes](std::initializer_list<unsigned char> magic) {
    if (bytes.size() < magic.size()) return false;
    size_t index = 0;
    for (const unsigned char expected : magic) {
      if (static_cast<unsigned char>(bytes[index++]) != expected) return false;
    }
    return true;
  };
  return starts_with({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}) ||  // PNG
         starts_with({0xFF, 0xD8, 0xFF}) ||                                // JPEG
         starts_with({0x49, 0x49, 0x2A, 0x00}) ||                          // TIFF little-endian
         starts_with({0x4D, 0x4D, 0x00, 0x2A});                            // TIFF big-endian
}

bool is_tiff(const std::string& bytes) {
  return bytes.size() >= 4 && (bytes.compare(0, 4, "II*\0", 4) == 0 ||
                               bytes.compare(0, 4, "MM\0*", 4) == 0);
}

struct RasterDims {
  uint64_t width = 0;
  uint64_t height = 0;
};

// Big-endian unless `little`; the caller has bounds-checked [at, at + width).
uint32_t read_uint(const std::string& bytes, size_t at, int width, bool little = false) {
  uint32_t value = 0;
  for (int index = 0; index < width; ++index) {
    const auto byte = static_cast<unsigned char>(bytes[at + (little ? width - 1 - index : index)]);
    value = (value << 8) | byte;
  }
  return value;
}

// PNG: the IHDR chunk is first, right after the signature.
std::optional<RasterDims> png_dims(const std::string& bytes) {
  if (bytes.size() < 24 || bytes.compare(12, 4, "IHDR") != 0) return std::nullopt;
  return RasterDims{read_uint(bytes, 16, 4), read_uint(bytes, 20, 4)};
}

// JPEG: walk the marker segments to the first start-of-frame header.
std::optional<RasterDims> jpeg_dims(const std::string& bytes) {
  size_t at = 2;
  while (at + 4 <= bytes.size()) {
    if (static_cast<unsigned char>(bytes[at]) != 0xFF) return std::nullopt;
    const auto marker = static_cast<unsigned char>(bytes[at + 1]);
    if (marker == 0xFF) {  // fill byte
      ++at;
      continue;
    }
    at += 2;
    if (marker == 0x01 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9 || marker == 0xDA) return std::nullopt;  // no frame before the scan
    const uint32_t length = read_uint(bytes, at, 2);
    if (length < 2) return std::nullopt;
    const bool start_of_frame =
        marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (start_of_frame) {
      if (at + 7 > bytes.size()) return std::nullopt;
      return RasterDims{read_uint(bytes, at + 5, 2), read_uint(bytes, at + 3, 2)};
    }
    at += length;
  }
  return std::nullopt;
}

// TIFF: one entry per image file directory on the main IFD chain, which is
// what libtiff (and so imdecodemulti) counts as pages.
std::optional<std::vector<RasterDims>> tiff_dims(const std::string& bytes) {
  if (bytes.size() < 8) return std::nullopt;
  const bool little = bytes[0] == 'I';
  std::vector<RasterDims> pages;
  std::unordered_set<uint32_t> visited;
  uint32_t offset = read_uint(bytes, 4, 4, little);
  while (offset != 0) {
    if (!visited.insert(offset).second) return std::nullopt;  // a looped chain
    if (static_cast<uint64_t>(offset) + 2 > bytes.size()) return std::nullopt;
    const uint32_t entries = read_uint(bytes, offset, 2, little);
    const uint64_t end = static_cast<uint64_t>(offset) + 2 + 12ULL * entries;
    if (end + 4 > bytes.size()) return std::nullopt;
    RasterDims dims;
    for (uint32_t index = 0; index < entries; ++index) {
      const size_t entry = offset + 2 + 12 * static_cast<size_t>(index);
      const uint32_t tag = read_uint(bytes, entry, 2, little);
      if (tag != 256 && tag != 257) continue;  // ImageWidth, ImageLength
      const uint32_t type = read_uint(bytes, entry + 2, 2, little);
      uint32_t value = 0;
      if (type == 3) {
        value = read_uint(bytes, entry + 8, 2, little);  // SHORT
      } else if (type == 4) {
        value = read_uint(bytes, entry + 8, 4, little);  // LONG
      } else {
        return std::nullopt;
      }
      (tag == 256 ? dims.width : dims.height) = value;
    }
    pages.push_back(dims);
    offset = read_uint(bytes, static_cast<size_t>(end), 4, little);
  }
  if (pages.empty()) return std::nullopt;
  return pages;
}

class RasterPageSource final : public PageSource {
 public:
  explicit RasterPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)) {
    if (bytes_->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("Raster image exceeds the in-memory decode limit");
    }
    if (!is_supported_raster(*bytes_)) {
      throw InvalidDocument("Raster image is not a supported format (PNG, JPEG, or TIFF)");
    }
    // The header states each page's size before any decoder allocates for
    // it: a few KB of PNG or TIFF can otherwise inflate to gigabytes.
    std::optional<std::vector<RasterDims>> pages;
    if (is_tiff(*bytes_)) {
      pages = tiff_dims(*bytes_);
    } else {
      const bool png = static_cast<unsigned char>((*bytes_)[0]) == 0x89;
      if (const auto dims = png ? png_dims(*bytes_) : jpeg_dims(*bytes_)) pages.emplace(1, *dims);
    }
    if (!pages.has_value()) throw InvalidDocument("Raster image header could not be read");
    const uint64_t limit = max_image_pixels();
    for (const RasterDims& dims : *pages) {
      if (dims.width == 0 || dims.height == 0) {
        throw InvalidDocument("Raster image header states an empty page");
      }
      if (dims.width * dims.height > limit) {
        throw InvalidDocument("Raster image page of " + std::to_string(dims.width) + "x" +
                              std::to_string(dims.height) +
                              " pixels exceeds GRPARSE_MAX_IMAGE_PIXELS (" +
                              std::to_string(limit) + ")");
      }
    }
    // Each page decode walks the TIFF directory chain from the start, so a
    // long chain of tiny pages costs quadratic time; real scans stay far
    // below this bound.
    if (pages->size() > kMaxRasterPages) {
      throw InvalidDocument("Raster image has more than " + std::to_string(kMaxRasterPages) +
                            " pages");
    }
    pages_ = static_cast<int>(pages->size());
#if !(CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7))
    // Without imdecodemulti only the first page decodes; say so up front
    // instead of failing the document on page 2.
    pages_ = std::min(pages_, 1);
#endif
  }

  int page_count() const override { return pages_; }

  cv::Mat render_page(int page_number) const override {
    if (page_number < 1 || page_number > pages_) {
      throw InvalidDocument("Raster page number is out of range");
    }
    // Decode from the existing buffer without an intermediate std::vector copy.
    const cv::Mat encoded(1, static_cast<int>(bytes_->size()), CV_8UC1,
                          const_cast<char*>(bytes_->data()));
    cv::Mat image;
    if (pages_ == 1) {
      image = cv::imdecode(encoded, cv::IMREAD_COLOR);
    } else {
      // A multi-page TIFF decodes only the asked-for page. imdecodemulti
      // arrived in OpenCV 4.7; the images build 5.x, and only the fuzz job's
      // distro OpenCV 4.6 lacks it, where page 1 is all that decodes.
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 7)
      std::vector<cv::Mat> decoded;
      if (cv::imdecodemulti(encoded, cv::IMREAD_COLOR, decoded,
                            cv::Range(page_number - 1, page_number)) &&
          decoded.size() == 1) {
        image = std::move(decoded.front());
      }
#else
      if (page_number == 1) image = cv::imdecode(encoded, cv::IMREAD_COLOR);
#endif
    }
    if (image.empty()) throw InvalidDocument("Raster image could not be decoded from memory");
    return image;
  }

 private:
  std::shared_ptr<const std::string> bytes_;
  int pages_ = 0;
};

}  // namespace

std::optional<OcrPage> PageSource::extract_digital_page(int) const { return std::nullopt; }

uint64_t max_image_pixels() {
  const char* configured = std::getenv("GRPARSE_MAX_IMAGE_PIXELS");
  if (configured == nullptr) return kDefaultMaxImagePixels;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(configured, &end, 10);
  // strtoull accepts a leading minus and wraps it to a huge value, which
  // would switch the cap off; only plain digits are taken.
  if (end == configured || *end != '\0' || parsed == 0 ||
      !std::isdigit(static_cast<unsigned char>(configured[0]))) {
    throw std::invalid_argument("GRPARSE_MAX_IMAGE_PIXELS must be a positive integer");
  }
  return static_cast<uint64_t>(parsed);
}

std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    double render_dpi,
                                                    [[maybe_unused]] SourceOpening opening) {
  if (!bytes || bytes->empty()) throw InvalidDocument("Document bytes are empty");
  if (!(render_dpi > 0.0)) throw std::invalid_argument("Render DPI must be positive");
  if (!pdf) return std::make_shared<RasterPageSource>(std::move(bytes));
#ifndef GRPARSE_FUZZ_STANDALONE
  // GRPARSE_PDF_BACKEND names the PdfBackendService targets that read PDFs.
  // A single target dials that backend; a comma-separated list reads the
  // document through every backend and takes each page from the one whose
  // word order wins the consensus vote.
  if (const auto target = remote_pdf_backend_target()) {
    const auto targets = split_backend_targets(*target);
    // A set-but-contentless value names no backend at all; failing here
    // beats dialing the literal string and waiting out a connect timeout.
    if (targets.empty()) {
      throw std::invalid_argument("GRPARSE_PDF_BACKEND names no backend targets");
    }
    if (targets.size() > 1) {
      return open_consensus_pdf_document(std::move(bytes), targets, render_dpi, opening);
    }
    return open_remote_pdf_document(std::move(bytes), targets.front(), render_dpi, opening);
  }
#endif
  throw PdfBackendNotConfigured(
      "PDF input needs a PDF backend service: set GRPARSE_PDF_BACKEND to a "
      "PdfBackendService target such as grpc-pdfium (host:port)");
}

}  // namespace grparse
