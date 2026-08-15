#include "grparse/document_render.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <google/protobuf/util/json_util.h>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

// A parsed "#/<arena>/<index>" reference. The body and furniture roots and
// anything unparseable resolve to kUnknown; renderers skip those rather than
// guess.
struct ArenaRef {
  enum Kind {
    kText,
    kTable,
    kPicture,
    kGroup,
    kKeyValue,
    kForm,
    kFieldRegion,
    kFieldItem,
    kUnknown,
  };
  Kind kind = kUnknown;
  int index = -1;
};

ArenaRef parse_ref(const std::string& ref) {
  static const std::vector<std::pair<std::string, ArenaRef::Kind>> kArenas{
      {"#/texts/", ArenaRef::kText},
      {"#/tables/", ArenaRef::kTable},
      {"#/pictures/", ArenaRef::kPicture},
      {"#/groups/", ArenaRef::kGroup},
      {"#/key_value_items/", ArenaRef::kKeyValue},
      {"#/form_items/", ArenaRef::kForm},
      {"#/field_regions/", ArenaRef::kFieldRegion},
      {"#/field_items/", ArenaRef::kFieldItem},
  };
  for (const auto& arena : kArenas) {
    if (ref.compare(0, arena.first.size(), arena.first) != 0) continue;
    const std::string digits = ref.substr(arena.first.size());
    // Nine digits keeps the index inside int range; anything longer cannot
    // name a real arena entry and resolves to kUnknown like other malformed
    // references.
    if (digits.empty() || digits.size() > 9 ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
      return {};
    }
    return {arena.second, std::stoi(digits)};
  }
  return {};
}

// The shared base fields of any text variant that carries a nested base;
// nullptr for CodeItem (inline fields) and unset variants.
const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading: return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

// Heading depth for a section header: docling maps level L to "##"×(L+1) in
// Markdown and <h(L+1)> in HTML, clamped to h6. An unset proto level (0)
// counts as level 1.
int heading_rank(int level) {
  return std::min(std::max(level, 1) + 1, 6);
}

// The fence info string for a code block, preferring the collector's raw
// language string over the enum. Enum names lower-case cleanly except the
// spelled-out punctuation ones.
std::string code_fence_language(const docv1::CodeItem& code) {
  if (code.has_code_language_raw()) return code.code_language_raw();
  switch (code.code_language()) {
    case docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED:
    case docv1::CODE_LANGUAGE_LABEL_UNKNOWN:
      return std::string();
    case docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS: return "cpp";
    case docv1::CODE_LANGUAGE_LABEL_C_SHARP: return "csharp";
    default: break;
  }
  std::string name = docv1::CodeLanguageLabel_Name(code.code_language());
  static const std::string kPrefix = "CODE_LANGUAGE_LABEL_";
  if (name.compare(0, kPrefix.size(), kPrefix) == 0) name = name.substr(kPrefix.size());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

// The table's cell layout as a row-major pointer grid. The grid field wins
// when populated; otherwise the flat cell list is placed by its offsets.
// A spanned cell appears at every position it covers; nullptr marks a
// position no cell reaches.
std::vector<std::vector<const docv1::TableCell*>> table_grid(
    const docv1::TableData& data) {
  std::vector<std::vector<const docv1::TableCell*>> grid;
  if (!data.grid().empty()) {
    grid.reserve(data.grid_size());
    for (const auto& row : data.grid()) {
      std::vector<const docv1::TableCell*> cells;
      cells.reserve(row.cells_size());
      for (const auto& cell : row.cells()) cells.push_back(&cell);
      grid.push_back(std::move(cells));
    }
    return grid;
  }
  const int rows = data.num_rows();
  const int cols = data.num_cols();
  if (rows <= 0 || cols <= 0) return grid;
  grid.assign(static_cast<size_t>(rows),
              std::vector<const docv1::TableCell*>(static_cast<size_t>(cols), nullptr));
  for (const auto& cell : data.table_cells()) {
    const int row_end = std::min(
        rows, std::max(cell.end_row_offset_idx(), cell.start_row_offset_idx() + 1));
    const int col_end = std::min(
        cols, std::max(cell.end_col_offset_idx(), cell.start_col_offset_idx() + 1));
    for (int row = std::max(0, cell.start_row_offset_idx()); row < row_end; ++row) {
      for (int col = std::max(0, cell.start_col_offset_idx()); col < col_end; ++col) {
        grid[static_cast<size_t>(row)][static_cast<size_t>(col)] = &cell;
      }
    }
  }
  return grid;
}

// A table cell's text made single-line and pipe-safe for a Markdown row.
std::string markdown_cell_text(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    if (c == '\n' || c == '\r') {
      if (!safe.empty() && safe.back() != ' ') safe.push_back(' ');
    } else if (c == '|') {
      safe.append("\\|");
    } else {
      safe.push_back(c);
    }
  }
  return safe;
}

