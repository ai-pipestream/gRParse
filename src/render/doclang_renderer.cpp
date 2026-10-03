// The DocLang XML renderer behind render_doclang; semantics documented on
// the declaration in include/grparse/document_render.h.
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doclang_markup.h"
#include "grparse/document_render.h"
#include "picture_image.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

class DoclangRenderer : RendererBase {
 public:
  DoclangRenderer(const docv1::Document& document, bool include_namespace,
                  const PictureUri& picture_uri)
      : RendererBase(document), include_namespace_(include_namespace), picture_uri_(picture_uri) {
    claim_satellites();
  }

  std::string render() {
    // The root grpc-xml sniffs: the doclang local name in its NS_DOCLANG
    // namespace. Without the namespace it falls back to the local name.
    out_ = include_namespace_ ? "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n"
                              : "<doclang>\n";
    render_children(document_.body(), 1);
    out_.append("</doclang>");
    return out_;
  }

 private:
  const bool include_namespace_;
  const PictureUri& picture_uri_;
  std::string out_;
  // A table's or picture's captions and footnotes (its satellites) render
  // with it and nowhere else, and so does everything nested under them;
  // the tree walk skips both sets, wherever the producer also linked them.
  // Nested content renders while a satellite is open (satellite_depth_).
  std::set<std::string> satellites_;
  std::set<std::string> satellite_nested_;
  int satellite_depth_ = 0;

  void line(int depth, const std::string& text) {
    out_.append(static_cast<size_t>(depth) * 2, ' ');
    out_.append(text);
    out_.push_back('\n');
  }

  static std::string element(const std::string& tag, const std::string& attributes,
                             const std::string& text) {
    return "<" + tag + attributes + ">" + escape_xml_text(text) + "</" + tag + ">";
  }

  void render_children(const docv1::GroupItem& group, int depth) {
    for (const auto& child : group.children()) render_ref(child.ref(), depth);
  }

  void render_ref(const std::string& raw, int depth) {
    if (claimed(raw) || !consume(raw)) return;
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

  // Whether the tree walk must leave a reference to its float: a satellite
  // always, the content nested in one unless a satellite is rendering it.
  bool claimed(const std::string& ref) const {
    return satellites_.contains(ref) ||
           (satellite_depth_ == 0 && satellite_nested_.contains(ref));
  }

  // The text satellites of every table and picture that renders, then
  // everything nested under them, so the walk can leave both to the float.
  // A float in an excluded layer claims nothing: it never renders, so a
  // satellite the producer also linked into the body renders there.
  void claim_satellites() {
    const auto claim = [this](const google::protobuf::RepeatedPtrField<docv1::RefItem>& refs) {
      for (const auto& ref : refs) {
        const ArenaRef parsed = parse_ref(ref.ref());
        if (parsed.kind == ArenaRef::kText && parsed.index < document_.texts_size()) {
          satellites_.insert(ref.ref());
        }
      }
    };
    for (const auto& table : document_.tables()) {
      if (excluded_layer(table.content_layer())) continue;
      claim(table.captions());
      claim(table.footnotes());
    }
    for (const auto& picture : document_.pictures()) {
      if (excluded_layer(picture.content_layer())) continue;
      claim(picture.captions());
      claim(picture.footnotes());
    }
    std::vector<std::string> pending;
    const auto push_children = [this, &pending](const std::string& ref) {
      if (const auto* children = children_of(ref)) {
        for (const auto& child : *children) pending.push_back(child.ref());
      }
    };
    for (const auto& ref : satellites_) push_children(ref);
    while (!pending.empty()) {
      const std::string ref = std::move(pending.back());
      pending.pop_back();
      if (satellites_.contains(ref) || !satellite_nested_.insert(ref).second) continue;
      push_children(ref);
    }
  }

  static const google::protobuf::RepeatedPtrField<docv1::RefItem>& text_children(
      const docv1::BaseTextItem& item) {
    static const google::protobuf::RepeatedPtrField<docv1::RefItem> kNone;
    if (item.item_case() == docv1::BaseTextItem::kCode) return item.code().children();
    const auto* base = text_base(item);
    return base != nullptr ? base->children() : kNone;
  }

  // The children of the item a reference names, for the arenas the walk
  // descends into; nullptr for the others and for a dangling reference.
  const google::protobuf::RepeatedPtrField<docv1::RefItem>* children_of(
      const std::string& raw) const {
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        return ref.index < document_.texts_size() ? &text_children(document_.texts(ref.index))
                                                  : nullptr;
      case ArenaRef::kGroup:
        return ref.index < document_.groups_size() ? &document_.groups(ref.index).children()
                                                   : nullptr;
      case ArenaRef::kTable:
        return ref.index < document_.tables_size() ? &document_.tables(ref.index).children()
                                                   : nullptr;
      case ArenaRef::kPicture:
        return ref.index < document_.pictures_size()
                   ? &document_.pictures(ref.index).children()
                   : nullptr;
      case ArenaRef::kFieldRegion:
        return ref.index < document_.field_regions_size()
                   ? &document_.field_regions(ref.index).children()
                   : nullptr;
      case ArenaRef::kFieldItem:
        return ref.index < document_.field_items_size()
                   ? &document_.field_items(ref.index).children()
                   : nullptr;
      default: return nullptr;
    }
  }

