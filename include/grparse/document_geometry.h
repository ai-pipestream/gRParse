#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Geometry of a finished Document, read the same way by every pass that
// runs after the merge (repair, body order, heading hierarchy, picture
// anchoring), so an item can never sit on one page for one pass and on
// another for the next.

// The base of a text item that has one; CodeItem inlines its fields and
// has no base.
const ai::pipestream::document::v1::TextItemBase* text_base_of(
    const ai::pipestream::document::v1::BaseTextItem& item);
ai::pipestream::document::v1::TextItemBase* mutable_text_base_of(
    ai::pipestream::document::v1::BaseTextItem* item);

// The lowest page number an item's provenance names; zero when it names
// none.
int first_page_of(
    const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::ProvenanceItem>& prov);

// Page heights by page number: the page map's own size where it states
// one, otherwise the furthest box edge any item reaches on that page.
std::map<int, double> document_page_heights(const ai::pipestream::document::v1::Document& document);

// An axis-aligned box measured downward from its page's top edge, whichever
// origin the provenance box states; a bottom-left box measures its edges
// upward from the page's bottom, so the page height is what flips it.
struct TopDownBox {
  double left = 0;
  double top = 0;
  double right = 0;
  double bottom = 0;
  double height() const { return bottom - top; }
  double width() const { return right - left; }
};

TopDownBox top_down_box(const ai::pipestream::document::v1::BoundingBox& box, double page_height);

// Where an arena item sits: its first page and the union of its boxes on
// that page, top-down. A group's placement is the union of its children's.
// Absent when the item names no page, has no box on it, or the page's
// height is unknown; a box of zero area does not count.
struct ItemPlacement {
  int page = 0;
  TopDownBox box;
};

std::optional<ItemPlacement> item_placement(const ai::pipestream::document::v1::Document& document,
                                            std::string_view ref,
                                            const std::map<int, double>& page_heights);

// The same placement straight from a provenance list, for an item in hand.
std::optional<ItemPlacement> provenance_placement(
    const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::ProvenanceItem>& prov,
    const std::map<int, double>& page_heights);

// The item label a body reference names: a text item's label, TABLE,
// PICTURE, or UNSPECIFIED for a group or an unresolvable reference.
ai::pipestream::document::v1::DocItemLabel item_label(
    const ai::pipestream::document::v1::Document& document, std::string_view ref);

// True when every collector any arena item names is in `collectors` and at
// least one item names one: the whole body came from producers whose
// order and heading depths are guesses from geometry rather than document
// structure, so a document-wide pass over the geometry may overrule them.
bool produced_only_by(const ai::pipestream::document::v1::Document& document,
                      const std::vector<std::string>& collectors);

}  // namespace grparse