std::string escape_html_text(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': safe.append("&amp;"); break;
      case '<': safe.append("&lt;"); break;
      case '>': safe.append("&gt;"); break;
      default: safe.push_back(c);
    }
  }
  return safe;
}

std::string escape_html_attribute(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': safe.append("&amp;"); break;
      case '<': safe.append("&lt;"); break;
      case '>': safe.append("&gt;"); break;
      case '"': safe.append("&quot;"); break;
      default: safe.push_back(c);
    }
  }
  return safe;
}

// Both renderers walk the body tree the same way: resolve each child
// reference, render the item, and record caption items when a table or
// figure claims them so a caption linked into the tree twice never renders
// twice. Furniture-layer items are excluded, matching docling's default of
// exporting the body content layer only.
class RendererBase {
 protected:
  explicit RendererBase(const docv1::Document& document) : document_(document) {}

  const docv1::Document& document_;
  std::set<std::string> consumed_;

  bool consume(const std::string& ref) { return consumed_.insert(ref).second; }

  bool furniture(docv1::ContentLayer layer) const {
    return layer == docv1::CONTENT_LAYER_FURNITURE;
  }

  // The caption texts a table or figure references, in reference order.
  // Each resolved caption is consumed so the tree walk skips it later.
  std::vector<std::string> caption_texts(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions) {
    std::vector<std::string> texts;
    for (const auto& ref : captions) {
      const ArenaRef resolved = parse_ref(ref.ref());
      if (resolved.kind != ArenaRef::kText ||
          resolved.index >= document_.texts_size()) {
        continue;
      }
      consumed_.insert(ref.ref());
      const auto* base = text_base(document_.texts(resolved.index));
      if (base != nullptr && !base->text().empty()) texts.push_back(base->text());
    }
    return texts;
  }
};

// ============================================================================
// Markdown
// ============================================================================

