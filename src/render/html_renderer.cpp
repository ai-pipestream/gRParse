// The HTML renderer behind render_html and render_html_split_page; semantics
// documented on the declarations in include/grparse/document_render.h.
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
      if (std::ranges::find(page_order, page) == page_order.end()) {
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

}  // namespace

std::string render_html(const docv1::Document& document) {
  return HtmlRenderer(document).render();
}

std::string render_html_split_page(const docv1::Document& document) {
  return HtmlRenderer(document).render_split_page();
}

}  // namespace grparse