  // What a text host's element holds: its own text with the runs of an
  // inline group joined on, and the children that are not runs, in order.
  struct HostContent {
    std::string text;
    std::vector<std::string> blocks;
    bool runs = false;
  };

  static void append_run(std::string& text, const std::string& run) {
    if (run.empty()) return;
    if (!text.empty()) text.push_back(' ');
    text.append(run);
  }

  // Folds one member of an inline flow into `content`: a text run joins the
  // text (its own children folding after it), a group's members fold in
  // turn, and anything else (a list, a table, a picture) stays a block.
  void append_inline(const std::string& raw, HostContent& content) {
    const ArenaRef ref = parse_ref(raw);
    if ((ref.kind != ArenaRef::kText && ref.kind != ArenaRef::kGroup) || list_group(ref)) {
      content.blocks.push_back(raw);
      return;
    }
    if (!consume(raw)) return;
    if (ref.kind == ArenaRef::kText) {
      if (ref.index >= document_.texts_size()) return;
      const auto& run = document_.texts(ref.index);
      const bool code = run.item_case() == docv1::BaseTextItem::kCode;
      const auto* base = text_base(run);
      if (!code && base == nullptr) return;
      if (excluded_layer(code ? run.code().content_layer() : base->content_layer())) return;
      append_run(content.text, code ? run.code().text() : base->text());
      for (const auto& child : text_children(run)) append_inline(child.ref(), content);
      return;
    }
    if (ref.index >= document_.groups_size()) return;
    const auto& group = document_.groups(ref.index);
    if (excluded_layer(group.content_layer())) return;
    for (const auto& child : group.children()) append_inline(child.ref(), content);
  }

  // Whether a reference names a list group, which keeps its structure even
  // inside an inline flow.
  bool list_group(const ArenaRef& ref) const {
    if (ref.kind != ArenaRef::kGroup || ref.index >= document_.groups_size()) return false;
    const auto label = document_.groups(ref.index).label();
    return label == docv1::GROUP_LABEL_LIST || label == docv1::GROUP_LABEL_ORDERED_LIST;
  }

  // A host's own text, then the runs of its first child when that child is
  // an inline group (a text item with no text of its own over its runs,
  // DocLang's mixed content): the runs are part of the host's text, never
  // items of their own.
  HostContent host_content(const docv1::BaseTextItem& item, const std::string& own) {
    HostContent content{own, {}, false};
    const auto& children = text_children(item);
    for (int index = 0; index < children.size(); ++index) {
      const std::string& raw = children[index].ref();
      const ArenaRef ref = parse_ref(raw);
      if (index == 0 && ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size() &&
          document_.groups(ref.index).label() == docv1::GROUP_LABEL_INLINE) {
        content.runs = true;
        append_inline(raw, content);
        continue;
      }
      content.blocks.push_back(raw);
    }
    return content;
  }

  // Where a host's block children go: after its element at the same depth,
  // inside it when the host has no text or runs of its own (a bare host's
  // children nest), or inside it always (a float's satellite).
  enum class Nest { kNever, kWhenBare, kAlways };

