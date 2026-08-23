// The DocLang XML renderer behind render_doclang; semantics documented on
// the declaration in include/grparse/document_render.h.
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "grparse/document_render.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

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

}  // namespace

std::string render_doclang(const docv1::Document& document) {
  return DoclangRenderer(document).render();
}

}  // namespace grparse
