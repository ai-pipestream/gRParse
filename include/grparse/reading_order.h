#pragma once

#include <cstddef>
#include <vector>

#include "grparse/ocr_types.h"

namespace grparse {

// Returns the indices of page.lines in reading order.
//
// Ordering is a recursive XY-cut over layout units: each text-carrying layout
// region is one unit holding the lines whose box centers it contains, and
// every line outside any region is its own unit.  A set of units splits first
// on horizontal whitespace bands (top-to-bottom), then on vertical ones
// (left-to-right, which is what keeps multi-column text in column order), and
// falls back to top/left sorting when no clean gap exists.  Lines inside one
// unit read top-to-bottom, left-to-right.
//
// Deterministic: the result depends only on line and region geometry.  With
// no regions the lines themselves still XY-cut, so a clean two-column page
// orders correctly even without a layout model.
std::vector<size_t> reading_order(const OcrPage& page);

}  // namespace grparse
