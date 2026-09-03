// The LaTeX renderer behind render_latex; semantics documented on the
// declaration in include/grparse/document_render.h.
//
// A port of docling-core's LaTeXDocSerializer driven with its own defaults:
// body content layer only, the export label vocabulary, placeholder image
// mode with "% image", picture annotations and chart tables on, a two-space
// list indent per level, no page-break command, LaTeX escaping on, and the
// article document class with the reference's default package list. The walk
// mirrors the reference's structure: a pre-order pass over the body tree
// where each node serializes once (list and inline groups consume their
// subtrees, every other group is a transparent container), block parts join
// with a blank line, and the document assembles around the fixed preamble
// with the title hoisted into it.
//
// Two deliberate deviations, both where the reference raises and an export
// must not: a section header level outside [1, 3] clamps to the nearest
// supported command instead of failing the export, and a table cell offset
// past the grid edge caps instead of raising (the shared derived-grid rule
// in renderer_base).
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grparse/document_render.h"
#include "load_normalization.h"
#include "renderer_base.h"
#include "text_class.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

constexpr std::string_view kImagePlaceholder = "% image";
constexpr std::string_view kFormulaPlaceholder = "% formula-not-decoded";
constexpr std::string_view kMissingKeyValue = "% missing-key-value-item";
constexpr std::string_view kMissingForm = "% missing-form-item";
constexpr int kListIndent = 2;

// docling's _escape_latex verbatim: a per-character map. Never applied inside
// math or verbatim contexts, exactly like the reference.
std::string escape_latex(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '\\': out.append("\\textbackslash{}"); break;
      case '{': out.append("\\{"); break;
      case '}': out.append("\\}"); break;
      case '#': out.append("\\#"); break;
      case '$': out.append("\\$"); break;
      case '%': out.append("\\%"); break;
      case '&': out.append("\\&"); break;
      case '_': out.append("\\_"); break;
      case '~': out.append("\\textasciitilde{}"); break;
      case '^': out.append("\\textasciicircum{}"); break;
      default: out.push_back(c);
    }
  }
  return out;
}

// The reference joins drop empty parts, so this one does too.
std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    if (!out.empty()) out.append(sep);
    out.append(part);
  }
  return out;
}

// The reference's post_process: escape first, then the formatting wrappers
// (bold innermost, the scripts outermost), then the hyperlink.
std::string post_process(const std::string& text, const docv1::Formatting* formatting,
                         const std::string* hyperlink) {
  std::string out = escape_latex(text);
  if (formatting != nullptr) {
    if (formatting->bold()) out = "\\textbf{" + out + "}";
    if (formatting->italic()) out = "\\textit{" + out + "}";
    if (formatting->underline()) out = "\\underline{" + out + "}";
    if (formatting->strikethrough()) out = "\\sout{" + out + "}";
    if (formatting->script() == docv1::SCRIPT_SUB) {
      out = "$_{" + out + "}$";
    } else if (formatting->script() == docv1::SCRIPT_SUPER) {
      out = "$^{" + out + "}$";
    }
  }
  if (hyperlink != nullptr) {
    // The model layer parses the hyperlink into a URL type whose serializer
    // normalizes it; the escape is the reference's own for the URL argument.
    out = "\\href{" + escape_latex(normalized_uri(*hyperlink)) + "}{" + out + "}";
  }
  return out;
}

class LatexRenderer : RendererBase {
 public:
  explicit LatexRenderer(const docv1::Document& document) : RendererBase(document) {
    collect_reference_sets();
  }

  std::string render() {
    std::vector<std::string> refs;
    std::set<std::string> seen;
    collect_walk("#/body", &seen, &refs);
    // The root itself is walk state, never a part.
    consumed_.insert("#/body");
    std::vector<std::string> parts;
    for (const auto& ref : refs) {
      if (!consume(ref)) continue;
      std::string part = serialize(ref, 0, false);
      if (!part.empty()) parts.push_back(std::move(part));
    }
    return assemble(join(parts, "\n\n"));
  }

 private:
  // -- reference resolution -------------------------------------------------

