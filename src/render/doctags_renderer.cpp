// The DocTags renderer behind render_doctags; semantics documented on the
// declaration in include/grparse/document_render.h.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "grparse/document_render.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

// The docling short name of a DocItemLabel ("section_header", "text",
// "checkbox_selected"), which doubles as the DocTags token vocabulary.
std::string label_short_name(docv1::DocItemLabel label) {
  std::string name = docv1::DocItemLabel_Name(label);
  static const std::string kPrefix = "DOC_ITEM_LABEL_";
  if (name.starts_with(kPrefix)) name = name.substr(kPrefix.size());
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

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
  if (name.starts_with(kPrefix)) name = name.substr(kPrefix.size());
  std::ranges::transform(name, name.begin(),
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
      if (excluded_layer(code.content_layer())) return std::string();
      // Code keeps its whitespace; the language token follows the location.
      return wrap("code", location_tokens(code.prov()) + "<_" +
                              doctags_code_language(code) + "_>" + code.text());
    }
    const auto* base = text_base(item);
    if (base == nullptr || excluded_layer(base->content_layer())) return std::string();
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
    if (excluded_layer(group.content_layer())) return std::string();
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
          if (base == nullptr || excluded_layer(base->content_layer())) continue;
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
    if (excluded_layer(table.content_layer())) return std::string();
    const std::string body =
        location_tokens(table.prov()) + otsl_cells(table.data()) +
        caption_block(table.captions());
    return body.empty() ? body : wrap("otsl", body);
  }

  std::string render_picture(const docv1::PictureItem& picture) {
    if (excluded_layer(picture.content_layer())) return std::string();
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
    if (excluded_layer(item.content_layer())) return std::string();
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
      if (label.starts_with(kPrefix)) label = label.substr(kPrefix.size());
      std::ranges::transform(label, label.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      body.append(wrap(label + "_" + std::to_string(cell.cell_id()), cell_text));
    }
    body.append(caption_block(item.captions()));
    return wrap("key_value_region", body);
  }
};

}  // namespace

std::string render_doctags(const docv1::Document& document) {
  return DocTagsRenderer(document).render();
}

}  // namespace grparse
