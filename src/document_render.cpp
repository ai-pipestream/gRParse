#include "grparse/document_render.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <google/protobuf/util/json_util.h>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

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

// The docling short name of a DocItemLabel ("section_header", "text",
// "checkbox_selected"), which doubles as the DocTags token vocabulary.
std::string label_short_name(docv1::DocItemLabel label) {
  std::string name = docv1::DocItemLabel_Name(label);
  static const std::string kPrefix = "DOC_ITEM_LABEL_";
  if (name.compare(0, kPrefix.size(), kPrefix) == 0) name = name.substr(kPrefix.size());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

// Whitespace-trimmed copy, mirroring docling's str.strip() on item text.
std::string trimmed(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n\f\v");
  if (begin == std::string::npos) return std::string();
  const auto end = text.find_last_not_of(" \t\r\n\f\v");
  return text.substr(begin, end - begin + 1);
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
    std::string out = skeleton_open();
    for (const auto& element : elements_) {
      out.append(element);
      out.push_back('\n');
    }
    out.append("</body>\n</html>");
    return out;
  }

  // The split-page layout docling's SPLIT_PAGE output style produces: one
  // two-column table row per page, page image beside a page div of that
  // page's elements. Elements recorded before any provenance was seen join
  // the first provenanced page; a document with no provenance at all is one
  // page.
  std::string render_split_page() {
    render_children(document_.body());
    int first_page = 0;
    for (const int page : element_pages_) {
      if (page > 0) {
        first_page = page;
        break;
      }
    }
    if (first_page == 0) {
      // No provenance anywhere: the lone page keys off the page map when one
      // exists, page 1 otherwise.
      for (const auto& entry : document_.pages()) {
        if (first_page == 0 || entry.first < first_page) first_page = entry.first;
      }
      if (first_page <= 0) first_page = 1;
    }
    for (int& page : element_pages_) {
      if (page == 0) page = first_page;
    }

    std::vector<int> page_order;
    for (const int page : element_pages_) {
      if (std::find(page_order.begin(), page_order.end(), page) == page_order.end()) {
        page_order.push_back(page);
      }
    }

    std::string out = skeleton_open();
    out.append("<table>\n<tbody>\n");
    for (const int page : page_order) {
      out.append("<tr>\n<td>\n");
      const auto page_item = document_.pages().find(page);
      const std::string uri =
          page_item != document_.pages().end() && page_item->second.has_image()
              ? page_item->second.image().uri()
              : std::string();
      if (uri.empty()) {
        // docling's exact placeholder when the page carries no image.
        out.append("<figure>no page-image found</figure>\n");
      } else {
        out.append("<figure><img src=\"" + escape_html_attribute(uri) + "\"></figure>\n");
      }
      out.append("</td>\n<td>\n<div class='page'>\n");
      bool first_element = true;
      for (size_t index = 0; index < elements_.size(); ++index) {
        if (element_pages_[index] != page) continue;
        if (!first_element) out.push_back('\n');
        out.append(elements_[index]);
        first_element = false;
      }
      out.append("\n</div>\n</td>\n</tr>\n");
    }
    out.append("</tbody>\n</table>\n</body>\n</html>");
    return out;
  }

 private:
  std::vector<std::string> elements_;
  std::vector<int> element_pages_;
  // The page the walk is currently on: the page of the last provenance seen,
  // 0 until any provenance appears. Un-provenanced items inherit it.
  int current_page_ = 0;

  std::string skeleton_open() const {
    return "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/><title>" +
           escape_html_text(document_.name()) + "</title></head>\n<body>\n";
  }

  void note_page(const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) {
    if (!prov.empty() && prov.Get(0).page_no() > 0) current_page_ = prov.Get(0).page_no();
  }

  // Moves the current page to the first provenanced text inside a group, so
  // the group's single element lands on the page where the group starts.
  void note_group_page(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kText && ref.index < document_.texts_size()) {
        const auto& item = document_.texts(ref.index);
        const auto* prov = item.item_case() == docv1::BaseTextItem::kCode
                               ? &item.code().prov()
                               : (text_base(item) != nullptr ? &text_base(item)->prov()
                                                             : nullptr);
        if (prov != nullptr && !prov->empty() && prov->Get(0).page_no() > 0) {
          current_page_ = prov->Get(0).page_no();
          return;
        }
      } else if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const int before = current_page_;
        note_group_page(document_.groups(ref.index));
        if (current_page_ != before) return;
      }
    }
  }

  void add_element(std::string element) {
    if (element.empty()) return;
    elements_.push_back(std::move(element));
    element_pages_.push_back(current_page_);
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
      note_page(code.prov());
      add_element("<pre><code>" + escape_html_text(code.text()) + "</code></pre>");
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    note_page(base->prov());
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
      note_group_page(group);
      add_element(render_list(group));
      return;
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      note_group_page(group);
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
    note_page(table.prov());
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
    note_page(picture.prov());
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

// ============================================================================
// DocTags
// ============================================================================

// docling's exact CodeLanguageLabel value string for our enum, used by the
// DocTags "<_Language_>" token. The raw wire string wins when present.
std::string doctags_code_language(const docv1::CodeItem& code) {
  if (code.has_code_language_raw()) return code.code_language_raw();
  switch (code.code_language()) {
    case docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED:
    case docv1::CODE_LANGUAGE_LABEL_UNKNOWN: return "unknown";
    case docv1::CODE_LANGUAGE_LABEL_BC: return "bc";
    case docv1::CODE_LANGUAGE_LABEL_DC: return "dc";
    case docv1::CODE_LANGUAGE_LABEL_C_SHARP: return "C#";
    case docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS: return "C++";
    case docv1::CODE_LANGUAGE_LABEL_CMAKE: return "CMake";
    case docv1::CODE_LANGUAGE_LABEL_COBOL: return "COBOL";
    case docv1::CODE_LANGUAGE_LABEL_CSS: return "CSS";
    case docv1::CODE_LANGUAGE_LABEL_DOCKERFILE: return "Dockerfile";
    case docv1::CODE_LANGUAGE_LABEL_DOCLANG: return "DocLang";
    case docv1::CODE_LANGUAGE_LABEL_FORTRAN: return "FORTRAN";
    case docv1::CODE_LANGUAGE_LABEL_HTML: return "HTML";
    case docv1::CODE_LANGUAGE_LABEL_JAVASCRIPT: return "JavaScript";
    case docv1::CODE_LANGUAGE_LABEL_JSON: return "JSON";
    case docv1::CODE_LANGUAGE_LABEL_LATEX: return "Latex";
    case docv1::CODE_LANGUAGE_LABEL_MATLAB: return "Matlab";
    case docv1::CODE_LANGUAGE_LABEL_MOONSCRIPT: return "MoonScript";
    case docv1::CODE_LANGUAGE_LABEL_OCAML: return "OCaml";
    case docv1::CODE_LANGUAGE_LABEL_OBJECTIVEC: return "ObjectiveC";
    case docv1::CODE_LANGUAGE_LABEL_PHP: return "PHP";
    case docv1::CODE_LANGUAGE_LABEL_SML: return "SML";
    case docv1::CODE_LANGUAGE_LABEL_SQL: return "SQL";
    case docv1::CODE_LANGUAGE_LABEL_TIKZ: return "Tikz";
    case docv1::CODE_LANGUAGE_LABEL_TYPESCRIPT: return "TypeScript";
    case docv1::CODE_LANGUAGE_LABEL_VISUALBASIC: return "VisualBasic";
    case docv1::CODE_LANGUAGE_LABEL_XML: return "XML";
    case docv1::CODE_LANGUAGE_LABEL_YAML: return "YAML";
    default: break;
  }
  // The remaining docling values are simple capitalized words ("Python",
  // "Ada", "Rust"), which the enum suffix reproduces.
  std::string name = docv1::CodeLanguageLabel_Name(code.code_language());
  static const std::string kPrefix = "CODE_LANGUAGE_LABEL_";
  if (name.compare(0, kPrefix.size(), kPrefix) == 0) name = name.substr(kPrefix.size());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// The picture classes DocTags promotes from <picture> to <chart>.
bool doctags_chart_class(const std::string& class_name) {
  static const std::set<std::string> kChartClasses{
      "pie_chart", "bar_chart",     "stacked_bar_chart", "line_chart",
      "flow_chart", "scatter_chart", "heatmap"};
  return kChartClasses.count(class_name) > 0;
}

class DocTagsRenderer : RendererBase {
 public:
  explicit DocTagsRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    std::vector<std::string> parts;
    for (const auto& child : document_.body().children()) {
      if (!consume(child.ref())) continue;
      std::string part = render_ref(child.ref());
      if (!part.empty()) parts.push_back(std::move(part));
    }
    // docling's document assembly verbatim: parts joined by the
    // human-friendly newline delimiter, with a trailing delimiter before the
    // closing wrapper.
    std::string out = "<doctag>";
    for (size_t index = 0; index < parts.size(); ++index) {
      if (index > 0) out.push_back('\n');
      out.append(parts[index]);
    }
    out.append("\n</doctag>");
    return out;
  }

 private:
  static std::string wrap(const std::string& tag, const std::string& text) {
    return "<" + tag + ">" + text + "</" + tag + ">";
  }

  // "<loc_x0><loc_y0><loc_x1><loc_y1>" for the item's first provenance
  // entry, normalized to docling's 500-step grid in top-left origin. Empty
  // when there is no provenance or the page's size is unknown, which is
  // docling's behavior for absent provenance.
  std::string location_tokens(
      const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) const {
    if (prov.empty()) return std::string();
    const auto& item = prov.Get(0);
    const auto page = document_.pages().find(item.page_no());
    if (page == document_.pages().end()) return std::string();
    const double width = page->second.size().width();
    const double height = page->second.size().height();
    if (width <= 0.0 || height <= 0.0) return std::string();
    double top = item.bbox().t();
    double bottom = item.bbox().b();
    if (item.bbox().coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) {
      top = height - item.bbox().t();
      bottom = height - item.bbox().b();
    }
    const auto token = [](double value, double extent) {
      const long index = std::lround(500.0 * (value / extent));
      return "<loc_" + std::to_string(std::min(499L, std::max(0L, index))) + ">";
    };
    const double left = std::min(item.bbox().l(), item.bbox().r());
    const double right = std::max(item.bbox().l(), item.bbox().r());
    return token(left, width) + token(std::min(top, bottom), height) +
           token(right, width) + token(std::max(top, bottom), height);
  }

  std::string render_ref(const std::string& raw) {
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        if (ref.index < document_.texts_size()) return render_text(document_.texts(ref.index));
        break;
      case ArenaRef::kTable:
        if (ref.index < document_.tables_size()) return render_table(document_.tables(ref.index));
        break;
      case ArenaRef::kPicture:
        if (ref.index < document_.pictures_size()) {
          return render_picture(document_.pictures(ref.index));
        }
        break;
      case ArenaRef::kGroup:
        if (ref.index < document_.groups_size()) return render_group(document_.groups(ref.index));
        break;
      case ArenaRef::kKeyValue:
        if (ref.index < document_.key_value_items_size()) {
          return render_key_value(document_.key_value_items(ref.index));
        }
        break;
      default: break;
    }
    return std::string();
  }

  std::string render_text(const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (furniture(code.content_layer())) return std::string();
      // Code keeps its whitespace; the language token follows the location.
      return wrap("code", location_tokens(code.prov()) + "<_" +
                              doctags_code_language(code) + "_>" + code.text());
    }
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return std::string();
    const std::string body = location_tokens(base->prov()) + trimmed(base->text());
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle: return wrap("title", body);
      case docv1::BaseTextItem::kSectionHeader:
        return wrap("section_header_level_" +
                        std::to_string(std::max(item.section_header().level(), 1)),
                    body);
      case docv1::BaseTextItem::kFormula: return wrap("formula", body);
      case docv1::BaseTextItem::kListItem:
        // A list item outside a list group still reads as a one-item list.
        return wrap("unordered_list", wrap("list_item", body) + "\n");
      default: break;
    }
    // The label short name IS the DocTags token for the labels docling
    // knows; labels outside that vocabulary degrade to <text>.
    static const std::set<std::string> kLabelTokens{
        "text",       "paragraph",         "caption",
        "footnote",   "checkbox_selected", "checkbox_unselected",
        "page_header", "page_footer",      "reference",
        "handwritten_text"};
    const std::string label = label_short_name(base->label());
    return wrap(kLabelTokens.count(label) > 0 ? label : "text", body);
  }

  std::string render_group(const docv1::GroupItem& group) {
    if (furniture(group.content_layer())) return std::string();
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      return render_list(group);
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      std::string body;
      for (const auto& child : group.children()) {
        if (!consume(child.ref())) continue;
        body.append(render_ref(child.ref()));
      }
      return body.empty() ? body : wrap("inline", body);
    }
    // Every other group label is a transparent container; its children join
    // like top-level parts.
    std::string body;
    for (const auto& child : group.children()) {
      if (!consume(child.ref())) continue;
      const std::string part = render_ref(child.ref());
      if (part.empty()) continue;
      if (!body.empty()) body.push_back('\n');
      body.append(part);
    }
    return body;
  }

  // docling's list assembly verbatim: every child's serialization wrapped in
  // <list_item>, items joined by newline with a trailing newline, wrapped in
  // the ordered or unordered token. List items themselves carry no label
  // wrapper; nested lists ride inside their <list_item>.
  std::string render_list(const docv1::GroupItem& group) {
    std::vector<std::string> items;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (!consume(child.ref())) continue;
      std::string body;
      if (ref.kind == ArenaRef::kText && ref.index < document_.texts_size()) {
        const auto& item = document_.texts(ref.index);
        if (item.item_case() == docv1::BaseTextItem::kListItem) {
          const auto* base = text_base(item);
          if (base == nullptr || furniture(base->content_layer())) continue;
          body = location_tokens(base->prov()) + trimmed(base->text());
        } else {
          body = render_text(item);
        }
      } else {
        body = render_ref(child.ref());
      }
      if (!body.empty()) items.push_back(wrap("list_item", body));
    }
    if (items.empty()) return std::string();
    std::string joined;
    for (size_t index = 0; index < items.size(); ++index) {
      if (index > 0) joined.push_back('\n');
      joined.append(items[index]);
    }
    joined.push_back('\n');
    return wrap(ordered_list(group) ? "ordered_list" : "unordered_list", joined);
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

  std::string caption_block(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions) {
    std::string text;
    for (const auto& caption : caption_texts(captions)) {
      if (!text.empty()) text.push_back(' ');
      text.append(trimmed(caption));
    }
    return text.empty() ? text : wrap("caption", text);
  }

  // OTSL cell tokens over the mirrored grid, docling's export_to_otsl
  // verbatim: start positions emit their flag token (ched/rhed/srow/fcel)
  // followed by the trimmed text, empty starts emit <ecel>, continuations
  // emit <ucel>/<lcel>/<xcel> by span direction, and <nl> closes each row.
  std::string otsl_cells(const docv1::TableData& data) const {
    const auto grid = table_grid(data);
    if (grid.empty()) return std::string();
    size_t columns = 0;
    for (const auto& row : grid) columns = std::max(columns, row.size());
    std::string out;
    for (size_t row = 0; row < grid.size(); ++row) {
      for (size_t col = 0; col < columns; ++col) {
        const docv1::TableCell* cell =
            col < grid[row].size() ? grid[row][col] : nullptr;
        if (cell == nullptr) {
          out.append("<ecel>");
          continue;
        }
        const size_t row_start = static_cast<size_t>(
            std::max(cell->start_row_offset_idx(), 0));
        const size_t col_start = static_cast<size_t>(
            std::max(cell->start_col_offset_idx(), 0));
        if (row_start == row && col_start == col) {
          const std::string content = trimmed(cell->text());
          if (content.empty()) {
            out.append("<ecel>");
          } else {
            if (cell->column_header()) {
              out.append("<ched>");
            } else if (cell->row_header()) {
              out.append("<rhed>");
            } else if (cell->row_section()) {
              out.append("<srow>");
            } else {
              out.append("<fcel>");
            }
            out.append(content);
          }
        } else {
          bool cross = false;
          if (row_start != row) {
            if (cell->col_span() <= 1) {
              out.append("<ucel>");
            } else {
              cross = true;
            }
          }
          if (col_start != col) {
            if (cell->row_span() <= 1) {
              out.append("<lcel>");
            } else {
              cross = true;
            }
          }
          if (cross) out.append("<xcel>");
        }
      }
      out.append("<nl>");
    }
    return out;
  }

  std::string render_table(const docv1::TableItem& table) {
    if (furniture(table.content_layer())) return std::string();
    const std::string body =
        location_tokens(table.prov()) + otsl_cells(table.data()) +
        caption_block(table.captions());
    return body.empty() ? body : wrap("otsl", body);
  }

  std::string render_picture(const docv1::PictureItem& picture) {
    if (furniture(picture.content_layer())) return std::string();
    std::string body = location_tokens(picture.prov());

    // Classification: the meta field wins, legacy annotations fall back.
    std::string predicted_class;
    if (picture.has_meta() && picture.meta().has_classification()) {
      double best = -1.0;
      for (const auto& prediction : picture.meta().classification().predictions()) {
        const double confidence =
            prediction.has_confidence() ? prediction.confidence() : 0.0;
        if (confidence > best) {
          best = confidence;
          predicted_class = prediction.class_name();
        }
      }
    } else {
      for (const auto& annotation : picture.annotations()) {
        if (annotation.has_classification() &&
            !annotation.classification().predicted_classes().empty()) {
          predicted_class =
              annotation.classification().predicted_classes(0).class_name();
          break;
        }
      }
    }
    if (!predicted_class.empty()) body.append("<" + predicted_class + ">");

    std::string smiles;
    if (picture.has_meta() && picture.meta().has_molecule()) {
      smiles = picture.meta().molecule().smi();
    } else {
      for (const auto& annotation : picture.annotations()) {
        if (annotation.has_molecule()) {
          smiles = annotation.molecule().smi();
          break;
        }
      }
    }
    if (!smiles.empty()) body.append(wrap("smiles", smiles));

    const docv1::TableData* chart_data = nullptr;
    if (picture.has_meta() && picture.meta().has_tabular_chart()) {
      chart_data = &picture.meta().tabular_chart().chart_data();
    } else {
      for (const auto& annotation : picture.annotations()) {
        if (annotation.has_tabular_chart()) {
          chart_data = &annotation.tabular_chart().chart_data();
          break;
        }
      }
    }
    if (chart_data != nullptr && !chart_data->table_cells().empty()) {
      body.append(otsl_cells(*chart_data));
    }

    body.append(caption_block(picture.captions()));
    if (body.empty()) return body;
    return wrap(doctags_chart_class(predicted_class) ? "chart" : "picture", body);
  }

  std::string render_key_value(const docv1::KeyValueItem& item) {
    if (furniture(item.content_layer())) return std::string();
    std::string body = location_tokens(item.prov());
    for (const auto& cell : item.graph().cells()) {
      std::string cell_text = trimmed(cell.text());
      for (const auto& link : item.graph().links()) {
        if (link.source_cell_id() == cell.cell_id()) {
          cell_text.append("<link_" + std::to_string(link.target_cell_id()) + ">");
        }
      }
      std::string label = docv1::GraphCellLabel_Name(cell.label());
      static const std::string kPrefix = "GRAPH_CELL_LABEL_";
      if (label.compare(0, kPrefix.size(), kPrefix) == 0) label = label.substr(kPrefix.size());
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      body.append(wrap(label + "_" + std::to_string(cell.cell_id()), cell_text));
    }
    body.append(caption_block(item.captions()));
    return wrap("key_value_region", body);
  }
};

