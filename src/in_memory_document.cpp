#include "grparse/in_memory_document.h"

#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

class RasterPageSource final : public PageSource {
 public:
  explicit RasterPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)) {
    if (bytes_->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("Raster image exceeds the in-memory decode limit");
    }
    if (!is_supported_raster(*bytes_)) {
      throw InvalidDocument("Raster image is not a supported format (PNG, JPEG, or TIFF)");
    }
  }

  int page_count() const override { return 1; }

  cv::Mat render_page(int page_number) const override {
    if (page_number != 1) throw InvalidDocument("Raster page number is out of range");
    // Decode from the existing buffer without an intermediate std::vector copy.
    const cv::Mat encoded(1, static_cast<int>(bytes_->size()), CV_8UC1,
                          const_cast<char*>(bytes_->data()));
    cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image.empty()) throw InvalidDocument("Raster image could not be decoded from memory");
    return image;
  }

 private:
  std::shared_ptr<const std::string> bytes_;
};

}  // namespace

std::optional<OcrPage> PageSource::extract_digital_page(int) const { return std::nullopt; }

std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    double render_dpi) {
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
      return open_consensus_pdf_document(std::move(bytes), targets, render_dpi);
    }
    return open_remote_pdf_document(std::move(bytes), targets.front(), render_dpi);
  }
#endif
  throw PdfBackendNotConfigured(
      "PDF input needs a PDF backend service: set GRPARSE_PDF_BACKEND to a "
      "PdfBackendService target such as grpc-pdfium (host:port)");
}

}  // namespace grparse
