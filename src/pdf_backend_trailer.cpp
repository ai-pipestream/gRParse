#include "grparse/pdf_backend_trailer.h"

#include <limits>
#include <string>
#include <utility>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grparse {
namespace {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;
namespace docv1 = ai::pipestream::document::v1;

// The families a client can count for itself: each item is one entry of a
// repeated field on the wire. The document-level families are left out
// because their trailer totals are not item counts a client can redo (a
// structure tree is counted node by node, the page inventory by the
// document's own page count, which a backend may state for pages it could
// not load).
bool countable(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_TEXT_CELLS:
    case pdfv1::PDF_FAMILY_FONTS:
    case pdfv1::PDF_FAMILY_PLACED_IMAGES:
    case pdfv1::PDF_FAMILY_HYPERLINKS:
    case pdfv1::PDF_FAMILY_ANNOTATIONS:
    case pdfv1::PDF_FAMILY_FORM_FIELDS:
    case pdfv1::PDF_FAMILY_VECTOR_SHAPES:
      return true;
    default:
      return false;
  }
}

// The family by its contract name, or by number for a value this build's
// copy of the contract does not know yet.
std::string family_label(pdfv1::PdfFamily family) {
  const std::string& name = pdfv1::PdfFamily_Name(family);
  return name.empty() ? "family " + std::to_string(static_cast<int>(family)) : name;
}

bool names_family(const pdfv1::ParseWarning& warning) {
  return warning.has_family() && warning.family() != pdfv1::PDF_FAMILY_UNSPECIFIED;
}

}  // namespace

void PdfParseTally::absorb(const pdfv1::ParseResponse& message) {
  switch (message.payload_case()) {
    case pdfv1::ParseResponse::kPage: {
      const pdfv1::PageChunk& page = message.page();
      received_[pdfv1::PDF_FAMILY_TEXT_CELLS] += static_cast<uint64_t>(page.text_cells_size());
      received_[pdfv1::PDF_FAMILY_PLACED_IMAGES] += static_cast<uint64_t>(page.images_size());
      received_[pdfv1::PDF_FAMILY_HYPERLINKS] += static_cast<uint64_t>(page.hyperlinks_size());
      received_[pdfv1::PDF_FAMILY_ANNOTATIONS] += static_cast<uint64_t>(page.annotations_size());
      received_[pdfv1::PDF_FAMILY_FORM_FIELDS] += static_cast<uint64_t>(page.form_fields_size());
      received_[pdfv1::PDF_FAMILY_VECTOR_SHAPES] += static_cast<uint64_t>(page.shapes_size());
      break;
    }
    case pdfv1::ParseResponse::kFonts:
      received_[pdfv1::PDF_FAMILY_FONTS] += static_cast<uint64_t>(message.fonts().fonts_size());
      break;
    case pdfv1::ParseResponse::kTrailer:
      trailer_ = message.trailer();
      break;
    default:
      break;
  }
}

uint64_t PdfParseTally::received(pdfv1::PdfFamily family) const {
  const auto found = received_.find(family);
  return found == received_.end() ? 0 : found->second;
}

bool PdfParseTally::reads_short(pdfv1::PdfFamily family, uint32_t page_index) const {
  if (!trailer_.has_value()) return false;
  for (const pdfv1::ParseWarning& warning : trailer_->warnings()) {
    if (!warning.has_page_index() || warning.page_index() != page_index) continue;
    if (!names_family(warning) || warning.family() == family) return true;
  }
  if (!countable(family)) return false;
  for (const pdfv1::FamilyCount& total : trailer_->counts()) {
    if (total.family() == family && total.count() > received(family)) return true;
  }
  return false;
}

std::vector<Warning> PdfParseTally::warnings(std::string_view backend,
                                             std::optional<int32_t> request_page) const {
  std::vector<Warning> out;
  if (!trailer_.has_value()) return out;
  const std::string who = "PDF backend " + std::string(backend);
  for (const pdfv1::ParseWarning& said : trailer_->warnings()) {
    // Zero-based on the backend's wire, one-based on the Document's. An
    // index past what page_no can hold names no page a Document could have.
    const bool on_page =
        said.has_page_index() &&
        said.page_index() < static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    std::string text = who + " skipped or repaired content";
    if (on_page) text += " on page " + std::to_string(said.page_index() + 1);
    if (names_family(said)) text += " (" + family_label(said.family()) + ")";
    text += ": " + (said.message().empty() ? std::string("no detail given") : said.message());
    Warning warning(docv1::WARNING_CODE_BACKEND_CONTENT_SKIPPED, std::move(text));
    if (on_page) warning.page_no = static_cast<int32_t>(said.page_index() + 1);
    out.push_back(std::move(warning));
  }
  for (const pdfv1::FamilyCount& total : trailer_->counts()) {
    if (!countable(total.family())) continue;
    const uint64_t arrived = received(total.family());
    if (total.count() == arrived) continue;
    std::string text = who + ": the trailer counts " + std::to_string(total.count()) + " " +
                       family_label(total.family()) + " item(s)";
    if (request_page.has_value()) text += " for page " + std::to_string(*request_page);
    text += ", " + std::to_string(arrived) + " arrived";
    Warning warning(docv1::WARNING_CODE_BACKEND_COUNT_MISMATCH, std::move(text));
    warning.page_no = request_page;
    out.push_back(std::move(warning));
  }
  return out;
}

}  // namespace grparse