// ============================================================================
// DocLang
// ============================================================================

class DoclangRenderer : RendererBase {
 public:
  explicit DoclangRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    // The root grpc-xml sniffs: the doclang local name in its NS_DOCLANG
    // namespace.
    out_ = "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n";
    render_children(document_.body(), 1);
    out_.append("</doclang>");
    return out_;
  }

 private:
  std::string out_;

  void line(int depth, const std::string& text) {
    out_.append(static_cast<size_t>(depth) * 2, ' ');
    out_.append(text);
    out_.push_back('\n');
  }

  static std::string element(const std::string& tag, const std::string& attributes,
                             const std::string& text) {
    return "<" + tag + attributes + ">" + escape_html_text(text) + "</" + tag + ">";
  }

  void render_children(const docv1::GroupItem& group, int depth) {
    for (const auto& child : group.children()) render_ref(child.ref(), depth);
  }

  void render_ref(const std::string& raw, int depth) {
    if (!consume(raw)) return;
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        if (ref.index < document_.texts_size()) render_text(document_.texts(ref.index), depth);
        break;
      case ArenaRef::kTable:
        if (ref.index < document_.tables_size()) render_table(document_.tables(ref.index), depth);
        break;
      case ArenaRef::kPicture:
        if (ref.index < document_.pictures_size()) {
          render_picture(document_.pictures(ref.index), depth);
        }
        break;
      case ArenaRef::kGroup:
        if (ref.index < document_.groups_size()) render_group(document_.groups(ref.index), depth);
        break;
      case ArenaRef::kKeyValue:
        // No element in the vocabulary grpc-xml reads; a comment keeps the
        // omission visible without inventing schema.
        line(depth, "<!-- key-value item omitted -->");
        break;
      case ArenaRef::kForm:
        line(depth, "<!-- form item omitted -->");
        break;
      case ArenaRef::kFieldRegion:
        if (ref.index < document_.field_regions_size()) {
          for (const auto& child : document_.field_regions(ref.index).children()) {
            render_ref(child.ref(), depth);
          }
        }
        break;
      case ArenaRef::kFieldItem:
        if (ref.index < document_.field_items_size()) {
          for (const auto& child : document_.field_items(ref.index).children()) {
            render_ref(child.ref(), depth);
          }
        }
        break;
      case ArenaRef::kUnknown: break;
    }
  }

  void render_text(const docv1::BaseTextItem& item, int depth) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (furniture(code.content_layer())) return;
      const std::string language = code_fence_language(code);
      const std::string attributes =
          language.empty() ? std::string()
                           : " language=\"" + escape_html_attribute(language) + "\"";
      line(depth, element("code", attributes, code.text()));
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        line(depth, element("title", "", base->text()));
        return;
      case docv1::BaseTextItem::kSectionHeader:
        line(depth, element("section-header",
                            " level=\"" +
                                std::to_string(std::max(item.section_header().level(), 1)) +
                                "\"",
                            base->text()));
        return;
      case docv1::BaseTextItem::kFormula:
        line(depth, element("formula", "", base->text()));
        return;
      case docv1::BaseTextItem::kListItem:
        line(depth, element("list-item", "", base->text()));
        return;
      default: break;
    }
    switch (base->label()) {
      case docv1::DOC_ITEM_LABEL_FOOTNOTE:
        line(depth, element("footnote", "", base->text()));
        return;
      case docv1::DOC_ITEM_LABEL_REFERENCE:
        line(depth, element("reference", "", base->text()));
        return;
      case docv1::DOC_ITEM_LABEL_CAPTION:
        line(depth, element("caption", "", base->text()));
        return;
      default:
        if (!base->text().empty()) line(depth, element("paragraph", "", base->text()));
        return;
    }
  }

  void render_group(const docv1::GroupItem& group, int depth) {
    if (furniture(group.content_layer())) return;
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      render_list(group, depth);
      return;
    }
    // Non-list groups are transparent containers, as in the other renderers.
    render_children(group, depth);
  }

  void render_list(const docv1::GroupItem& group, int depth) {
    const bool ordered = ordered_list(group);
    line(depth, std::string("<list ordered=\"") + (ordered ? "true" : "false") + "\">");
    int ordinal = 0;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const auto& nested = document_.groups(ref.index);
        if (nested.label() == docv1::GROUP_LABEL_LIST ||
            nested.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
          if (consume(child.ref())) render_list(nested, depth + 1);
          continue;
        }
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      const auto* base = text_base(document_.texts(ref.index));
      if (base == nullptr || furniture(base->content_layer())) continue;
      ++ordinal;
      const std::string attributes =
          ordered ? " ordinal=\"" + std::to_string(ordinal) + "\"" : std::string();
      line(depth + 1, element("list-item", attributes, base->text()));
    }
    line(depth, "</list>");
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

  void render_captions(const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions,
                       int depth) {
    for (const auto& caption : caption_texts(captions)) {
      line(depth, element("caption", "", caption));
    }
  }

  void render_table(const docv1::TableItem& table, int depth) {
    if (furniture(table.content_layer())) return;
    render_captions(table.captions(), depth);
    const auto grid = table_grid(table.data());
    if (grid.empty()) return;
    size_t columns = 0;
    for (const auto& row : grid) columns = std::max(columns, row.size());
    line(depth, "<table>");
    std::vector<std::vector<bool>> covered(grid.size(), std::vector<bool>(columns, false));
    for (size_t row = 0; row < grid.size(); ++row) {
      line(depth + 1, "<tr>");
      for (size_t col = 0; col < grid[row].size(); ++col) {
        if (covered[row][col]) continue;
        const docv1::TableCell* cell = grid[row][col];
        if (cell == nullptr) {
          line(depth + 2, "<td></td>");
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
        std::string attributes;
        if (row_span > 1) attributes += " rowspan=\"" + std::to_string(row_span) + "\"";
        if (col_span > 1) attributes += " colspan=\"" + std::to_string(col_span) + "\"";
        line(depth + 2, element(header ? "th" : "td", attributes, cell->text()));
      }
      line(depth + 1, "</tr>");
    }
    line(depth, "</table>");
  }

  void render_picture(const docv1::PictureItem& picture, int depth) {
    if (furniture(picture.content_layer())) return;
    render_captions(picture.captions(), depth);
    const std::string& uri = picture.has_image() ? picture.image().uri() : std::string();
    if (uri.empty()) {
      line(depth, "<picture/>");
    } else {
      line(depth, "<picture uri=\"" + escape_html_attribute(uri) + "\"/>");
    }
  }
};

