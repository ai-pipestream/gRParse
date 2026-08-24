// The Markdown renderer behind render_markdown; semantics documented on the
// declaration in include/grparse/document_render.h.
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "grparse/document_render.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

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
      if (excluded_layer(code.content_layer())) return;
      add_block("```" + code_fence_language(code) + "\n" + code.text() + "\n```");
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || excluded_layer(base->content_layer())) return;
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
    if (excluded_layer(group.content_layer())) return;
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
      if (base == nullptr || excluded_layer(base->content_layer())) continue;
      ++position;
      if (!out->empty()) out->push_back('\n');
      out->append(indent);
      out->append(ordered ? std::to_string(position) + ". " : std::string("- "));
      out->append(base->text());
    }
  }

  void render_table(const docv1::TableItem& table) {
    if (excluded_layer(table.content_layer())) return;
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
    if (excluded_layer(picture.content_layer())) return;
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

}  // namespace

std::string render_markdown(const docv1::Document& document) {
  return MarkdownRenderer(document).render();
}

}  // namespace grparse