  // Renders a text host as the element `tag`. The text sits on the element's
  // line; with block children nested (or a `head` such as an <href/>) the
  // element opens on its own line and holds the text as a bare text line
  // followed by the blocks. An empty host with nothing nested is omitted
  // when `skip_empty`.
  void render_host(const docv1::BaseTextItem& item, const std::string& own,
                   const std::string& tag, const std::string& attributes, int depth, Nest nest,
                   bool skip_empty, const std::string& head = std::string()) {
    HostContent content = host_content(item, own);
    const bool inside = nest == Nest::kAlways ||
                        (nest == Nest::kWhenBare && own.empty() && !content.runs);
    std::string nested;
    if (inside) {
      std::swap(out_, nested);
      for (const auto& block : content.blocks) render_ref(block, depth + 1);
      std::swap(out_, nested);
    }
    if (content.text.empty() && nested.empty() && skip_empty) {
      if (!inside) {
        for (const auto& block : content.blocks) render_ref(block, depth);
      }
      return;
    }
    if (nested.empty() && head.empty()) {
      line(depth, element(tag, attributes, content.text));
    } else {
      line(depth, "<" + tag + attributes + ">");
      if (!head.empty()) line(depth + 1, head);
      if (!content.text.empty()) line(depth + 1, escape_xml_text(content.text));
      out_.append(nested);
      line(depth, "</" + tag + ">");
    }
    if (!inside) {
      for (const auto& block : content.blocks) render_ref(block, depth);
    }
  }

  void render_text(const docv1::BaseTextItem& item, int depth) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (excluded_layer(code.content_layer())) return;
      const std::string language = code_fence_language(code);
      const std::string attributes =
          language.empty() ? std::string()
                           : " language=\"" + escape_xml_attribute(language) + "\"";
      render_host(item, code.text(), "code", attributes, depth, Nest::kWhenBare, false);
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || excluded_layer(base->content_layer())) return;
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        render_host(item, base->text(), "title", "", depth, Nest::kWhenBare, false);
        return;
      case docv1::BaseTextItem::kSectionHeader:
        render_host(item, base->text(), "section-header",
                    " level=\"" + std::to_string(std::max(item.section_header().level(), 1)) +
                        "\"",
                    depth, Nest::kWhenBare, false);
        return;
      case docv1::BaseTextItem::kFormula:
        render_host(item, base->text(), "formula", "", depth, Nest::kWhenBare, false);
        return;
      case docv1::BaseTextItem::kListItem:
        // A nested list follows the item: inside it, grpc-xml would read the
        // list as part of the item's text.
        render_host(item, base->text(), "list-item", "", depth, Nest::kNever, false);
        return;
      default: break;
    }
    switch (base->label()) {
      case docv1::DOC_ITEM_LABEL_FOOTNOTE:
        render_host(item, base->text(), "footnote", "", depth, Nest::kWhenBare, false);
        return;
      case docv1::DOC_ITEM_LABEL_REFERENCE:
        render_host(item, base->text(), "reference", "", depth, Nest::kWhenBare, false);
        return;
      case docv1::DOC_ITEM_LABEL_CAPTION:
        render_host(item, base->text(), "caption", "", depth, Nest::kWhenBare, false);
        return;
      default:
        // A paragraph with no text adds nothing around its children, so
        // they render in its place.
        render_host(item, base->text(), "paragraph", "", depth, Nest::kNever, true);
        return;
    }
  }

  void render_group(const docv1::GroupItem& group, int depth) {
    if (excluded_layer(group.content_layer())) return;
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      render_list(group, depth);
      return;
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      // One flow of runs, so one paragraph, as the HTML export folds it.
      HostContent content;
      for (const auto& child : group.children()) append_inline(child.ref(), content);
      if (!content.text.empty()) line(depth, element("paragraph", "", content.text));
      for (const auto& block : content.blocks) render_ref(block, depth);
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
          if (!claimed(child.ref()) && consume(child.ref())) render_list(nested, depth + 1);
          continue;
        }
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) {
        // Anything else in a list renders in place, one level in.
        render_ref(child.ref(), depth + 1);
        continue;
      }
      if (claimed(child.ref()) || !consume(child.ref())) continue;
      const auto& item = document_.texts(ref.index);
      const auto* base = text_base(item);
      if (base == nullptr || excluded_layer(base->content_layer())) continue;
      ++ordinal;
      const std::string attributes =
          ordered ? " ordinal=\"" + std::to_string(ordinal) + "\"" : std::string();
      render_host(item, base->text(), "list-item", attributes, depth + 1, Nest::kNever, false);
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

  // A float's captions (before it) or footnotes (after it), each as the
  // element `tag` holding its own text, the runs of an inline group, and
  // whatever else is nested under it (a field region in an otherwise empty
  // footnote), the last one level in. A satellite with none of these is
  // omitted. A linked caption takes the block form: an <href/> head
  // naming the normalized target, then the caption text as a bare text line.
  void render_satellites(const google::protobuf::RepeatedPtrField<docv1::RefItem>& refs,
                         const std::string& tag, int depth) {
    for (const auto& ref : refs) {
      const ArenaRef parsed = parse_ref(ref.ref());
      if (parsed.kind != ArenaRef::kText || parsed.index >= document_.texts_size()) continue;
      if (!consume(ref.ref())) continue;
      const auto& item = document_.texts(parsed.index);
      const auto* base = text_base(item);
      if (base == nullptr || excluded_layer(base->content_layer())) continue;
      const std::string head =
          base->has_hyperlink()
              ? "<href uri=\"" + escape_xml_attribute(normalized_uri(base->hyperlink())) + "\"/>"
              : std::string();
      ++satellite_depth_;
      render_host(item, base->text(), tag, "", depth, Nest::kAlways, true, head);
      --satellite_depth_;
    }
  }

  // A rich cell (docling's RichTableCell.ref) opens its element and renders
  // the referenced group's blocks inside it, one indent deeper; the group
  // reference is consumed so the blocks never render twice. Returns false
  // when the cell carries no usable group reference, leaving the plain
  // single-line form to the caller.
  bool render_cell_blocks(const docv1::TableCell* cell, const std::string& tag,
                          const std::string& attributes, int depth) {
    if (!cell->has_ref()) return false;
    const ArenaRef ref = parse_ref(cell->ref().ref());
    if (ref.kind != ArenaRef::kGroup || ref.index >= document_.groups_size()) {
      return false;
    }
    if (!consume(cell->ref().ref())) return false;
    const auto& group = document_.groups(ref.index);
    if (excluded_layer(group.content_layer())) return false;
    line(depth, "<" + tag + attributes + ">");
    render_group(group, depth + 1);
    line(depth, "</" + tag + ">");
    return true;
  }

  void render_table(const docv1::TableItem& table, int depth) {
    if (excluded_layer(table.content_layer())) return;
    render_satellites(table.captions(), "caption", depth);
    render_grid(table, depth);
    render_satellites(table.footnotes(), "footnote", depth);
  }

  void render_grid(const docv1::TableItem& table, int depth) {
    const auto grid = table_grid(table.data(), grid_budget_);
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
        const std::string tag = header ? "th" : "td";
        if (render_cell_blocks(cell, tag, attributes, depth + 2)) continue;
        line(depth + 2, element(tag, attributes, cell->text()));
      }
      line(depth + 1, "</tr>");
    }
    line(depth, "</table>");
  }

  void render_picture(const docv1::PictureItem& picture, int depth) {
    if (excluded_layer(picture.content_layer())) return;
    render_satellites(picture.captions(), "caption", depth);
    const std::string uri = picture_uri_ ? picture_uri_(picture) : std::string();
    const std::string open =
        uri.empty() ? std::string("<picture")
                    : "<picture uri=\"" + escape_xml_attribute(uri) + "\"";
    // A picture description rides as a nested description element.
    const std::string description = trimmed(picture_description(picture));
    if (description.empty()) {
      line(depth, open + "/>");
    } else {
      line(depth, open + ">");
      line(depth + 1, element("description", "", description));
      line(depth, "</picture>");
    }
    render_satellites(picture.footnotes(), "footnote", depth);
  }
};

}  // namespace

