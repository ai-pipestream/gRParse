#pragma once

#include <memory>
#include <string>
#include <vector>

#include "grparse/in_memory_document.h"

namespace grparse {

// Splits a GRPARSE_PDF_BACKEND value into targets ("a:1,b:2" -> two).
std::vector<std::string> split_backend_targets(const std::string& value);

// The word fold the consensus vote reconciles on: ASCII and Latin-1
// case-folded, curly quotes and long dashes straightened, soft hyphens
// dropped, common Latin ligatures expanded. Declared here so the unit
// tests can pin the fold directly; not part of the PageSource surface.
std::string fold_word(const std::string& word);

// A PageSource over several PdfBackend targets: every backend reads the
// same document, and each page's text comes from the backend whose word
// order wins a bigram-agreement vote (adjacent word pairs the other
// backends also emit adjacently), with a sentence-continuity fallback for
// the two-backend case where agreement is symmetric. This is the
// production form of the consensus prototype in eval/consensus, which
// picked the truth-perfect order on every truth document. Targets that
// cannot load the document drop out; rasters come from the first target
// that loaded, so target order states raster priority. A vote that runs
// short is never silent: page_warnings() carries, typed, each backend that
// is out of the document (WARNING_CODE_CONSENSUS_BACKEND_FAILED: could not
// open it, or taken out after repeated page failures), a disagreement on
// the page count (WARNING_CODE_CONSENSUS_PAGE_COUNT_DISAGREEMENT), and each
// page decided by fewer backends than were configured
// (WARNING_CODE_CONSENSUS_PAGE_UNDERVOTED), and every one of them moves
// grparse_pdf_consensus_events_total (ConsensusEvent). `opening` says
// whether the legs' Probes run before this returns or on first use. When
// every leg fails, a cancel fails as cancelled and a backend outage as
// PdfBackendUnavailable, not as a bad document. `precomputed_sha256`, when
// non-empty, is the hex SHA-256 of `*bytes`, shared by every leg in place of
// hashing the document here.
std::shared_ptr<PageSource> open_consensus_pdf_document(
    std::shared_ptr<const std::string> bytes,
    const std::vector<std::string>& targets, double render_dpi,
    SourceOpening opening = SourceOpening::kNow, std::string precomputed_sha256 = {});

}  // namespace grparse
