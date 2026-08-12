// Fuzzes the PDF ingest door: open_in_memory_document(pdf=true) plus the
// digital-text extraction pass, which is the Poppler surface every streamed
// document crosses before a model ever runs.  The contract under test:
// malformed bytes fail with InvalidDocument, never a crash, hang, or
// sanitizer finding.  Rendering is deliberately excluded - a fuzzed MediaBox
// can demand a multi-gigabyte raster, which only measures the RSS limit.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <poppler/cpp/poppler-global.h>

#include "grparse/in_memory_document.h"

namespace {

// Poppler reports malformed-document diagnostics to stderr by default;
// silence them so fuzzer output stays readable.
const bool poppler_silenced = [] {
  poppler::set_debug_error_function([](const std::string&, void*) {}, nullptr);
  return true;
}();

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  (void)poppler_silenced;
  auto bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(data), size);
  try {
    const auto source = grparse::open_in_memory_document(std::move(bytes), true);
    // A fuzzed xref table can claim an absurd page count; walk a few pages
    // only.  Out-of-range and unparseable pages must both fail cleanly.
    const int pages = std::min(source->page_count(), 4);
    for (int page = 1; page <= pages; ++page) {
      (void)source->extract_digital_page(page);
    }
  } catch (const grparse::InvalidDocument&) {
    // Expected door for malformed input.
  }
  return 0;
}