// ============================================================================
// WebVTT
// ============================================================================

// "HH:MM:SS.mmm" with two-digit fields (hours widen past 99), from
// milliseconds so 999.6 ms rounds into the seconds field instead of
// printing a four-digit millisecond count.
std::string vtt_timestamp(double seconds) {
  const long long total_ms = std::llround(std::max(seconds, 0.0) * 1000.0);
  const long long hours = total_ms / 3600000;
  const long long minutes = (total_ms / 60000) % 60;
  const long long secs = (total_ms / 1000) % 60;
  const long long millis = total_ms % 1000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld", hours,
                minutes, secs, millis);
  return std::string(buffer);
}

class VttRenderer : RendererBase {
 public:
  explicit VttRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    walk(document_.body());
    flush_cue();
    std::string out = title_.empty() ? "WEBVTT\n" : "WEBVTT " + title_ + "\n";
    for (const auto& block : blocks_) {
      out.push_back('\n');
      out.append(block);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
  }

 private:
  std::string title_;
  std::vector<std::string> blocks_;
  // The open cue being merged: consecutive items with the same identifier
  // and timing join into one payload.
  bool cue_open_ = false;
  std::string cue_identifier_;
  double cue_start_ = 0.0;
  double cue_end_ = 0.0;
  std::string cue_payload_;

