// Fuzzes the raster ingest door: open_in_memory_document(pdf=false), the
// container-magic admission gate, and the in-memory cv::imdecode pass.  The
// contract under test: unsupported or undecodable bytes fail with
// InvalidDocument, never a crash or sanitizer finding.  The magic gate keeps
// OpenCV's exotic decoders (WebP/BMP/GDAL) out of the fuzzed surface, which is
// also why a sustained campaign no longer exhausts descriptors on those paths.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "grparse/in_memory_document.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(data), size);
  try {
    const auto source = grparse::open_in_memory_document(std::move(bytes), false);
    (void)source->render_page(1);
  } catch (const grparse::InvalidDocument&) {
    // Expected door for undecodable input.
  }
  return 0;
}
