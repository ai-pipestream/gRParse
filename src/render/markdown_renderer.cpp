// The Markdown renderer behind render_markdown, the per-item emission half:
// what one text, table, picture or meta block reads as. The structural half
// (reference resolution, the exclusion sets, the pre-order walk, and the
// group, list and inline-group shapes) lives in markdown_walk.h, which
// reaches this class through the `serialize` seam. Semantics documented on
// the declaration in include/grparse/document_render.h.
//
// The walk is a port of the reference Markdown serializer: a pre-order pass
// over the body tree where each node is serialized once (a group consumes its
// subtree, everything else leaves its children to the pass), block results
// join with a blank line, and list groups join with a single newline plus a
// four-space indent per nesting level. The reference is driven with its own
// default parameters throughout:
//
//   body content layer only, the export label vocabulary, no page slicing,
//   formatting and hyperlinks on, caption delimiter " ", placeholder image
//   mode with "<!-- image -->", chart tables on, indent 4, no wrap width, no
//   page-break placeholder, HTML escaping on, underscore escaping on, meta
//   and annotation sections unmarked, automatic original-list-marker mode
//   with marker validation, fenced code blocks, padded (non-compact) tables,
//   picture classification included, pictures not traversed.
//
// Two upstream facts shape the port. First, the reference reaches a document
// through its model layer, which normalizes on load; render_markdown applies
// the shared load normalization (src/render/load_normalization.h) so it
// starts from the same tree. Only the list-item migration can change
// Markdown, so the box clamping is skipped. Second, the model layer treats
// a table's cell grid as derived from the flat cell list and rebuilds picture
// and table annotations from `meta`; the wire `grid` and `annotations`
// projections are therefore not read here.
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "grparse/document_render.h"
#include "load_normalization.h"
#include "markdown_text.h"
#include "markdown_walk.h"
#include "meta_repr.h"
#include "renderer_base.h"
#include "table_markdown.h"
#include "text_class.h"
#include "value_repr.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

constexpr std::string_view kImagePlaceholder = "<!-- image -->";
constexpr std::string_view kFormulaPlaceholder = "<!-- formula-not-decoded -->";
constexpr std::string_view kMissingKeyValue = "<!-- missing-key-value-item -->";
constexpr std::string_view kMissingForm = "<!-- missing-form-item -->";
constexpr std::string_view kCaptionDelim = " ";

class MarkdownRenderer : public MarkdownWalk {
 public:
  explicit MarkdownRenderer(const docv1::Document& document)
      : MarkdownWalk(document) {}

  std::string render() {
    std::vector<std::string> parts;
    consumed_.insert("#/body");
    parts.push_back(join(part_texts(get_parts("#/body", 0, false)), "\n\n"));
    if (document_.body().has_meta()) {
      parts.push_back(serialize_meta(document_.body().meta()));
    }
    return join(parts, "\n\n");
  }

 private:
  // How the table formatter resolves a cell that points at another item.
  CellTextResolver cell_resolver() {
    return [this](const std::string& ref) { return rich_cell_text(ref); };
  }

  // The first document item a part covers wins; a later one never overwrites
  // it, and a caller that asked for no span is left alone.
  static void claim_span(std::string* first_span, const std::string& span) {
    if (first_span != nullptr && first_span->empty()) *first_span = span;
  }

  std::string serialize(const std::string& ref, int list_level,
                        bool inline_scope, std::string* first_span) override {
    consumed_.insert(ref);
    if (const auto* group = group_at(ref)) {
      return serialize_group(ref, *group, list_level, inline_scope, first_span);
    }
    if (const auto* text = text_at(ref)) {
      return serialize_text_node(ref, *text, inline_scope, first_span);
    }
    if (const auto* table = table_at(ref)) {
      return serialize_table_node(ref, *table, first_span);
    }
    if (const auto* picture = picture_at(ref)) {
      return serialize_picture_node(ref, *picture, first_span);
    }
    return serialize_graph_node(ref, first_span);
  }

  std::string serialize_group(const std::string& ref, const docv1::GroupItem& group,
                              int list_level, bool inline_scope,
                              std::string* first_span) {
    std::vector<std::string> parts;
    // A group is not a document item, so it never covers a span itself; its
    // span is the first one its content covers.
    parts.push_back(
        serialize_group_content(ref, group, list_level, inline_scope, first_span));
    if (group.has_meta() && !excluded(ref)) {
      parts.push_back(serialize_meta(group.meta()));
    }
    return join(parts, "\n\n");
  }

