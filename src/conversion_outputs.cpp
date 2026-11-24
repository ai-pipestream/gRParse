#include "conversion_outputs.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "grparse/document_render.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// The document's plain text export: its text items in arena order, which is
// each collector's emission order.
std::string document_plain_text(const pipestream::document::v1::Document& document) {
  std::string text;
  for (const auto& item : document.texts()) {
    const pipestream::document::v1::TextItemBase* base = nullptr;
    switch (item.item_case()) {
      case pipestream::document::v1::BaseTextItem::kTitle: base = &item.title().base(); break;
      case pipestream::document::v1::BaseTextItem::kSectionHeader:
        base = &item.section_header().base();
        break;
      case pipestream::document::v1::BaseTextItem::kListItem:
        base = &item.list_item().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFormula: base = &item.formula().base(); break;
      case pipestream::document::v1::BaseTextItem::kText: base = &item.text().base(); break;
      case pipestream::document::v1::BaseTextItem::kCode:
        // CodeItem carries its fields inline instead of a nested base.
        if (!text.empty()) text.push_back('\n');
        text.append(item.code().text());
        break;
      case pipestream::document::v1::BaseTextItem::kFieldHeading:
        base = &item.field_heading().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFieldValue:
        base = &item.field_value().base();
        break;
      case pipestream::document::v1::BaseTextItem::ITEM_NOT_SET: break;
    }
    if (base == nullptr) continue;
    if (!text.empty()) text.push_back('\n');
    text.append(base->text());
  }
  return text;
}

// True when the response must carry this output format. An empty to_formats
// keeps the historical default of the plain-text export alone; every other
// format is opt-in by explicit request.
bool requested(const pipestream::parse::v1::ConvertDocumentOptions& options,
               pipestream::parse::v1::OutputFormat format) {
  if (options.to_formats().empty()) {
    return format == pipestream::parse::v1::OUTPUT_FORMAT_TEXT;
  }
  return std::ranges::find(options.to_formats(), format) !=
         options.to_formats().end();
}

}  // namespace

void render_exports(const pipestream::parse::v1::ConvertDocumentOptions& options,
                    const pipestream::document::v1::Document& document,
                    pipestream::parse::v1::DocumentExports* exports) {
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_TEXT)) {
    exports->set_text(document_plain_text(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN)) {
    MarkdownOptions markdown;
    if (options.has_md_page_break_placeholder()) {
      markdown.page_break_placeholder = options.md_page_break_placeholder();
    }
    markdown.compact_tables = options.md_compact_tables();
    if (options.has_caption_placement()) {
      switch (options.caption_placement()) {
        case pipestream::parse::v1::CAPTION_PLACEMENT_LAYOUT:
          markdown.caption_placement = MarkdownOptions::CaptionPlacement::kLayout;
          break;
        case pipestream::parse::v1::CAPTION_PLACEMENT_STANDARD:
          markdown.caption_placement = MarkdownOptions::CaptionPlacement::kStandard;
          break;
        default:
          throw std::invalid_argument("caption_placement is unspecified or unknown");
      }
    }
    if (options.has_image_export_mode()) {
      switch (options.image_export_mode()) {
        case pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kEmbedded;
          break;
        case pipestream::parse::v1::IMAGE_REF_MODE_REFERENCED:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kReferenced;
          break;
        case pipestream::parse::v1::IMAGE_REF_MODE_PLACEHOLDER:
        case pipestream::parse::v1::IMAGE_REF_MODE_UNSPECIFIED:
        default:
          markdown.image_export_mode = MarkdownOptions::ImageExportMode::kPlaceholder;
          break;
      }
    }
    if (options.has_formatting_fallback_mode() &&
        options.formatting_fallback_mode() ==
            pipestream::parse::v1::FORMATTING_FALLBACK_MODE_HTML) {
      markdown.html_formatting_fallback = true;
    }
    exports->set_md(render_markdown(document, markdown));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML)) {
    exports->set_html(render_html(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_JSON)) {
    exports->set_json(render_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_CANONICAL_JSON)) {
    exports->set_canonical_json(render_canonical_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_GDOCS_JSON)) {
    exports->set_gdocs_json(render_gdocs_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS)) {
    exports->set_doctags(render_doctags(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG)) {
    exports->set_doclang(render_doclang(document, doclang_options(options)));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_VTT)) {
    exports->set_vtt(render_vtt(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE)) {
    exports->set_html_split_page(render_html_split_page(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_YAML)) {
    exports->set_yaml(render_yaml(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_LATEX)) {
    exports->set_latex(render_latex(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DCLX)) {
    exports->set_dclx(render_dclx(document, doclang_options(options)));
  }
}

grpc::Status render_chunks(
    const pipestream::parse::v1::ConvertDocumentOptions& options,
    const pipestream::document::v1::Document& document, const chunking::OffsetTable& offsets,
    std::string_view filename,
    google::protobuf::RepeatedPtrField<pipestream::parse::v1::Chunk>* chunks) {
  if (std::ranges::find(options.to_formats(), pipestream::parse::v1::OUTPUT_FORMAT_CHUNKS) ==
      options.to_formats().end()) {
    return grpc::Status::OK;
  }
  std::vector<pipestream::parse::v1::Chunk> rendered;
  if (options.chunking_options_case() ==
      pipestream::parse::v1::ConvertDocumentOptions::kHybridChunking) {
    const grpc::Status chunked =
        chunking::chunk_hybrid(document, offsets, options.hybrid_chunking(), filename, &rendered);
    if (!chunked.ok()) return chunked;
  } else {
    // hierarchical_chunking, chunking_preset, or CHUNKS with no config:
    // hierarchical defaults (validate_options admits no other preset).
    pipestream::parse::v1::HierarchicalChunkerOptions hierarchical;
    if (options.chunking_options_case() ==
        pipestream::parse::v1::ConvertDocumentOptions::kHierarchicalChunking) {
      hierarchical = options.hierarchical_chunking();
    }
    rendered = chunking::chunk_hierarchical(document, offsets,
                                            chunking::chunk_options_from(hierarchical), filename);
  }
  for (auto& chunk : rendered) *chunks->Add() = std::move(chunk);
  return grpc::Status::OK;
}

}  // namespace grparse