class MarkdownRenderer : RendererBase {
 public:
  explicit MarkdownRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    render_children(document_.body());
    std::string out;
    for (const auto& block : blocks_) {
      if (!out.empty()) out.append("\n\n");
      out.append(block);
    }
    return out;
  }

 private:
  std::vector<std::string> blocks_;

  void add_block(std::string block) {
    if (!block.empty()) blocks_.push_back(std::move(block));
  }

  void render_children(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) render_ref(child.ref());
  }

  void render_ref(const std::string& raw) {
    if (!consume(raw)) return;
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        if (ref.index < document_.texts_size()) render_text(document_.texts(ref.index));
        break;
      case ArenaRef::kTable:
        if (ref.index < document_.tables_size()) render_table(document_.tables(ref.index));
        break;
      case ArenaRef::kPicture:
        if (ref.index < document_.pictures_size()) {
          render_picture(document_.pictures(ref.index));
        }
        break;
      case ArenaRef::kGroup:
        if (ref.index < document_.groups_size()) render_group(document_.groups(ref.index));
        break;
      case ArenaRef::kKeyValue:
        // No Markdown counterpart; docling's placeholder keeps the item's
        // presence visible without inventing syntax.
        add_block("<!-- missing-key-value-item -->");
        break;
      case ArenaRef::kForm:
        add_block("<!-- missing-form-item -->");
        break;
      case ArenaRef::kFieldRegion:
        if (ref.index < document_.field_regions_size()) {
          for (const auto& child : document_.field_regions(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      case ArenaRef::kFieldItem:
        if (ref.index < document_.field_items_size()) {
          for (const auto& child : document_.field_items(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      case ArenaRef::kUnknown: break;
    }
  }

  void render_text(const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (furniture(code.content_layer())) return;
      add_block("```" + code_fence_language(code) + "\n" + code.text() + "\n```");
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        add_block("# " + base->text());
        return;
      case docv1::BaseTextItem::kSectionHeader:
        add_block(std::string(static_cast<size_t>(
                                  heading_rank(item.section_header().level())),
                              '#') +
                  " " + base->text());
        return;
      case docv1::BaseTextItem::kFieldHeading:
        add_block(std::string(static_cast<size_t>(
                                  heading_rank(item.field_heading().level())),
                              '#') +
                  " " + base->text());
        return;
      case docv1::BaseTextItem::kFormula:
        // docling's exact placeholder for a formula with no decoded text.
        add_block(base->text().empty() ? std::string("<!-- formula-not-decoded -->")
                                       : "$$" + base->text() + "$$");
        return;
      case docv1::BaseTextItem::kListItem:
        // A list item outside a list group still reads as a one-item list.
        add_block("- " + base->text());
        return;
      default: break;
    }
    if (base->label() == docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED) {
      add_block("- [x] " + base->text());
    } else if (base->label() == docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED) {
      add_block("- [ ] " + base->text());
    } else if (!base->text().empty()) {
      add_block(base->text());
    }
  }

  void render_group(const docv1::GroupItem& group) {
    if (furniture(group.content_layer())) return;
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      std::string list;
      render_list(group, 0, &list);
      add_block(std::move(list));
      return;
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      // Inline groups join their text children into one paragraph, matching
      // docling's space-joined inline scope.
      std::string paragraph;
      for (const auto& child : group.children()) {
        const ArenaRef ref = parse_ref(child.ref());
        if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
        if (!consume(child.ref())) continue;
        const auto* base = text_base(document_.texts(ref.index));
        if (base == nullptr || base->text().empty()) continue;
        if (!paragraph.empty()) paragraph.push_back(' ');
        paragraph.append(base->text());
      }
      add_block(std::move(paragraph));
      return;
    }
    // Every other group label is a transparent container.
    render_children(group);
  }

  // Whether a list group renders ordered: an explicit ordered label, or a
  // plain list whose first list item is enumerated (docling's
  // first_item_is_enumerated rule).
  bool ordered_list(const docv1::GroupItem& group) const {
    if (group.label() == docv1::GROUP_LABEL_ORDERED_LIST) return true;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      const auto& item = document_.texts(ref.index);
      if (item.item_case() != docv1::BaseTextItem::kListItem) continue;
      return item.list_item().enumerated();
    }
    return false;
  }

  void render_list(const docv1::GroupItem& group, int depth, std::string* out) {
    const bool ordered = ordered_list(group);
    const std::string indent(static_cast<size_t>(depth) * 4, ' ');
    int position = 0;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const auto& nested = document_.groups(ref.index);
        if (nested.label() == docv1::GROUP_LABEL_LIST ||
            nested.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
          if (consume(child.ref())) render_list(nested, depth + 1, out);
          continue;
        }
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      const auto* base = text_base(document_.texts(ref.index));
      if (base == nullptr || furniture(base->content_layer())) continue;
      ++position;
      if (!out->empty()) out->push_back('\n');
      out->append(indent);
      out->append(ordered ? std::to_string(position) + ". " : std::string("- "));
      out->append(base->text());
    }
  }

  void render_table(const docv1::TableItem& table) {
    if (furniture(table.content_layer())) return;
    for (const auto& caption : caption_texts(table.captions())) {
      add_block("*" + caption + "*");
    }
    const auto grid = table_grid(table.data());
    if (grid.empty()) return;
    size_t columns = 0;
    for (const auto& row : grid) columns = std::max(columns, row.size());
    if (columns == 0) return;
    std::string out;
    for (size_t row_index = 0; row_index < grid.size(); ++row_index) {
      if (!out.empty()) out.push_back('\n');
      out.push_back('|');
      for (size_t col = 0; col < columns; ++col) {
        const docv1::TableCell* cell =
            col < grid[row_index].size() ? grid[row_index][col] : nullptr;
        out.append(" " +
                   markdown_cell_text(cell != nullptr ? cell->text() : std::string()) +
                   " |");
      }
      if (row_index == 0) {
        out.push_back('\n');
        out.push_back('|');
        for (size_t col = 0; col < columns; ++col) out.append("---|");
      }
    }
    add_block(std::move(out));
  }

  void render_picture(const docv1::PictureItem& picture) {
    if (furniture(picture.content_layer())) return;
    const std::vector<std::string> captions = caption_texts(picture.captions());
    for (const auto& caption : captions) add_block("*" + caption + "*");
    const std::string& uri = picture.has_image() ? picture.image().uri() : std::string();
    if (uri.empty()) {
      // docling's exact placeholder for a picture with nothing to reference.
      add_block("<!-- image -->");
      return;
    }
    const std::string alt = captions.empty() ? std::string("Image") : captions.front();
    add_block("![" + alt + "](" + uri + ")");
  }
};