  std::string serialize_text_node(const std::string& ref,
                                  const docv1::BaseTextItem& text,
                                  bool inline_scope, std::string* first_span) {
    // A caption or footnote renders through the item that claims it, meta
    // included.
    if (caption_refs_.contains(ref) || footnote_refs_.contains(ref)) {
      return std::string();
    }
    std::vector<std::string> parts;
    std::string body;
    if (!excluded(ref)) {
      std::string body_span;
      body = serialize_text(ref, text, inline_scope, &body_span);
      claim_span(first_span, body_span);
    }
    parts.push_back(std::move(body));
    const std::size_t before = parts.size();
    append_text_meta(text, ref, &parts);
    if (parts.size() > before && !parts.back().empty()) claim_span(first_span, ref);
    return join(parts, "\n\n");
  }

  std::string serialize_table_node(const std::string& ref,
                                   const docv1::TableItem& table,
                                   std::string* first_span) {
    std::vector<std::string> parts;
    std::string body_span;
    parts.push_back(serialize_table(ref, table, false, &body_span));
    claim_span(first_span, body_span);
    if (table.has_meta() && !excluded(ref)) {
      parts.push_back(serialize_meta_floating(table.meta()));
      if (!parts.back().empty()) claim_span(first_span, ref);
    }
    return join(parts, "\n\n");
  }

  std::string serialize_picture_node(const std::string& ref,
                                     const docv1::PictureItem& picture,
                                     std::string* first_span) {
    std::vector<std::string> parts;
    std::string body_span;
    parts.push_back(serialize_picture(ref, picture, &body_span));
    claim_span(first_span, body_span);
    if (picture.has_meta() && !excluded(ref)) {
      parts.push_back(serialize_picture_meta(picture.meta()));
      if (!parts.back().empty()) claim_span(first_span, ref);
    }
    return join(parts, "\n\n");
  }

  // The key-value and form arenas have no Markdown rendering of their own and
  // stand in with a placeholder; field regions and field items serialize to
  // nothing, their children being reached by the walk instead.
  std::string serialize_graph_node(const std::string& ref,
                                   std::string* first_span) {
    const ArenaRef parsed = parse_ref(ref);
    std::vector<std::string> parts;
    const docv1::FloatingMeta* meta = nullptr;
    if (parsed.kind == ArenaRef::kKeyValue &&
        parsed.index < document_.key_value_items_size()) {
      const auto& item = document_.key_value_items(parsed.index);
      if (excluded(ref)) return std::string();
      parts.emplace_back(kMissingKeyValue);
      if (item.has_meta()) meta = &item.meta();
    } else if (parsed.kind == ArenaRef::kForm &&
               parsed.index < document_.form_items_size()) {
      const auto& item = document_.form_items(parsed.index);
      if (excluded(ref)) return std::string();
      parts.emplace_back(kMissingForm);
      if (item.has_meta()) meta = &item.meta();
    } else {
      return std::string();
    }
    claim_span(first_span, ref);
    if (meta != nullptr) parts.push_back(serialize_meta_floating(*meta));
    return join(parts, "\n\n");
  }

  void append_text_meta(const docv1::BaseTextItem& text, const std::string& ref,
                        std::vector<std::string>* parts) {
    if (excluded(ref)) return;
    if (text.item_case() == docv1::BaseTextItem::kCode) {
      if (text.code().has_meta()) {
        parts->push_back(serialize_meta_floating(text.code().meta()));
      }
      return;
    }
    const auto* base = text_base(text);
    if (base != nullptr && base->has_meta()) {
      parts->push_back(serialize_meta(base->meta()));
    }
  }

  // -- text items -----------------------------------------------------------

  std::string post_process(const std::string& text, bool escape_html_chars,
                           bool escape_underscore_chars,
                           const docv1::Formatting* formatting,
                           const std::string* hyperlink) {
    std::string out = text;
    if (escape_underscore_chars) out = escape_underscores(out);
    if (escape_html_chars) out = escape_html_text(out);
    if (formatting != nullptr) {
      if (formatting->bold()) out = "**" + out + "**";
      if (formatting->italic()) out = "*" + out + "*";
      // Underline, subscript, and superscript have no Markdown rendering.
      if (formatting->strikethrough()) out = "~~" + out + "~~";
    }
    if (hyperlink != nullptr) out = "[" + out + "](" + normalized_uri(*hyperlink) + ")";
    return out;
  }

