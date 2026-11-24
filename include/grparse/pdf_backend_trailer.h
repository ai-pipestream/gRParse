#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"
#include "grparse/warning.h"

namespace grparse {

// One Parse answer of a PdfBackendService (the unary stream, or one request
// on a ParseStream) held against its own trailer. The contract closes every
// answer with a ParseTrailer: per-family totals "the harness reconciles
// against what it received", and a ParseWarning for each thing the engine
// "skipped or repaired without failing the stream". A client that drops the
// trailer takes a page the engine read half of, or an answer that lost items
// on the way, for a full read; so every client feeds each message of the
// answer to absorb() and then asks what the answer says about itself.
//
// An answer without a trailer says nothing here: the call's own status is
// then the only word on whether the stream was whole.
class PdfParseTally {
 public:
  void absorb(const ai::protomolt::parse::pdf::v1::ParseResponse& message);

  // Whether the answer is known to hold less of `family` on the zero-based
  // `page_index` than the page has: the engine named that page in a warning
  // about the family (or about no family in particular), or the trailer
  // counts more items of the family than arrived. A warning that names no
  // page is about the document, not about this page's content, and does not
  // count.
  bool reads_short(ai::protomolt::parse::pdf::v1::PdfFamily family, uint32_t page_index) const;

  // The answer's account of itself as typed warnings, the engine's first and
  // in trailer order: a WARNING_CODE_BACKEND_CONTENT_SKIPPED for each
  // ParseWarning, on the page the engine named (one-based) when it named
  // one, and a WARNING_CODE_BACKEND_COUNT_MISMATCH for each family whose
  // trailer total differs from the number of items that arrived, on
  // `request_page` when the answer was for that one page. `backend` is the
  // name the messages give the backend. Only families whose items a client
  // can count on the wire are compared: the repeated fields of a PageChunk
  // and the font table.
  std::vector<Warning> warnings(std::string_view backend,
                                std::optional<int32_t> request_page) const;

 private:
  uint64_t received(ai::protomolt::parse::pdf::v1::PdfFamily family) const;

  std::map<ai::protomolt::parse::pdf::v1::PdfFamily, uint64_t> received_;
  std::optional<ai::protomolt::parse::pdf::v1::ParseTrailer> trailer_;
};

}  // namespace grparse
