#pragma once

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/ocr_types.h"
#include "grparse/text_geometry.h"

namespace grparse {

// Writes a page-space box as a top-left-origin BoundingBox.
void set_bounding_box(const AxisAlignedBox& box, ai::pipestream::document::v1::BoundingBox* output);

// Fills a table item's cells from the page lines bound to the region:
// model-recognized cells (spans, header rows) when the region carries
// structured cells, the geometry grid otherwise.
void fill_table_data(const OcrPage& page, const LayoutRegion& region,
                     ai::pipestream::document::v1::TableData* data);

}  // namespace grparse