  // The item's own text, or the rendering of the single inline group that is
  // its whole content, with a checkbox label spelled out in front. `pending`
  // reports whether the post-processing still has to run over the result.
  std::string text_content(const std::string& ref, const docv1::TextItemBase& base,
                           bool* pending) {
    std::string text = base.text();
    *pending = true;
    // A text item whose only content is one inline group renders that group
    // and skips its own processing: the children carry the formatting.
    const auto children = children_of(ref);
    if (text.empty() && children.size() == 1 && is_inline_group(children.front())) {
      text = consume(children.front()) ? serialize(children.front(), 0, false, nullptr)
                                       : std::string();
      *pending = false;
    }
    if (base.label() == docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED) {
      text = "- [x] " + text;
    } else if (base.label() == docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED) {
      text = "- [ ] " + text;
    }
    return text;
  }

  // The title, section-header and list-item shapes: the line-break rule and
  // the post-processing run first, then the marker or the "#" run goes on.
  std::string headed_part(const std::string& ref, const docv1::BaseTextItem& item,
                          TextClass kind, std::string text, bool* pending,
                          const docv1::Formatting* formatting,
                          const std::string* hyperlink) {
    if (*pending) {
      text = kind == TextClass::kListItem ? md_line_breaks(text)
                                          : heading_line_breaks(text);
      text = post_process(text, true, true, formatting, hyperlink);
      *pending = false;
    }
    if (kind == TextClass::kListItem) {
      const std::string prefix = list_item_prefix(ref, item);
      return prefix.empty() ? text : prefix + " " + text;
    }
    if (kind == TextClass::kTitle) return "# " + text;
    const int level = std::max(item.section_header().level(), 1);
    return std::string(static_cast<std::size_t>(level) + 1, '#') + " " + text;
  }

  std::string serialize_text(const std::string& ref,
                             const docv1::BaseTextItem& item, bool inline_scope,
                             std::string* first_span) {
    const TextClass kind = classify_text(item);
    if (kind == TextClass::kCode) {
      return serialize_code(ref, item, inline_scope, first_span);
    }

    const auto* base = text_base(item);
    if (base == nullptr) return std::string();
    const docv1::Formatting* formatting =
        base->has_formatting() ? &base->formatting() : nullptr;
    const std::string* hyperlink =
        base->has_hyperlink() ? &base->hyperlink() : nullptr;

    bool processing_pending = true;
    const std::string text = text_content(ref, *base, &processing_pending);

    std::string text_part;
    bool escape_html_chars = true;
    bool escape_underscore_chars = true;
    if (kind == TextClass::kListItem || kind == TextClass::kTitle ||
        kind == TextClass::kSectionHeader) {
      text_part = headed_part(ref, item, kind, text, &processing_pending, formatting,
                              hyperlink);
    } else if (kind == TextClass::kFormula) {
      escape_html_chars = false;
      escape_underscore_chars = false;
      if (!text.empty()) {
        text_part = inline_scope ? "$" + text + "$" : "$$" + text + "$$";
      } else if (!base->orig().empty()) {
        text_part = std::string(kFormulaPlaceholder);
      }
    } else {
      text_part = md_line_breaks(text);
    }

    std::vector<std::string> res_parts;
    if (!text_part.empty()) {
      res_parts.push_back(text_part);
      *first_span = ref;
    }
    std::string out = join(res_parts, inline_scope ? " " : "\n\n");
    if (processing_pending) {
      out = post_process(out, escape_html_chars, escape_underscore_chars, formatting,
                         hyperlink);
    }
    return out;
  }

