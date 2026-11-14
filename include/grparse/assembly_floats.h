#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/assembly_blocks.h"
#include "grparse/ocr_types.h"

namespace grparse {

// A table or picture emitted on the page, kept for the passes that link
// panels to their figures and captions to their floats.
struct EmittedFloat {
  const LayoutRegion* region;
  std::string self_ref;
  google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::RefItem>* captions;
  google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::RefItem>* children;
  ai::pipestream::document::v1::RefItem* parent;
};

// A caption-region item emitted on the page, not yet bound to a float.
struct EmittedCaption {
  const LayoutRegion* region;
  std::string self_ref;
  ai::pipestream::document::v1::TextItemBase* base;
};

// A panel label whose panel is emitted after it: it hangs off the figure
// (floats[figure]) until the panel exists, then moves.
struct PendingPanelCaption {
  const LayoutRegion* panel;
  size_t figure;
  std::string self_ref;
  ai::pipestream::document::v1::TextItemBase* base;
};

// The picture a picture-text block belongs to. A panel label ("(a) ...")
// set just under its panel, outside the panel's box but inside the figure,
// labels the panel directly above it; any other block keeps `region`. A
// text layer splits the label into word cells, so this is judged on the
// whole block's text and box.
const LayoutRegion* panel_label_owner(const OcrPage& page, const TextBlock& block,
                                      const LayoutRegion* region, const std::string& text);

// After a page's items are out: moves pending panel labels to their panels,
// hangs each picture inside another off the smallest picture around it, and
// binds captions to floats. Linked items leave `body_order`.
void link_floats(std::vector<EmittedFloat>& floats, const std::vector<EmittedCaption>& captions,
                 const std::vector<PendingPanelCaption>& pending_panel_captions, int page_height,
                 std::vector<std::string>* body_order);

}  // namespace grparse
