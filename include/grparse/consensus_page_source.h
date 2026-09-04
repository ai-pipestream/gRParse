#pragma once

#include <memory>
#include <string>
#include <vector>

#include "grparse/in_memory_document.h"

namespace grparse {

// Splits a GRPARSE_PDF_BACKEND value into targets ("a:1,b:2" -> two).
std::vector<std::string> split_backend_targets(const std::string& value);

// A PageSource over several PdfBackend targets: every backend reads the
// same document, and each page's text comes from the backend whose word
// order wins a bigram-agreement vote (adjacent word pairs the other
// backends also emit adjacently), with a sentence-continuity fallback for
// the two-backend case where agreement is symmetric. This is the
// production form of the consensus prototype in eval/consensus, which
// picked the truth-perfect order on every truth document. Targets that
// cannot load the document drop out; rasters come from the first target
// that loaded, so target order states raster priority.
std::shared_ptr<PageSource> open_consensus_pdf_document(
    std::shared_ptr<const std::string> bytes,
    const std::vector<std::string>& targets, double render_dpi);

}  // namespace grparse