  const docv1::GroupItem* group_at(const std::string& ref) const {
    if (ref == "#/body") return &document_.body();
    if (ref == "#/furniture") return &document_.furniture();
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kGroup && parsed.index < document_.groups_size()) {
      return &document_.groups(parsed.index);
    }
    return nullptr;
  }

  const docv1::BaseTextItem* text_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kText && parsed.index < document_.texts_size()) {
      return &document_.texts(parsed.index);
    }
    return nullptr;
  }

  const docv1::TableItem* table_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kTable && parsed.index < document_.tables_size()) {
      return &document_.tables(parsed.index);
    }
    return nullptr;
  }

  const docv1::PictureItem* picture_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kPicture && parsed.index < document_.pictures_size()) {
      return &document_.pictures(parsed.index);
    }
    return nullptr;
  }

  // The child references of any node, in document order.
  std::vector<std::string> children_of(const std::string& ref) const {
    std::vector<std::string> refs;
    const auto collect = [&refs](const auto& children) {
      for (const auto& child : children) refs.push_back(child.ref());
    };
    if (const auto* group = group_at(ref)) {
      collect(group->children());
      return refs;
    }
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        collect(text->code().children());
      } else if (const auto* base = text_base(*text)) {
        collect(base->children());
      }
      return refs;
    }
    if (const auto* table = table_at(ref)) {
      collect(table->children());
      return refs;
    }
    if (const auto* picture = picture_at(ref)) {
      collect(picture->children());
      return refs;
    }
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        if (parsed.index < document_.key_value_items_size()) {
          collect(document_.key_value_items(parsed.index).children());
        }
        break;
      case ArenaRef::kForm:
        if (parsed.index < document_.form_items_size()) {
          collect(document_.form_items(parsed.index).children());
        }
        break;
      case ArenaRef::kFieldRegion:
        if (parsed.index < document_.field_regions_size()) {
          collect(document_.field_regions(parsed.index).children());
        }
        break;
      case ArenaRef::kFieldItem:
        if (parsed.index < document_.field_items_size()) {
          collect(document_.field_items(parsed.index).children());
        }
        break;
      default: break;
    }
    return refs;
  }

  docv1::ContentLayer layer_of(const std::string& ref) const {
    if (const auto* group = group_at(ref)) return group->content_layer();
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        return text->code().content_layer();
      }
      if (const auto* base = text_base(*text)) return base->content_layer();
      return docv1::CONTENT_LAYER_UNSPECIFIED;
    }
    if (const auto* table = table_at(ref)) return table->content_layer();
    if (const auto* picture = picture_at(ref)) return picture->content_layer();
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        if (parsed.index < document_.key_value_items_size()) {
          return document_.key_value_items(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kForm:
        if (parsed.index < document_.form_items_size()) {
          return document_.form_items(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kFieldRegion:
        if (parsed.index < document_.field_regions_size()) {
          return document_.field_regions(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kFieldItem:
        if (parsed.index < document_.field_items_size()) {
          return document_.field_items(parsed.index).content_layer();
        }
        break;
      default: break;
    }
    return docv1::CONTENT_LAYER_UNSPECIFIED;
  }

  // The label the model reconstructs for one node, or unspecified for a node
  // that is not a document item (groups are never excluded).
  docv1::DocItemLabel label_of(const std::string& ref) const {
    if (const auto* text = text_at(ref)) {
      switch (classify_text(*text)) {
        case TextClass::kTitle: return docv1::DOC_ITEM_LABEL_TITLE;
        case TextClass::kSectionHeader: return docv1::DOC_ITEM_LABEL_SECTION_HEADER;
        case TextClass::kListItem: return docv1::DOC_ITEM_LABEL_LIST_ITEM;
        case TextClass::kCode: return docv1::DOC_ITEM_LABEL_CODE;
        case TextClass::kFormula: return docv1::DOC_ITEM_LABEL_FORMULA;
        case TextClass::kFieldHeading: return docv1::DOC_ITEM_LABEL_FIELD_HEADING;
        case TextClass::kFieldValue: return docv1::DOC_ITEM_LABEL_FIELD_VALUE;
        case TextClass::kText: break;
      }
      const auto* base = text_base(*text);
      return effective_label(base != nullptr ? base->label() : docv1::DOC_ITEM_LABEL_TEXT,
                             docv1::DOC_ITEM_LABEL_TEXT);
    }
    if (const auto* table = table_at(ref)) {
      return effective_label(table->label(), docv1::DOC_ITEM_LABEL_TABLE);
    }
    if (const auto* picture = picture_at(ref)) {
      return effective_label(picture->label(), docv1::DOC_ITEM_LABEL_PICTURE);
    }
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        return parsed.index < document_.key_value_items_size()
                   ? effective_label(document_.key_value_items(parsed.index).label(),
                                     docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION)
                   : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
      case ArenaRef::kForm:
        return parsed.index < document_.form_items_size()
                   ? effective_label(document_.form_items(parsed.index).label(),
                                     docv1::DOC_ITEM_LABEL_FORM)
                   : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
      case ArenaRef::kFieldRegion: return docv1::DOC_ITEM_LABEL_FIELD_REGION;
      case ArenaRef::kFieldItem: return docv1::DOC_ITEM_LABEL_FIELD_ITEM;
      default: return docv1::DOC_ITEM_LABEL_UNSPECIFIED;
    }
  }

  // -- the reference sets -----------------------------------------------------

  // The caption references an item this export renders them through (tables,
  // pictures, code items), and the body-layer items the export label
  // vocabulary drops. A caption linked into the tree twice renders with its
  // owner only.
  void collect_reference_sets() {
    const auto claim = [this](const auto& captions) {
      for (const auto& ref : captions) caption_refs_.insert(ref.ref());
    };
    for (const auto& table : document_.tables()) claim(table.captions());
    for (const auto& picture : document_.pictures()) claim(picture.captions());
    for (const auto& item : document_.texts()) {
      if (item.item_case() == docv1::BaseTextItem::kCode) claim(item.code().captions());
    }

    std::set<std::string> seen;
    walk_for_exclusions("#/body", &seen);
  }

  void walk_for_exclusions(const std::string& ref, std::set<std::string>* seen) {
    if (!seen->insert(ref).second) return;
    const docv1::ContentLayer layer = layer_of(ref);
    if (layer == docv1::CONTENT_LAYER_BODY ||
        layer == docv1::CONTENT_LAYER_UNSPECIFIED) {
      if (group_at(ref) == nullptr && parse_ref(ref).kind != ArenaRef::kUnknown &&
          !exported_label(label_of(ref))) {
        excluded_refs_.insert(ref);
      }
    }
    for (const auto& child : children_of(ref)) walk_for_exclusions(child, seen);
  }

  // -- the walk ---------------------------------------------------------------

  // Pre-order references under `root`, yielding only body-layer nodes but
  // descending regardless, and stopping at a picture's children unless they
  // are that picture's own captions.
  void collect_walk(const std::string& ref, std::set<std::string>* seen,
                    std::vector<std::string>* out) const {
    if (!seen->insert(ref).second) return;
    const docv1::ContentLayer layer = layer_of(ref);
    if (layer == docv1::CONTENT_LAYER_BODY ||
        layer == docv1::CONTENT_LAYER_UNSPECIFIED) {
      out->push_back(ref);
    }
    const ArenaRef parsed = parse_ref(ref);
    std::set<std::string> allowed;
    if (const auto* picture = picture_at(ref)) {
      for (const auto& caption : picture->captions()) allowed.insert(caption.ref());
    }
    for (const auto& child : children_of(ref)) {
      if (parsed.kind == ArenaRef::kPicture && !allowed.contains(child)) continue;
      collect_walk(child, seen, out);
    }
  }

  // -- dispatch -----------------------------------------------------------------

  std::string serialize(const std::string& ref, int list_level, bool inline_scope) {
    consume(ref);
    if (excluded_refs_.contains(ref)) return std::string();
    if (const auto* group = group_at(ref)) {
      return serialize_group(*group, list_level, inline_scope);
    }
    if (const auto* text = text_at(ref)) return serialize_text(ref, *text, inline_scope);
    if (const auto* table = table_at(ref)) return serialize_table(*table);
    if (const auto* picture = picture_at(ref)) return serialize_picture(*picture);
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kKeyValue &&
        parsed.index < document_.key_value_items_size()) {
      return std::string(kMissingKeyValue);
    }
    if (parsed.kind == ArenaRef::kForm && parsed.index < document_.form_items_size()) {
      return std::string(kMissingForm);
    }
    // Field regions and items stay transparent: the walk already collected
    // their children, which render as parts of their own.
    return std::string();
  }

  static bool is_list_group(const docv1::GroupItem& group) {
    return group.label() == docv1::GROUP_LABEL_LIST ||
           group.label() == docv1::GROUP_LABEL_ORDERED_LIST;
  }

  std::string serialize_group(const docv1::GroupItem& group, int list_level,
                              bool inline_scope) {
    if (is_list_group(group)) return serialize_list(group, list_level, inline_scope);
    const bool inline_group = group.label() == docv1::GROUP_LABEL_INLINE;
    std::vector<std::string> parts;
    for (const auto& child : group.children()) {
      if (!consume(child.ref())) continue;
      // The reference's layer filter applies at iteration, so a non-body
      // child of a body group stays out too.
      if (excluded_layer(layer_of(child.ref()))) continue;
      std::string part = serialize(child.ref(), list_level, inline_group || inline_scope);
      if (!part.empty()) parts.push_back(std::move(part));
    }
    return join(parts, inline_group ? " " : "\n\n");
  }

  // A list group is one itemize or enumerate environment; the environment is
  // chosen by the first list item's enumerated flag, the begin/end lines
  // indent by two spaces per nesting level, and the items themselves are not
  // indented.
  std::string serialize_list(const docv1::GroupItem& group, int list_level,
                             bool inline_scope) {
    std::vector<std::string> parts;
    for (const auto& child : group.children()) {
      if (!consume(child.ref())) continue;
      if (excluded_layer(layer_of(child.ref()))) continue;
      std::string part = serialize(child.ref(), list_level + 1, inline_scope);
      if (!part.empty()) parts.push_back(std::move(part));
    }
    if (parts.empty()) return std::string();
    const std::string env = first_item_is_enumerated(group) ? "enumerate" : "itemize";
    const std::string indent(static_cast<std::size_t>(std::max(list_level, 0)) *
                                 static_cast<std::size_t>(kListIndent),
                             ' ');
    return indent + "\\begin{" + env + "}\n" + join(parts, "\n") + "\n" + indent +
           "\\end{" + env + "}";
  }

  // docling's first_item_is_enumerated: the first child that is a list item
  // decides the environment for the whole group.
  bool first_item_is_enumerated(const docv1::GroupItem& group) const {
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      const auto& item = document_.texts(ref.index);
      if (classify_text(item) != TextClass::kListItem) continue;
      return item.item_case() == docv1::BaseTextItem::kListItem &&
             item.list_item().enumerated();
    }
    return false;
  }

  // -- text items -----------------------------------------------------------------

  static const google::protobuf::RepeatedPtrField<docv1::RefItem>* item_children(
      const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) return &item.code().children();
    const auto* base = text_base(item);
    return base != nullptr ? &base->children() : nullptr;
  }

  // The section command for a header level: the reference accepts [1, 3] and
  // raises outside it; the export clamps instead. An unset level reads as 1.
  static std::string_view section_command(int level) {
    if (level <= 1) return "section";
    if (level == 2) return "subsection";
    return "subsubsection";
  }

  // Code is never escaped or formatted; inline code escapes only the macro
  // parameter character, with the reference's doubled-backslash literal
  // verbatim. A code label on the generic text arm is the model's rebuild of
  // a code item and takes the same path with the base fields.
  std::string serialize_code(const docv1::BaseTextItem& item, bool inline_scope) {
    const bool own_variant = item.item_case() == docv1::BaseTextItem::kCode;
    const auto* base = own_variant ? nullptr : text_base(item);
    if (!own_variant && base == nullptr) return std::string();
    const std::string& text = own_variant ? item.code().text() : base->text();

    std::string text_part;
    if (inline_scope) {
      std::string safe;
      safe.reserve(text.size());
      for (const char c : text) {
        if (c == '#') {
          safe.append("\\\\#");
        } else {
          safe.push_back(c);
        }
      }
      text_part = "\\texttt{" + safe + "}";
    } else {
      text_part = "\\begin{verbatim}\n" + text + "\n\\end{verbatim}";
    }

    std::vector<std::string> parts;
    parts.push_back(std::move(text_part));
    // Among the text variants only the code item carries captions on the
    // wire; they render beside their item, never on their own.
    if (own_variant) {
      const std::string captions = serialized_captions(item.code().captions());
      if (!captions.empty()) parts.push_back(captions);
    }
    return join(parts, inline_scope ? " " : "\n\n");
  }

  std::string serialize_text(const std::string& ref, const docv1::BaseTextItem& item,
                             bool inline_scope) {
    // A caption or footnote renders through the item that claims it.
    if (caption_refs_.contains(ref)) return std::string();
    const TextClass kind = classify_text(item);
    if (kind == TextClass::kCode) return serialize_code(item, inline_scope);
    const auto* base = text_base(item);
    if (base == nullptr) return std::string();
    const docv1::Formatting* formatting =
        base->has_formatting() ? &base->formatting() : nullptr;
    const std::string* hyperlink = base->has_hyperlink() ? &base->hyperlink() : nullptr;

    // A text item whose only content is one inline group renders that group
    // and skips its own post-processing; the children carry the formatting.
    std::string text = base->text();
    bool post = true;
    const auto* children = item_children(item);
    if (text.empty() && children != nullptr && children->size() == 1) {
      const auto* child_group = group_at(children->Get(0).ref());
      if (child_group != nullptr && child_group->label() == docv1::GROUP_LABEL_INLINE) {
        text = serialize(children->Get(0).ref(), 0, true);
        post = false;
      }
    }

    std::string text_part;
    if (kind == TextClass::kListItem || kind == TextClass::kTitle ||
        kind == TextClass::kSectionHeader) {
      if (post) text = post_process(text, formatting, hyperlink);
      if (kind == TextClass::kListItem) {
        text_part = "\\item " + text;
      } else if (kind == TextClass::kTitle) {
        // The document assembly hoists this into the preamble.
        text_part = "\\title{" + text + "}";
      } else {
        const int level = item.item_case() == docv1::BaseTextItem::kSectionHeader
                              ? item.section_header().level()
                              : 1;
        text_part = "\\" + std::string(section_command(level)) + "{" + text + "}";
      }
    } else if (kind == TextClass::kFormula) {
      if (!text.empty()) {
        text_part = inline_scope ? "$" + text + "$" : "$$" + text + "$$";
      } else if (!base->orig().empty()) {
        text_part = std::string(kFormulaPlaceholder);
      }
    } else {
      if (post) text = post_process(text, formatting, hyperlink);
      text_part = text;
    }

    std::vector<std::string> parts;
    if (!text_part.empty()) parts.push_back(std::move(text_part));
    return join(parts, inline_scope ? " " : "\n\n");
  }

  // The captions one floating item claims, delimiter-joined and escaped. Each
  // resolved caption is consumed so the walk never renders it twice.
  std::string serialized_captions(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions) {
    const std::vector<std::string> texts = caption_texts(captions);
    if (texts.empty()) return std::string();
    return post_process(join(texts, " "), nullptr, nullptr);
  }

  // -- tables -----------------------------------------------------------------

  // One cell's text: a rich cell renders the item it points at, a plain cell
  // escapes its own text; both fold their newlines into spaces.
  std::string cell_text(const docv1::TableCell& cell) {
    std::string text;
    if (cell.has_ref()) {
      const std::string& ref = cell.ref().ref();
      if (consume(ref)) text = serialize(ref, 0, false);
    } else {
      text = escape_latex(cell.text());
    }
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      out.push_back(c == '\n' ? ' ' : c);
    }
    return out;
  }

  // The tabular block of one table data grid, without span support: every
  // covered position repeats its cell, a position no cell reaches renders
  // empty, and the column count is the widest row's.
  std::string tabular(const docv1::TableData& data) {
    const auto grid = derived_table_grid(data);
    std::size_t ncols = 0;
    for (const auto& row : grid) ncols = std::max(ncols, row.size());
    if (ncols == 0) return std::string();
    std::string colspec = "|";
    for (std::size_t i = 0; i < ncols; ++i) colspec.append("l|");
    std::vector<std::string> lines;
    lines.push_back("\\begin{tabular}{" + colspec + "}");
    lines.emplace_back("\\hline");
    for (const auto& row : grid) {
      std::vector<std::string> cells;
      cells.reserve(row.size());
      for (const auto* cell : row) {
        cells.push_back(cell != nullptr ? cell_text(*cell) : std::string());
      }
      lines.push_back(join(cells, " & ") + " \\\\ \\hline");
    }
    lines.emplace_back("\\end{tabular}");
    return join(lines, "\n");
  }

  // A table wraps in the table environment when it has content or a caption.
  std::string table_block(const docv1::TableData& data, const std::string& caption) {
    const std::string body = tabular(data);
    if (body.empty() && caption.empty()) return std::string();
    std::vector<std::string> lines;
    lines.emplace_back("\\begin{table}[h]");
    if (!caption.empty()) lines.push_back("\\caption{" + caption + "}");
    if (!body.empty()) lines.push_back(body);
    lines.emplace_back("\\end{table}");
    return join(lines, "\n");
  }

  std::string serialize_table(const docv1::TableItem& table) {
    return table_block(table.data(), serialized_captions(table.captions()));
  }

  // -- pictures ---------------------------------------------------------------

  // The picture's chart table, the meta field first with the wire annotation
  // list as the fallback, matching the other renderers.
  const docv1::TableData* chart_data(const docv1::PictureItem& picture) const {
    if (picture.has_meta() && picture.meta().has_tabular_chart()) {
      return &picture.meta().tabular_chart().chart_data();
    }
    for (const auto& annotation : picture.annotations()) {
      if (annotation.has_tabular_chart()) {
        return &annotation.tabular_chart().chart_data();
      }
    }
    return nullptr;
  }

  // The reference's annotation comments: classification, description, and
  // molecule annotations render as "% annotation[kind]: text" comment lines,
  // with continuation lines carrying a bare "% " prefix. The classification
  // text is the first prediction's class name, humanized.
  std::vector<std::string> picture_annotations(const docv1::PictureItem& picture) {
    std::vector<std::pair<std::string_view, std::string>> found;
    std::string classification;
    if (picture.has_meta() && picture.meta().has_classification() &&
        !picture.meta().classification().predictions().empty()) {
      classification = picture.meta().classification().predictions(0).class_name();
    } else {
      for (const auto& annotation : picture.annotations()) {
        if (annotation.has_classification() &&
            !annotation.classification().predicted_classes().empty()) {
          classification = annotation.classification().predicted_classes(0).class_name();
          break;
        }
      }
    }
    std::ranges::replace(classification, '_', ' ');
    if (!classification.empty()) found.emplace_back("classification", classification);
    const std::string description = picture_description(picture);
    if (!description.empty()) found.emplace_back("description", description);
    std::string molecule;
    if (picture.has_meta() && picture.meta().has_molecule()) {
      molecule = picture.meta().molecule().smi();
    } else {
      for (const auto& annotation : picture.annotations()) {
        if (annotation.has_molecule()) {
          molecule = annotation.molecule().smi();
          break;
        }
      }
    }
    if (!molecule.empty()) found.emplace_back("molecule", molecule);

    std::vector<std::string> comments;
    for (const auto& [kind, text] : found) {
      std::string comment = "% annotation[" + std::string(kind) + "]: ";
      bool first = true;
      std::size_t at = 0;
      while (true) {
        const std::size_t end = text.find('\n', at);
        if (!first) comment.append("\n% ");
        comment.append(text, at, end == std::string::npos ? end : end - at);
        if (end == std::string::npos) break;
        first = false;
        at = end + 1;
      }
      comments.push_back(std::move(comment));
    }
    return comments;
  }

  // The default image mode positions every picture with the placeholder,
  // whatever image the item carries; a tabular chart follows the figure as a
  // table of its own.
  std::string serialize_picture(const docv1::PictureItem& picture) {
    std::vector<std::string> lines;
    lines.emplace_back("\\begin{figure}[h]");
    lines.emplace_back(kImagePlaceholder);
    const std::string caption = serialized_captions(picture.captions());
    if (!caption.empty()) lines.push_back("\\caption{" + caption + "}");
    for (auto& comment : picture_annotations(picture)) lines.push_back(std::move(comment));
    lines.emplace_back("\\end{figure}");
    std::vector<std::string> parts;
    parts.push_back(join(lines, "\n"));
    if (const docv1::TableData* chart = chart_data(picture);
        chart != nullptr && !chart->table_cells().empty()) {
      const std::string chart_table = table_block(*chart, std::string());
      if (!chart_table.empty()) parts.push_back(chart_table);
    }
    return join(parts, "\n\n");
  }

  // -- document assembly --------------------------------------------------------

  // docling's _post_process_title: the first \title{...} in the body moves to
  // the preamble, every occurrence leaves the body, and the remaining body is
  // stripped with its runs of 3+ newlines collapsed to two. The scan repeats
  // the reference regex's caveat: no nested braces inside a title.
  static bool hoist_title(std::string* body, std::string* title_cmd) {
    std::string& text = *body;
    std::string title;
    bool found = false;
    std::size_t search = 0;
    while (true) {
      const std::size_t at = text.find("\\title", search);
      if (at == std::string::npos) break;
      std::size_t pos = at + 6;
      while (pos < text.size() &&
             std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
      }
      if (pos >= text.size() || text[pos] != '{') {
        search = at + 6;
        continue;
      }
      std::size_t end = pos + 1;
      while (end < text.size() && text[end] != '{' && text[end] != '}') ++end;
      if (end >= text.size() || text[end] != '}') {
        search = at + 6;
        continue;
      }
      if (!found) {
        title = text.substr(pos + 1, end - pos - 1);
        found = true;
      }
      text.erase(at, end + 1 - at);
      search = at;
    }
    if (!found) return false;
    *title_cmd = "\\title{" + title + "}";
    // re.sub(r"\n{3,}", "\n\n", body).strip()
    std::string collapsed;
    collapsed.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
      if (text[i] != '\n') {
        collapsed.push_back(text[i++]);
        continue;
      }
      std::size_t run = i;
      while (run < text.size() && text[run] == '\n') ++run;
      collapsed.append(run - i >= 3 ? "\n\n" : std::string_view(text).substr(i, run - i));
      i = run;
    }
    *body = trimmed(collapsed);
    return true;
  }

  // docling's serialize_doc with the default document class and package list.
  static std::string assemble(std::string body) {
    std::string title_cmd;
    const bool titled = hoist_title(&body, &title_cmd);
    std::string header =
        "\\documentclass[11pt,a4paper]{article}\n"
        "\n"
        "\\usepackage[utf8]{inputenc} % allow utf-8 input\n"
        "\\usepackage[T1]{fontenc}    % use 8-bit T1 fonts\n"
        "\\usepackage{hyperref}       % hyperlinks\n"
        "\\usepackage{url}            % simple URL typesetting\n"
        "\\usepackage{booktabs}       % professional-quality tables\n"
        "\\usepackage{amsfonts}       % blackboard math symbols\n"
        "\\usepackage{nicefrac}       % compact symbols for 1/2, etc.\n"
        "\\usepackage{microtype}      % microtypography\n"
        "\\usepackage{xcolor}         % colors\n"
        "\\usepackage{graphicx}       % graphics\n"
        "\\usepackage[normalem]{ulem} % strikethrough\n";
    if (titled) header += title_cmd + "\n";
    header += "\n\\begin{document}";

    std::string body_block;
    if (titled) body_block = "\\maketitle";
    if (!body.empty()) {
      if (!body_block.empty()) body_block += "\n\n";
      body_block += body;
    }
    std::string out = header + "\n\n";
    if (!body_block.empty()) out += body_block + "\n\n";
    out += "\\end{document}";
    return out;
  }

  // The caption references rendered through their owner and the body-tree
  // items the label vocabulary drops.
  std::set<std::string> caption_refs_;
  std::set<std::string> excluded_refs_;
};

}  // namespace

std::string render_latex(const docv1::Document& document) {
  // The reference reaches the document through its model layer, which
  // relabels ordered-list groups and re-homes list items parented outside a
  // list group on load. Only the migration can change this export, so the
  // defensive copy happens only when a list item actually needs re-homing.
  if (!render::has_misplaced_list_items(document)) {
    return LatexRenderer(document).render();
  }
  docv1::Document normalized = document;
  render::relabel_ordered_list_groups(&normalized);
  render::migrate_misplaced_list_items(&normalized);
  return LatexRenderer(normalized).render();
}

}  // namespace grparse
