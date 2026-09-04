#pragma once

#include <cstddef>
#include <vector>

#include "grparse/ocr_types.h"

namespace grparse {

// One axis-aligned box in any top-down coordinate space (page pixels, PDF
// points measured from the top edge, twips): the only geometry the XY-cut
// reads.
struct OrderBox {
  double left = 0;
  double top = 0;
  double right = 0;
  double bottom = 0;
};

// How the cut chooses between a horizontal band gap and a vertical column
// gutter when a block offers both. The defaults are the line-level rule:
// the widest gap wins, a tie going to the band.
struct CutPolicy {
  // A band cut wins when its gap is at least this many times the gutter's
  // width. Above 1 favours columns: paragraph spacing inside a column can
  // exceed the gutter, and only a much wider band (a title block above
  // two-column text, an abstract band) should split rows first.
  double band_over_gutter = 1.0;
  // A vertical gap is a gutter only when the boxes on each side of it span
  // at least this share of the block's height; a label beside one row of a
  // taller block does not make a column. Zero accepts any gap.
  double gutter_side_share = 0.0;
};

// Returns the indices of `boxes` in reading order by recursive XY-cut: a set
// of boxes splits at the widest whitespace gap no box crosses on either
// axis, the policy above arbitrating between a horizontal band cut
// (top-to-bottom) and a vertical one (left-to-right, which is what keeps
// multi-column text in column order); a set with no clean gap sorts by
// top, then left, keeping input order for exact ties. Deterministic in the
// boxes and the policy alone.
std::vector<size_t> xy_cut_order(const std::vector<OrderBox>& boxes, const CutPolicy& policy = {});

// Returns the indices of page.lines in reading order.
//
// Ordering is the XY-cut above over layout units: each text-carrying layout
// region is one unit holding the lines whose box centers it contains, and
// every line outside any region is its own unit. Lines inside one unit read
// top-to-bottom, left-to-right.
//
// Deterministic: the result depends only on line and region geometry.  With
// no regions the lines themselves still XY-cut, so a clean two-column page
// orders correctly even without a layout model.
//
// When trust_source_order is set (the caller asserts the lines' emission
// order already is the reading order; the consensus page source marks such
// pages with OcrPage::source_order_trusted), the cut is skipped instead:
// units order by their first line's emission index and lines within a unit
// by emission index. Unit construction, including region binding, is
// identical either way; only the sort key changes.
std::vector<size_t> reading_order(const OcrPage& page, bool trust_source_order = false);

}  // namespace grparse