namespace render {

std::string render_doclang_markup(const docv1::Document& document, bool include_namespace,
                                  const PictureUri& picture_uri) {
  return DoclangRenderer(document, include_namespace, picture_uri).render();
}

}  // namespace render

std::string render_doclang(const docv1::Document& document) {
  return render_doclang(document, DoclangOptions{});
}

std::string render_doclang(const docv1::Document& document, const DoclangOptions& options) {
  using Mode = DoclangOptions::ImageMode;
  // docling-core's export_to_doclang default.
  const Mode mode = options.image_mode.value_or(Mode::kPlaceholder);
  render::PictureUri picture_uri;
  switch (mode) {
    case Mode::kPlaceholder:
      // No source on the picture element.
      break;
    case Mode::kReferenced:
      // The picture's existing image uri, whatever it is.
      picture_uri = [](const docv1::PictureItem& picture) {
        return picture.has_image() ? picture.image().uri() : std::string();
      };
      break;
    case Mode::kEmbedded:
      // The existing uri when there is one (docling writes it as is, data
      // URI or not), otherwise the picture cropped out of its page image.
      picture_uri = [&document](const docv1::PictureItem& picture) {
        if (picture.has_image() && !picture.image().uri().empty()) return picture.image().uri();
        const auto png = render::crop_picture_png(document, picture);
        return png.has_value() ? render::png_data_uri(*png) : std::string();
      };
      break;
  }
  return render::render_doclang_markup(document, options.include_namespace, picture_uri);
}

}  // namespace grparse
