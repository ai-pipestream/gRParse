// Fuzzes the door a web page's JSON-LD script passes: the strict JSON
// reader and the fold of its tree into document.v1.StructuredData.
//
// The whole input is the script's text. The contract under test: any text
// ends in a decode or a refusal with a reason, never a crash, a sanitizer
// finding or a stack the nesting cap does not bound; a refusal leaves no
// items; and the heap held is linear in the input.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/json_reader.h"
#include "grparse/structured_data.h"
#include "heap_budget.h"

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  grparse_fuzz::install_heap_budget();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  // The smallest value is two bytes of text ("[]", or "0,") and costs one
  // tree node and at most one message: both a fixed size per value.
  const grparse_fuzz::HeapBudget budget(size, 512, size_t{1} << 20);
  const grparse::json::Parsed parsed = grparse::json::parse(text);
  if (parsed.ok != parsed.error.empty()) __builtin_trap();  // ok, or a reason
  ai::pipestream::document::v1::StructuredData decoded;
  const grparse::JsonLdDecode decode = grparse::decode_json_ld(text, &decoded);
  if (decode.decoded != decode.error.empty()) __builtin_trap();
  if (!decode.decoded && (decoded.items_size() != 0 || decoded.contexts_size() != 0)) {
    __builtin_trap();  // a refusal leaves nothing half read
  }
  if (decode.decoded && !parsed.ok) __builtin_trap();
  return 0;
}
