#pragma once

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Reading order over a finished Document, for bodies whose order is a guess
// from geometry. The CV path orders lines with the XY-cut before it emits
// items; a collector that folds a PDF text layer has no more evidence than
// that and less machinery, and its body arrives with floats and figure text
// at the top of each page. This module runs the same cut over the items.

struct BodyOrderOptions {
  // Collectors whose body order is derived from geometry rather than from
  // document structure. The pass runs only when every item in the document
  // came from one of these; a structural producer (an office suite's
  // outline, a markup tree) knows the order and is never overruled.
  std::vector<std::string> geometry_collectors{"pdf"};
};

struct BodyOrderReport {
  int pages_reordered = 0;
  // Direct body children whose position changed.
  int items_moved = 0;
};

// The body's direct children, page by page in page order, each page in
// reading order: a recursive XY-cut over the items' page boxes (columns read
// top to bottom then left to right, a full-width item is a band of its
// own), footnotes after the body of their page, page headers and footers
// still in the body last, and a caption right after the table or picture
// it visually labels. An item without a usable box rides with the item
// before it. Groups order by the union of their children and keep their
// own internal order. Pure: returns the new sequence of body positions.
std::vector<int> body_reading_order(const ai::pipestream::document::v1::Document& document);

// Applies body_reading_order to the document when produced_only_by says
// the whole body came from `options.geometry_collectors`; a document any
// other producer contributed to is left exactly as it is. Idempotent.
BodyOrderReport order_body_by_geometry(ai::pipestream::document::v1::Document* document,
                                       const BodyOrderOptions& options = {});

struct PictureAnchorReport {
  // Pictures placed among the page's items by their provenance.
  int anchored = 0;
  // Pictures with no usable provenance, kept at the end in a stable order.
  int appended = 0;
};

// Moves the named pictures (direct body children) to their provenance
// positions: on their page, right after the first item whose vertical
// extent overlaps the picture's (the paragraph a drawing sits inline in or
// beside), else before the first item of that page whose top edge is at or
// below the picture's, after the page's last item when none is, and where
// the page has no items at all, after the last item of any earlier page.
// Pictures without a page or box go to the end of the body.
// Both groups are processed in (page, top, left, self_ref) order, so the
// result is the same whatever order the detector reported them in.
PictureAnchorReport anchor_pictures_by_provenance(
    ai::pipestream::document::v1::Document* document,
    const std::vector<std::string>& picture_refs);

}  // namespace grparse
