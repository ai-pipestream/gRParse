#include "renderer_base.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {

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
  for (const auto& [prefix, kind] : kArenas) {
    if (!ref.starts_with(prefix)) continue;
    const std::string digits = ref.substr(prefix.size());
    // Nine digits keeps the index inside int range; anything longer cannot
    // name a real arena entry and resolves to kUnknown like other malformed
    // references.
    if (digits.empty() || digits.size() > 9 ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
      return {};
    }
    return {kind, std::stoi(digits)};
  }
  return {};
}

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

int heading_rank(int level) {
  return std::min(std::max(level, 1) + 1, 6);
}

std::string trimmed(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n\f\v");
  if (begin == std::string::npos) return std::string();
  const auto end = text.find_last_not_of(" \t\r\n\f\v");
  return text.substr(begin, end - begin + 1);
}

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
  if (name.starts_with(kPrefix)) name = name.substr(kPrefix.size());
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

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

std::string picture_description(const docv1::PictureItem& picture) {
  if (picture.has_meta() && picture.meta().has_description()) {
    return picture.meta().description().text();
  }
  for (const auto& annotation : picture.annotations()) {
    if (annotation.has_description()) return annotation.description().text();
  }
  return std::string();
}

std::string picture_classification_class(const docv1::PictureItem& picture) {
  if (picture.has_meta() && picture.meta().has_classification()) {
    std::string predicted_class;
    double best = -1.0;
    for (const auto& prediction : picture.meta().classification().predictions()) {
      const double confidence =
          prediction.has_confidence() ? prediction.confidence() : 0.0;
      if (confidence > best) {
        best = confidence;
        predicted_class = prediction.class_name();
      }
    }
    return predicted_class;
  }
  for (const auto& annotation : picture.annotations()) {
    if (annotation.has_classification() &&
        !annotation.classification().predicted_classes().empty()) {
      return annotation.classification().predicted_classes(0).class_name();
    }
  }
  return std::string();
}

std::vector<std::string> RendererBase::caption_texts(
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

}  // namespace grparse::render
