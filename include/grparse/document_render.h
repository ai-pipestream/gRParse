// Pure export renderers over the merged ai.pipestream.document.v1.Document.
// Each function folds the document's body reference tree (the same tree the
// collectors build and merge_documents keeps well formed) into one output
// string; none of them mutate the document or touch the service layer.
#ifndef GRPARSE_DOCUMENT_RENDER_H
#define GRPARSE_DOCUMENT_RENDER_H

#include <optional>
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
// as the "<!-- image -->" placeholder by default (image_export_mode can
// emit a Markdown image from the item's uri instead). Items with
// no Markdown counterpart degrade to the reference's comment placeholders
// instead of inventing syntax. Underscores and the HTML specials are escaped
// in item text; a link target is not.
std::string render_markdown(const ai::pipestream::document::v1::Document& document);

// The Markdown export parameters a request can move off the reference
// defaults (ConvertDocumentOptions.md_page_break_placeholder,
// md_compact_tables, and image_export_mode).
struct MarkdownOptions {
  // When set, a part carrying this text is emitted wherever the walk moves
  // from an item on one page to an item on a later page (the reference's
  // page_break_placeholder). Unset emits no page breaks.
  std::optional<std::string> page_break_placeholder;
  // Tables without column padding (the reference's compact_tables).
  bool compact_tables = false;
  // How picture ImageRefs appear in Markdown (Docling image_export_mode).
  // PLACEHOLDER (default) keeps "<!-- image -->"; EMBEDDED and REFERENCED
  // emit a Markdown image using the item's uri when present.
  enum class ImageExportMode { kPlaceholder, kEmbedded, kReferenced };
  ImageExportMode image_export_mode = ImageExportMode::kPlaceholder;
  // Caption order (docling-core CaptionPlacement). kStandard keeps each
  // item type's order. kLayout places a caption after the item when every
  // caption center is lower than the item in top-left page space, and keeps
  // the standard order when the position cannot be determined.
  enum class CaptionPlacement { kStandard, kLayout };
  CaptionPlacement caption_placement = CaptionPlacement::kStandard;
};

std::string render_markdown(const ai::pipestream::document::v1::Document& document,
                            const MarkdownOptions& options);

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
// a uri attribute, table/tr/th/td with rowspan and colspan). A caption
// carrying a hyperlink takes the block form with an `<href uri=...>` head,
// and a rich table cell (its ref naming a group) renders the group's blocks
// inside the cell element. Content is XML-escaped; provenance is not
// emitted (the reader skips it anyway). Uses DoclangOptions{}: the
// namespace is declared and pictures carry no uri (placeholder mode).
std::string render_doclang(const ai::pipestream::document::v1::Document& document);

// The DocLang export parameters a request can move off the defaults
// (ConvertDocumentOptions.image_export_mode and doclang_include_namespace),
// after docling-core's export_to_doclang / save_as_doclang_archive options.
struct DoclangOptions {
  // How a picture's image is carried.
  //   kPlaceholder: no uri on the picture element (and, in the archive, no
  //     assets/ member).
  //   kReferenced: render_doclang writes the picture's existing image uri;
  //     render_dclx stores the picture's image bytes (its own data-URI image,
  //     else a crop of its first provenance box from its page image) as an
  //     assets/ member and points the uri at it. A picture whose image uri is
  //     not a data URI keeps that uri, as docling does when it cannot load
  //     the image.
  //   kEmbedded: render_doclang writes the existing uri, else a PNG data URI
  //     cropped from the page image. render_dclx rejects it.
  enum class ImageMode { kPlaceholder, kEmbedded, kReferenced };
  // Unset takes the format's docling default: kPlaceholder for
  // render_doclang, kReferenced for render_dclx.
  std::optional<ImageMode> image_mode;
  // Declare the NS_DOCLANG namespace on the root. On by default, unlike
  // docling-core (off): the namespaced root is the strongest signal the
  // DocLang readers sniff. False writes a bare `<doclang>` root for byte
  // parity with docling's default output.
  bool include_namespace = true;
};

std::string render_doclang(const ai::pipestream::document::v1::Document& document,
                           const DoclangOptions& options);

// Renders the WebVTT export of track-timed text: a "WEBVTT" header (with the
// document title item's text when present), then one cue per body text item
// whose source list carries a TrackSource, in document order, as
// "HH:MM:SS.mmm --> HH:MM:SS.mmm" timings with the cue's identifier line and
// a "<v Voice>" span when the track names them. Consecutive items with the
// same identifier and timing merge into one multi-line cue. Cue text escapes
// & < > and collapses blank lines, so it cannot end its cue early. A document
// with no timed items renders the bare "WEBVTT" header, matching docling.
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
// through yaml-cpp. Every JSON string is written double-quoted, so text such
// as "2024", "true" or "off" stays a string under YAML 1.1 and 1.2 loaders.
// Throws std::runtime_error on emitter failure.
std::string render_yaml(const ai::pipestream::document::v1::Document& document);

// Renders docling-core's LaTeXDocSerializer output with its defaults: the
// article document class and the reference's package list as the preamble,
// the first title item hoisted into a "\title{...}" there with "\maketitle"
// opening the body, section headers as \section/\subsection/\subsubsection
// (levels outside [1, 3] clamp, where the reference raises), list groups as
// itemize/enumerate with a two-space indent per nesting level, code blocks as
// verbatim and inline code as \texttt, formulas as "$...$"/"$$...$$", tables
// as a "|l|...|l|" tabular with per-row \hline inside a table[h] float that
// carries the caption, and pictures as figure[h] floats holding the "% image"
// placeholder. A rich table cell (its ref naming a group) renders its blocks
// folded onto the cell line: a list keeps its itemize/enumerate environment,
// a nested table renders as the bare tabular, and a heading degrades to its
// text. Item text is LaTeX-escaped; code, formula, and URL arguments
// follow the reference's own escaping rules. Items with no LaTeX counterpart
// degrade to the reference's "% missing-..." comment placeholders instead of
// failing the export.
std::string render_latex(const ai::pipestream::document::v1::Document& document);

// Packs a DocLang OPC archive (`.dclx`) in the layout docling-core's
// save_as_doclang_archive writes: `[Content_Types].xml` and `_rels/.rels`
// (the OPC parts the doclang packager writes), root `document.xml` holding
// the DocLang export plus a final newline, every page image as
// `pages/<page_no>.<ext>`, and in kReferenced mode (the archive's default)
// every picture image as `assets/image_<NNNNNN>_<sha256>.<ext>`, numbered
// in body order, which the picture's uri names. Image bytes are kept as
// they are when they are PNG, JPEG or WebP (the types the content-types part
// declares) and re-encoded as PNG otherwise. Built in memory; members sort
// by path, so the same document always packs to the same bytes. Throws
// std::invalid_argument for kEmbedded. Readable by COLLECTOR_XML / grpc-xml.
std::string render_dclx(const ai::pipestream::document::v1::Document& document);

std::string render_dclx(const ai::pipestream::document::v1::Document& document,
                        const DoclangOptions& options);

}  // namespace grparse

#endif