// ============================================================================
// HTML
// ============================================================================

class HtmlRenderer : RendererBase {
 public:
  explicit HtmlRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    render_children(document_.body());
    std::string out = "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/><title>";
    out.append(escape_html_text(document_.name()));
    out.append("</title></head>\n<body>\n");
    for (const auto& element : elements_) {
      out.append(element);
      out.push_back('\n');
    }
    out.append("</body>\n</html>");
    return out;
  }

 private:
  std::vector<std::string> elements_;

  void add_element(std::string element) {
    if (!element.empty()) elements_.push_back(std::move(element));
  }

  // Paragraph-flavoured escaping: entities for markup characters, <br> for
  // the newlines a <p> would otherwise collapse.
  static std::string paragraph_text(const std::string& text) {
    std::string escaped = escape_html_text(text);
    std::string out;
    out.reserve(escaped.size());
    for (const char c : escaped) {
      if (c == '\n') {
        out.append("<br>");
      } else if (c != '\r') {
        out.push_back(c);
      }
    }
    return out;
  }

  void render_children(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) render_ref(child.ref());
  }

  void render_ref(const std::string& raw) {
    if (!consume(raw)) return;
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        if (ref.index < document_.texts_size()) render_text(document_.texts(ref.index));
        break;
      case ArenaRef::kTable:
        if (ref.index < document_.tables_size()) render_table(document_.tables(ref.index));
        break;
      case ArenaRef::kPicture:
        if (ref.index < document_.pictures_size()) {
          render_picture(document_.pictures(ref.index));
        }
        break;
      case ArenaRef::kGroup:
        if (ref.index < document_.groups_size()) render_group(document_.groups(ref.index));
        break;
      case ArenaRef::kKeyValue:
        add_element("<!-- missing-key-value-item -->");
        break;
      case ArenaRef::kForm:
        add_element("<!-- missing-form-item -->");
        break;
      case ArenaRef::kFieldRegion:
        if (ref.index < document_.field_regions_size()) {
          for (const auto& child : document_.field_regions(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      case ArenaRef::kFieldItem:
        if (ref.index < document_.field_items_size()) {
          for (const auto& child : document_.field_items(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      case ArenaRef::kUnknown: break;
    }
  }

  void render_text(const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (furniture(code.content_layer())) return;
      add_element("<pre><code>" + escape_html_text(code.text()) + "</code></pre>");
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        add_element("<h1>" + paragraph_text(base->text()) + "</h1>");
        return;
      case docv1::BaseTextItem::kSectionHeader:
      case docv1::BaseTextItem::kFieldHeading: {
        const int level = item.item_case() == docv1::BaseTextItem::kSectionHeader
                              ? item.section_header().level()
                              : item.field_heading().level();
        const std::string tag = "h" + std::to_string(heading_rank(level));
        add_element("<" + tag + ">" + paragraph_text(base->text()) + "</" + tag + ">");
        return;
      }
      case docv1::BaseTextItem::kFormula:
        // No MathML conversion here: the formula source rides an escaped
        // block, with docling's placeholder when nothing was decoded.
        add_element(base->text().empty()
                        ? std::string("<div class=\"formula-not-decoded\">Formula "
                                      "not decoded</div>")
                        : "<div class=\"formula\">" + escape_html_text(base->text()) +
                              "</div>");
        return;
      case docv1::BaseTextItem::kListItem:
        // A list item outside a list group still reads as a one-item list.
        add_element("<ul><li>" + paragraph_text(base->text()) + "</li></ul>");
        return;
      default: break;
    }
    if (!base->text().empty()) {
      add_element("<p>" + paragraph_text(base->text()) + "</p>");
    }
  }

  void render_group(const docv1::GroupItem& group) {
    if (furniture(group.content_layer())) return;
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      add_element(render_list(group));
      return;
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      std::string paragraph;
      for (const auto& child : group.children()) {
        const ArenaRef ref = parse_ref(child.ref());
        if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
        if (!consume(child.ref())) continue;
        const auto* base = text_base(document_.texts(ref.index));
        if (base == nullptr || base->text().empty()) continue;
        if (!paragraph.empty()) paragraph.push_back(' ');
        paragraph.append(paragraph_text(base->text()));
      }
      if (!paragraph.empty()) add_element("<p>" + paragraph + "</p>");
      return;
    }
    render_children(group);
  }

  bool ordered_list(const docv1::GroupItem& group) const {
    if (group.label() == docv1::GROUP_LABEL_ORDERED_LIST) return true;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      const auto& item = document_.texts(ref.index);
      if (item.item_case() != docv1::BaseTextItem::kListItem) continue;
      return item.list_item().enumerated();
    }
    return false;
  }

  std::string render_list(const docv1::GroupItem& group) {
    const std::string tag = ordered_list(group) ? "ol" : "ul";
    std::string out = "<" + tag + ">";
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const auto& nested = document_.groups(ref.index);
        if (nested.label() == docv1::GROUP_LABEL_LIST ||
            nested.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
          if (consume(child.ref())) out.append("<li>" + render_list(nested) + "</li>");
          continue;
        }
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      const auto* base = text_base(document_.texts(ref.index));
      if (base == nullptr || furniture(base->content_layer())) continue;
      out.append("<li>" + paragraph_text(base->text()) + "</li>");
    }
    out.append("</" + tag + ">");
    return out;
  }

  void render_table(const docv1::TableItem& table) {
    if (furniture(table.content_layer())) return;
    const std::vector<std::string> captions = caption_texts(table.captions());
    const auto grid = table_grid(table.data());
    if (grid.empty() && captions.empty()) return;
    std::string out = "<table>";
    if (!captions.empty()) {
      std::string caption;
      for (const auto& text : captions) {
        if (!caption.empty()) caption.push_back(' ');
        caption.append(paragraph_text(text));
      }
      out.append("<caption>" + caption + "</caption>");
    }
    out.append("<tbody>");
    // A spanned cell is emitted once, at the first grid position it covers;
    // the coverage map suppresses its mirrored continuations regardless of
    // whether the producer stamped span offsets on every mirror.
    size_t columns = 0;
    for (const auto& row : grid) columns = std::max(columns, row.size());
    std::vector<std::vector<bool>> covered(
        grid.size(), std::vector<bool>(columns, false));
    for (size_t row = 0; row < grid.size(); ++row) {
      out.append("<tr>");
      for (size_t col = 0; col < grid[row].size(); ++col) {
        if (covered[row][col]) continue;
        const docv1::TableCell* cell = grid[row][col];
        if (cell == nullptr) {
          out.append("<td></td>");
          continue;
        }
        const int row_span = std::max(cell->row_span(), 1);
        const int col_span = std::max(cell->col_span(), 1);
        for (size_t r = row; r < std::min(grid.size(), row + static_cast<size_t>(row_span)); ++r) {
          for (size_t c = col; c < std::min(columns, col + static_cast<size_t>(col_span)); ++c) {
            covered[r][c] = true;
          }
        }
        const bool header =
            cell->column_header() || cell->row_header() || cell->row_section();
        std::string tag = header ? "th" : "td";
        std::string attributes;
        if (row_span > 1) attributes += " rowspan=\"" + std::to_string(row_span) + "\"";
        if (col_span > 1) attributes += " colspan=\"" + std::to_string(col_span) + "\"";
        out.append("<" + tag + attributes + ">" + paragraph_text(cell->text()) +
                   "</" + tag + ">");
      }
      out.append("</tr>");
    }
    out.append("</tbody></table>");
    add_element(std::move(out));
  }

  void render_picture(const docv1::PictureItem& picture) {
    if (furniture(picture.content_layer())) return;
    const std::vector<std::string> captions = caption_texts(picture.captions());
    const std::string& uri = picture.has_image() ? picture.image().uri() : std::string();
    std::string out = "<figure>";
    if (uri.empty()) {
      out.append("<!-- image -->");
    } else {
      const std::string alt = captions.empty() ? std::string("Image") : captions.front();
      out.append("<img src=\"" + escape_html_attribute(uri) + "\" alt=\"" +
                 escape_html_attribute(alt) + "\"/>");
    }
    for (const auto& caption : captions) {
      out.append("<figcaption>" + paragraph_text(caption) + "</figcaption>");
    }
    out.append("</figure>");
    add_element(std::move(out));
  }
};

}  // namespace

std::string render_markdown(const docv1::Document& document) {
  return MarkdownRenderer(document).render();
}

std::string render_html(const docv1::Document& document) {
  return HtmlRenderer(document).render();
}

std::string render_json(const docv1::Document& document) {
  std::string out;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  const auto status = google::protobuf::util::MessageToJsonString(document, &out, options);
  if (!status.ok()) {
    throw std::runtime_error("document JSON export failed: " +
                             std::string(status.message()));
  }
  return out;
}

}  // namespace grparse
