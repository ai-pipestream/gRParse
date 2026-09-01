#pragma once

#include <cstdint>

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

struct OfficeCvReport {
  // Figures the detector found and the document now carries.
  int pictures_added = 0;
  // Detections dropped because a picture already there (the office core's
  // own, or an earlier detection on the page) covers the same place.
  int pictures_deduplicated = 0;
  // Of those, the ones placed in the body at their provenance position
  // (the rest sit at the end of the body in a stable order).
  int pictures_anchored = 0;
};

// Runs layout detection, figure classification, and barcode decoding over a
// mapped office document's page images (the PNG data URIs the LibreOffice
// collector stores on each PageItem) and adds the detected figures as
// source-tagged PictureItem entries, boxes converted from render pixels into
// the document's page coordinate space (twips).  Native office text and
// tables are exact already, so this adds only what the office core cannot
// see: where the figures are, what they depict, and any barcode payloads.
// Pages run in page order and each page's figures in (top, left) order, so
// the pictures arena is the same run to run; a detection that lands where
// the document already has a picture (the office core's own drawing, or an
// earlier detection) is dropped rather than doubled; every figure kept is
// then placed in the body at its provenance position
// (document_reading_order.h) rather than appended. Pages without an image,
// or whose image cannot be decoded, are skipped.
OfficeCvReport enrich_office_document(const OfficeCvEnrichment& enrichment,
                                      ai::pipestream::document::v1::Document* document);

// Process-wide totals of the figures this leg added and anchored since
// startup, beside the repair totals in the metrics exposition.
struct OfficeCvTotals {
  uint64_t pictures_added = 0;
  uint64_t pictures_anchored = 0;
};

OfficeCvTotals office_cv_totals();

}  // namespace grparse
