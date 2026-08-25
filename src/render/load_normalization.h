// The load-time normalizations the reference model applies to a document
// before anything reads it, factored out so every export renderer
// (src/render/*.cpp) reproduces the same starting state. Not part of the
// public API; include/grparse/document_render.h stays the only public
// surface.
//
// Two mutations happen while the model loads a document:
//   * provenance bounding boxes are clamped to their page, and
//   * list items whose parent is not a list group are moved into a
//     synthesized list group (re-appended at the end of the text arena, with
//     every reference renumbered).
// The detection predicates let a renderer skip the defensive document copy
// when nothing would change; they are conservative, so a false positive only
// costs the copy while a false negative can never hide a required
// normalization.
#ifndef GRPARSE_RENDER_LOAD_NORMALIZATION_H
#define GRPARSE_RENDER_LOAD_NORMALIZATION_H

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// True when at least one provenance (or table cell) box falls outside its
// page and clamping would change the document.
bool needs_clamping(const ai::pipestream::document::v1::Document& doc);

// True when at least one list item is parented outside a list group and the
// migration pass would change the document.
bool has_misplaced_list_items(const ai::pipestream::document::v1::Document& doc);

// Clamps every provenance box (and single-page table cell box) to its page.
void clamp_document(ai::pipestream::document::v1::Document* doc);

// Moves runs of misplaced list items into synthesized list groups, deleting
// the originals and renumbering every reference, exactly like the model.
void migrate_misplaced_list_items(ai::pipestream::document::v1::Document* doc);

// True when any group still carries the ordered-list label, which the
// reference model rewrites to the plain list label at load.
bool has_ordered_list_groups(const ai::pipestream::document::v1::Document& doc);

// Rewrites every ordered-list group label to the plain list label, exactly
// like the model does at load; item numbering lives on the list items
// themselves. Must run before the misplaced-list migration so ordered-list
// groups are recognized as legitimate list parents.
void relabel_ordered_list_groups(ai::pipestream::document::v1::Document* doc);

}  // namespace grparse::render

#endif
