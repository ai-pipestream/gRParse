// Enum tag to canonical-string tables for the canonical JSON dialect. An
// unknown tag falls back to the model default the import path would produce,
// never to zero-value pollution. Internal to the export renderers;
// include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_CANONICAL_JSON_LABELS_H
#define GRPARSE_RENDER_CANONICAL_JSON_LABELS_H

#include <optional>
#include <string_view>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

std::string_view content_layer_string(
    ai::pipestream::document::v1::ContentLayer layer);

// Empty for a label the dialect has no name for; the caller decides what an
// unnamed label means for the member it was going to write.
std::optional<std::string_view> doc_item_label_string(
    ai::pipestream::document::v1::DocItemLabel label);

std::string_view group_label_string(
    ai::pipestream::document::v1::GroupLabel label);

std::string_view script_string(ai::pipestream::document::v1::Script script);

std::string_view coord_origin_string(
    const ai::pipestream::document::v1::BoundingBox& bbox);

std::string_view orientation_string(
    ai::pipestream::document::v1::Orientation orientation);

std::string_view graph_cell_label_string(
    ai::pipestream::document::v1::GraphCellLabel label);

std::string_view graph_link_label_string(
    ai::pipestream::document::v1::GraphLinkLabel label);

// The exported code language: a recognized tag maps directly; anything else
// (unset, unspecified, an unknown tag, or a raw-only fallback) collapses to
// the vocabulary's catch-all "unknown", which is also the model default.
std::string_view exported_code_language(
    ai::pipestream::document::v1::CodeLanguageLabel tag);

}  // namespace grparse::render

#endif