  void flush_cue() {
    if (!cue_open_) return;
    std::string block;
    if (!cue_identifier_.empty()) block.append(cue_identifier_ + "\n");
    block.append(vtt_timestamp(cue_start_) + " --> " + vtt_timestamp(cue_end_) + "\n");
    block.append(cue_payload_ + "\n");
    blocks_.push_back(std::move(block));
    cue_open_ = false;
    cue_payload_.clear();
  }

  void walk(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        if (consume(child.ref())) walk(document_.groups(ref.index));
        continue;
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      handle_text(document_.texts(ref.index));
    }
  }

  // The item's first track source. Our wire also stamps CollectorSource
  // attribution into the same list, so the scan skips past those instead of
  // testing only the first entry as docling does.
  static const docv1::TrackSource* track_source(const docv1::TextItemBase& base) {
    for (const auto& source : base.source()) {
      if (source.has_track()) return &source.track();
    }
    return nullptr;
  }

  void handle_text(const docv1::BaseTextItem& item) {
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    if (item.item_case() == docv1::BaseTextItem::kTitle ||
        base->label() == docv1::DOC_ITEM_LABEL_TITLE) {
      if (!base->text().empty()) title_ = trimmed(base->text());
      return;
    }
    if (base->text().empty()) return;
    const docv1::TrackSource* track = track_source(*base);
    if (track == nullptr) return;

    std::string text = base->text();
    if (track->has_voice() && !track->voice().empty()) {
      text = "<v " + track->voice() + ">" + text + "</v>";
    }
    const std::string identifier = track->has_identifier() ? track->identifier() : "";
    if (cue_open_ && identifier == cue_identifier_ && track->start_time() == cue_start_ &&
        track->end_time() == cue_end_) {
      cue_payload_.append("\n" + text);
      return;
    }
    flush_cue();
    cue_open_ = true;
    cue_identifier_ = identifier;
    cue_start_ = track->start_time();
    cue_end_ = track->end_time();
    cue_payload_ = text;
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

std::string render_doctags(const docv1::Document& document) {
  return DocTagsRenderer(document).render();
}

std::string render_doclang(const docv1::Document& document) {
  return DoclangRenderer(document).render();
}

std::string render_vtt(const docv1::Document& document) {
  return VttRenderer(document).render();
}

std::string render_html_split_page(const docv1::Document& document) {
  return HtmlRenderer(document).render_split_page();
}

namespace {

// yaml-cpp keeps the flow style it parsed from JSON input; the export
// promises block style, so every container is restyled before emitting.
void set_block_style(YAML::Node node) {  // NOLINT(performance-unnecessary-value-param): YAML::Node is a shared handle
  if (node.IsMap()) {
    node.SetStyle(YAML::EmitterStyle::Block);
    for (auto entry : node) set_block_style(entry.second);
  } else if (node.IsSequence()) {
    node.SetStyle(YAML::EmitterStyle::Block);
    for (auto entry : node) set_block_style(entry);
  }
}

}  // namespace

std::string render_yaml(const docv1::Document& document) {
  // The canonical JSON is already the exact structure this export promises;
  // YAML is a superset of JSON, so the parsed tree re-emits as the same
  // document in block-style YAML form.
  try {
    YAML::Node tree = YAML::Load(render_json(document));
    set_block_style(tree);
    YAML::Emitter emitter;
    emitter << tree;
    if (!emitter.good()) {
      throw std::runtime_error("document YAML export failed: " + emitter.GetLastError());
    }
    return std::string(emitter.c_str());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("document YAML export failed: " + std::string(error.what()));
  }
}

}  // namespace grparse
