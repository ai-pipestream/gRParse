// Pure export renderers over the merged ai.pipestream.document.v1.Document.
// Each function folds the document's body reference tree (the same tree the
// collectors build and merge_documents keeps well formed) into one output
// string; none of them mutate the document or touch the service layer.
#ifndef GRPARSE_DOCUMENT_RENDER_H
#define GRPARSE_DOCUMENT_RENDER_H

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Renders the document as Markdown with docling-core's export_to_markdown
// semantics mapped onto this proto's item vocabulary: "#" headings by level,
// paragraph blocks separated by blank lines, "-"/"1." lists with four-space
// nesting, GitHub pipe tables with a header and alignment row (pipes inside
// cells escaped), fenced code blocks, "$$...$$" formula blocks, captions as
// emphasized lines adjacent to their table or figure, and images as
// "![alt](ref)" or docling's "<!-- image -->" placeholder when the picture
// carries no reference. Items with no Markdown counterpart degrade to
// docling's comment placeholders instead of inventing syntax.
std::string render_markdown(const ai::pipestream::document::v1::Document& document);

// Renders the document as structural HTML mirroring docling-core's HTML
// serializer: a "<!DOCTYPE html>" skeleton with a charset meta and the
// document name as its title (no CSS), then h1-h6, p, ul/ol/li,
// table/tbody/tr/th/td with rowspan/colspan, table captions as <caption>,
// pre/code, figure/figcaption/img, and formulas as plain <div> blocks (no
// MathML conversion). Text content is HTML-escaped; code and formula text
// too, since nothing downstream re-escapes it.
std::string render_html(const ai::pipestream::document::v1::Document& document);

// Renders the canonical protobuf JSON of the document via
// google::protobuf::util::MessageToJsonString with proto field names
// preserved, so the payload round-trips through JsonStringToMessage.
// Throws std::runtime_error if protobuf reports a conversion failure.
std::string render_json(const ai::pipestream::document::v1::Document& document);

}  // namespace grparse

#endif
