#pragma once

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/figure_classifier.h"
#include "grparse/layout_engine.h"
#include "grparse/page_scheduler.h"

namespace grparse {

// The CV engines the office collector may run over LibreOffice page renders.
// All optional: a null detector disables enrichment entirely, a null
// classifier skips classification (and class-triggered barcode decoding
// never fires without it).  The pools behind these pointers are shared with
// the scheduler's inference stage and are thread-safe.
struct OfficeCvEnrichment {
  RegionDetector* detector = nullptr;
  FigureClassifierBase* classifier = nullptr;
  PageScheduler::BarcodeMode barcode_mode = PageScheduler::BarcodeMode::kOff;
};

// Runs layout detection, figure classification, and barcode decoding over a
// mapped office document's page images (the PNG data URIs the LibreOffice
// collector stores on each PageItem) and appends the detected figures as
// source-tagged PictureItem entries, boxes converted from render pixels into
// the document's page coordinate space (twips).  Native office text and
// tables are exact already, so this adds only what the office core cannot
// see: where the figures are, what they depict, and any barcode payloads.
// Pages without an image, or whose image cannot be decoded, are skipped.
void enrich_office_document(const OfficeCvEnrichment& enrichment,
                            ai::pipestream::document::v1::Document* document);

}  // namespace grparse
