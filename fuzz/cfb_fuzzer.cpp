// Fuzzes the [MS-CFB] compound-file walker in content_sniff.cpp: the header,
// the FAT through the header DIFAT and the DIFAT chain, the directory chain,
// the mini FAT and the mini stream, reached through its two callers
// (encrypted_office_document, which reads stream heads, and sniff_mimetype,
// which places a compound file by its root streams). Every input is made a
// compound file first by writing the signature over its first eight bytes,
// so no mutation is spent outside the walker.
//
// The contract under test: any bytes end the walk, never the process, and
// the walk holds heap linear in the input (the heap budget). #225, a DIFAT
// chain that names one FAT sector over and over, broke the second half; its
// reproducer is in fuzz/regressions/cfb-fuzzer/.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "grparse/content_sniff.h"
#include "heap_budget.h"

namespace {

constexpr std::string_view kSignature{"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8};

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  grparse_fuzz::install_heap_budget();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string bytes(reinterpret_cast<const char*>(data), size);
  bytes.replace(0, kSignature.size(), kSignature);
  {
    const grparse_fuzz::HeapBudget budget(bytes.size(), 16, size_t{4} << 20);
    const auto verdict = grparse::encrypted_office_document(bytes);
    if (verdict.has_value() && (verdict->format.empty() || verdict->evidence.empty())) {
      __builtin_trap();  // a verdict always says what and why
    }
    (void)grparse::sniff_mimetype(bytes);
  }
  return 0;
}