  // A code item reaches the renderer two ways: through its own variant, or
  // through the generic text arm carrying a code label, which the model
  // rebuilds into a code item with the base fields and no captions.
  std::string serialize_code(const std::string& ref, const docv1::BaseTextItem& item,
                             bool inline_scope, std::string* first_span) {
    const bool own_variant = item.item_case() == docv1::BaseTextItem::kCode;
    const auto* base = own_variant ? nullptr : text_base(item);
    if (!own_variant && base == nullptr) return std::string();
    const std::string& text = own_variant ? item.code().text() : base->text();
    const bool has_formatting =
        own_variant ? item.code().has_formatting() : base->has_formatting();
    const docv1::Formatting* formatting =
        !has_formatting            ? nullptr
        : own_variant              ? &item.code().formatting()
                                   : &base->formatting();
    const bool has_hyperlink =
        own_variant ? item.code().has_hyperlink() : base->has_hyperlink();
    const std::string* hyperlink =
        !has_hyperlink ? nullptr
        : own_variant  ? &item.code().hyperlink()
                       : &base->hyperlink();
    // Inline code, and any code carrying a hyperlink, uses single backticks;
    // everything else uses a plain fence with no info string.
    const bool backticks = inline_scope || hyperlink != nullptr;
    std::vector<std::string> res_parts;
    res_parts.push_back(backticks ? "`" + text + "`" : "```\n" + text + "\n```");
    *first_span = ref;
    if (own_variant) {
      const std::string captions = serialize_captions(item.code().captions());
      if (!captions.empty()) res_parts.push_back(captions);
    }
    return post_process(join(res_parts, inline_scope ? " " : "\n\n"), false, false,
                        formatting, hyperlink);
  }

  // -- captions -------------------------------------------------------------

