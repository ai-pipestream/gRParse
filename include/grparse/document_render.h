// Pure export renderers over the merged ai.pipestream.document.v1.Document.
// Each function folds the document's body reference tree (the same tree the
// collectors build and merge_documents keeps well formed) into one output
// string; none of them mutate the document or touch the service layer.
#ifndef GRPARSE_DOCUMENT_RENDER_H
#define GRPARSE_DOCUMENT_RENDER_H

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Renders the document as Markdown, byte for byte as the reference Markdown
// serializer does with its export defaults: "#" headings by level, paragraph
// blocks separated by blank lines, lists with four-space nesting and the
// reference's marker rules, pipe tables with a header rule and columns padded
// to a common width (a pipe inside a cell becomes a character reference),
// fenced code blocks with no info string, "$$...$$" formula blocks, captions
// and item metadata as plain paragraphs beside their item, and every picture
// as the "<!-- image -->" placeholder, whatever image it carries. Items with
// no Markdown counterpart degrade to the reference's comment placeholders
// instead of inventing syntax. Underscores and the HTML specials are escaped
// in item text; a link target is not.
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

// Renders the upstream-canonical JSON dialect of the document: the proto is
// walked directly into the flat, label-discriminated object model that
// dialect's own serializer produces (model-declaration key order, {"$ref"}
// references, [start, end] charspans, enum tags as canonical strings,
// exclude-none and empty-list suppression semantics, two-space indent,
// ASCII-escaped strings). The identity header always declares the dialect's
// schema name and version; collector attribution sources are omitted, and
// non-conforming custom meta field names move under the "pipestream"
// namespace. The dialect's load-time normalizations are applied first on a
// private copy: provenance boxes clamp to their page, and list items not
// parented to a list group move into a synthesized one (re-appended at the
// end of the text arena with every reference renumbered). Throws
// std::runtime_error on a texts entry whose variant is unset (dropping it
// would corrupt arena references).
std::string render_canonical_json(const ai::pipestream::document::v1::Document& document);

// Renders the document into the body shape of the Docs API "document"
// resource: a {"title", "body": {"content": [...]}, "lists",
// "inlineImagePlaceholders"} object whose structural elements are the
// paragraphs, bulleted paragraphs, and tables that API accepts. Pictures
// cannot carry bytes in a create body, so each one renders as a placeholder
// paragraph and is listed under "inlineImagePlaceholders" for the
// integration layer to upload and patch; the file header of
// src/render/gdocs_renderer.cpp states that contract in full. Output is
// deterministic: the same document always renders byte for byte the same.
std::string render_gdocs_json(const ai::pipestream::document::v1::Document& document);

// Renders docling's DocTags serialization: a "<doctag>" wrapper holding one
// line per body item, label-named tags ("<title>", "<section_header_level_N>",
// "<text>"...), "<unordered_list>"/"<ordered_list>" with "<list_item>"
// children, tables as "<otsl>" OTSL cell token streams
// (fcel/ecel/ched/rhed/srow/lcel/ucel/xcel/nl) with the caption nested inside,
// code with a "<_Language_>" token, and pictures carrying classification,
// SMILES, chart-table, and caption payloads. "<loc_N>" location tokens are
// emitted only for items whose provenance names a page with a known size;
// documents without provenance carry none, matching docling. Text is emitted
// raw (DocTags does not escape).
std::string render_doctags(const ai::pipestream::document::v1::Document& document);

// Renders the DocLang XML export: a `<doclang>` root in grpc-xml's
// NS_DOCLANG namespace whose element vocabulary is the one that service's
// dialect fold reads back (title, section-header with level, paragraph,
// list/list-item, caption, code, formula, footnote, reference, picture with
// a uri attribute, table/tr/th/td with rowspan and colspan). Content is
// XML-escaped; provenance is not emitted (the reader skips it anyway).
std::string render_doclang(const ai::pipestream::document::v1::Document& document);

// Renders the WebVTT export of track-timed text: a "WEBVTT" header (with the
// document title item's text when present), then one cue per body text item
// whose source list carries a TrackSource, in document order, as
// "HH:MM:SS.mmm --> HH:MM:SS.mmm" timings with the cue's identifier line and
// a "<v Voice>" span when the track names them. Consecutive items with the
// same identifier and timing merge into one multi-line cue. A document with
// no timed items renders the bare "WEBVTT" header, matching docling.
std::string render_vtt(const ai::pipestream::document::v1::Document& document);

// Renders docling's split-page HTML layout: the same skeleton and element
// vocabulary as render_html, but body content grouped per page inside a
// two-column table where each row holds the page image (or docling's
// "no page-image found" figure) beside a `<div class='page'>` of that page's
// elements. Items map to the page of their first provenance entry; items
// without provenance stay with the page in effect where they appear, and a
// document with no page provenance at all renders as one page.
std::string render_html_split_page(const ai::pipestream::document::v1::Document& document);

// Renders the document as block-style YAML with exactly the structure of
// render_json (proto field names preserved), by re-emitting that JSON
// through yaml-cpp. Throws std::runtime_error on emitter failure.
std::string render_yaml(const ai::pipestream::document::v1::Document& document);

}  // namespace grparse

#endif