  std::string serialize_captions(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions,
      std::string* first_span = nullptr) {
    std::vector<std::string> texts;
    for (const auto& ref : captions) {
      const auto* text = text_at(ref.ref());
      if (text == nullptr || excluded(ref.ref())) continue;
      if (first_span != nullptr && first_span->empty()) *first_span = ref.ref();
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        texts.push_back(text->code().text());
        continue;
      }
      const auto* base = text_base(*text);
      if (base != nullptr) texts.push_back(base->text());
    }
    std::string joined;
    for (std::size_t i = 0; i < texts.size(); ++i) {
      if (i != 0) joined.append(kCaptionDelim);
      joined.append(texts[i]);
    }
    return post_process(joined, true, true, nullptr, nullptr);
  }

  // -- tables ---------------------------------------------------------------

  std::string serialize_table(const std::string& ref, const docv1::TableItem& table,
                              bool nested_in_table,
                              std::string* first_span = nullptr) {
    if (nested_in_table) {
      mark_subtree_visited(ref);
      return collect_subtree_text(ref);
    }
    std::vector<std::string> parts;
    std::string caption_span;
    parts.push_back(serialize_captions(table.captions(), &caption_span));
    if (!parts.back().empty() && first_span != nullptr) *first_span = caption_span;
    if (!excluded(ref)) {
      parts.push_back(render::table_markdown(table.data(), cell_resolver()));
      if (!parts.back().empty() && first_span != nullptr && first_span->empty()) {
        *first_span = ref;
      }
    }
    return join(parts, "\n\n");
  }

  // A cell that points at another item renders that item, with a nested
  // table flattened to its text so the row stays intact.
  std::string rich_cell_text(const std::string& ref) {
    if (const auto* table = table_at(ref)) return serialize_table(ref, *table, true);
    consume(ref);
    return serialize(ref, 0, false, nullptr);
  }

  void mark_subtree_visited(const std::string& ref) {
    if (!consume(ref)) return;
    for (const auto& child : children_of(ref)) mark_subtree_visited(child);
  }

  std::string collect_subtree_text(const std::string& ref) const {
    std::vector<std::string> parts;
    if (const auto* table = table_at(ref)) {
      for (const auto& cell : table->data().table_cells()) {
        if (!cell.text().empty()) parts.push_back(cell.text());
      }
      return join(parts, " ");
    }
    if (const auto* text = text_at(ref)) {
      const auto* base = text_base(*text);
      const std::string& own =
          text->item_case() == docv1::BaseTextItem::kCode ? text->code().text()
          : base != nullptr                               ? base->text()
                                                          : std::string();
      if (!own.empty()) parts.push_back(own);
    }
    for (const auto& child : children_of(ref)) {
      const std::string child_text = collect_subtree_text(child);
      if (!child_text.empty()) parts.push_back(child_text);
    }
    return join(parts, " ");
  }

  // -- pictures -------------------------------------------------------------

  std::string serialize_picture(const std::string& ref,
                                const docv1::PictureItem& picture,
                                std::string* first_span) {
    std::vector<std::string> parts;
    std::string caption_span;
    parts.push_back(serialize_captions(picture.captions(), &caption_span));
    if (!parts.back().empty()) *first_span = caption_span;
    // The default image mode positions every picture with a placeholder,
    // whatever image the item carries.
    if (!excluded(ref)) {
      parts.emplace_back(kImagePlaceholder);
      if (first_span->empty()) *first_span = ref;
    }
    return join(parts, "\n\n");
  }

  // -- meta -----------------------------------------------------------------

  std::string serialize_meta(const docv1::BaseMeta& meta) {
    std::vector<std::string> parts;
    append_base_meta(meta, &parts);
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  std::string serialize_meta_floating(const docv1::FloatingMeta& meta) {
    std::vector<std::string> parts;
    append_inherited_meta(meta, &parts);
    if (meta.has_description() && !meta.description().text().empty()) {
      parts.push_back(meta.description().text());
    }
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  std::string serialize_picture_meta(const docv1::PictureMeta& meta) {
    std::vector<std::string> parts;
    append_inherited_meta(meta, &parts);
    if (meta.has_description() && !meta.description().text().empty()) {
      parts.push_back(meta.description().text());
    }
    if (meta.has_classification()) {
      const std::string main = main_classification(meta.classification());
      if (!main.empty()) parts.push_back(humanized(main));
    }
    if (meta.has_molecule() && !meta.molecule().smi().empty()) {
      parts.push_back(meta.molecule().smi());
    }
    if (meta.has_tabular_chart()) {
      const std::string table = stripped(
          render::table_markdown(meta.tabular_chart().chart_data(), cell_resolver()));
      if (!table.empty()) parts.push_back(table);
    }
    if (meta.has_code()) parts.push_back(code_meta_repr(meta.code()));
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  // The declaration order the reference iterates: the inherited fields
  // first, then the shape's own, then the custom part.
  template <typename Meta>
  void append_inherited_meta(const Meta& meta, std::vector<std::string>* parts) {
    if (meta.has_summary() && !meta.summary().text().empty()) {
      parts->push_back(meta.summary().text());
    }
    if (meta.has_language()) parts->push_back(language_meta_repr(meta.language()));
    if (meta.has_entities()) parts->push_back(entities_meta_repr(meta.entities()));
    if (meta.has_keywords()) {
      parts->push_back(join_values(meta.keywords().values()));
    }
    if (meta.has_topics()) {
      parts->push_back(join_values(meta.topics().values()));
    }
  }

  void append_base_meta(const docv1::BaseMeta& meta, std::vector<std::string>* parts) {
    append_inherited_meta(meta, parts);
  }

  // The model's keyword and topic lists are unique lists: a repeat drops on
  // load, keeping the first occurrence.
  static std::string join_values(
      const google::protobuf::RepeatedPtrField<std::string>& values) {
    std::string out;
    std::set<std::string_view> seen;
    for (const auto& value : values) {
      if (!seen.insert(value).second) continue;
      if (!out.empty()) out.append(", ");
      out.append(value);
    }
    return out;
  }

  static std::string main_classification(
      const docv1::PictureClassificationMetaField& classification) {
    const docv1::PictureClassificationPrediction* best = nullptr;
    double best_confidence = 0.0;
    for (const auto& prediction : classification.predictions()) {
      if (!prediction.has_confidence()) continue;
      if (best == nullptr || prediction.confidence() > best_confidence) {
        best = &prediction;
        best_confidence = prediction.confidence();
      }
    }
    if (best == nullptr && !classification.predictions().empty()) {
      best = &classification.predictions(0);
    }
    return best != nullptr ? best->class_name() : std::string();
  }

  // The custom part of a meta block, in the shared export order. Only the
  // value renders; the name is what fixes the order.
  void append_custom_fields(
      const google::protobuf::Map<std::string, google::protobuf::Value>& fields,
      std::vector<std::string>* parts) {
    for (const auto& [key, value] : ordered_custom_fields(fields)) {
      if (!value_is_truthy(*value)) continue;
      parts->push_back(value_str(*value));
    }
  }
};

}  // namespace

std::string render_markdown(const docv1::Document& document) {
  // Only the list-item migration can change the Markdown; the box clamping
  // the model also applies on load is invisible here, so the defensive copy
  // is taken only when a list item actually needs re-homing.
  if (!render::has_misplaced_list_items(document) &&
      !render::has_ordered_list_groups(document)) {
    return MarkdownRenderer(document).render();
  }
  docv1::Document normalized = document;
  // Ordered-list groups relabel to plain list groups first, exactly like the
  // reference load; only then can the migration see them as list parents.
  render::relabel_ordered_list_groups(&normalized);
  render::migrate_misplaced_list_items(&normalized);
  return MarkdownRenderer(normalized).render();
}

}  // namespace grparse
