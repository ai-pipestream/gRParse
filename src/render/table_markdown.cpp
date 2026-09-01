#include "table_markdown.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "display_width.h"
#include "markdown_text.h"
#include "renderer_base.h"
#include "value_repr.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {
namespace {

// The reference table formatter's minimum header padding.
constexpr int kMinTablePadding = 2;

// A cell must not break the row or the column separator.
std::string row_safe(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    if (c == '\n') {
      safe.push_back(' ');
    } else if (c == '|') {
      safe.append("&#124;");
    } else {
      safe.push_back(c);
    }
  }
  return safe;
}

// The alignment of every column: right when the data below the header folds
// to a number.
std::vector<bool> column_alignments(
    const std::vector<std::vector<std::string>>& rows, std::size_t columns) {
  std::vector<bool> right_aligned(columns, false);
  if (rows.size() <= 1) return right_aligned;
  for (std::size_t col = 0; col < columns; ++col) {
    std::vector<std::string> values;
    values.reserve(rows.size() - 1);
    for (std::size_t row = 1; row < rows.size(); ++row) {
      values.push_back(col < rows[row].size() ? rows[row][col] : std::string());
    }
    right_aligned[col] = column_is_numeric(values);
  }
  return right_aligned;
}

// The width of every column: the header width plus the minimum padding, never
// narrower than the widest stripped data cell.
std::vector<int> column_widths(const std::vector<std::vector<std::string>>& rows,
                               std::size_t columns) {
  const std::vector<std::string>& headers = rows.front();
  std::vector<int> widths(columns, 0);
  for (std::size_t col = 0; col < columns; ++col) {
    widths[col] = display_width(headers[col]) + kMinTablePadding;
    for (std::size_t row = 1; row < rows.size(); ++row) {
      if (col >= rows[row].size()) continue;
      widths[col] = std::max(widths[col], display_width(stripped(rows[row][col])));
    }
  }
  return widths;
}

std::string pad(const std::string& cell, int width, bool right) {
  const int fill = std::max(width - display_width(cell), 0);
  return right ? std::string(static_cast<std::size_t>(fill), ' ') + cell
               : cell + std::string(static_cast<std::size_t>(fill), ' ');
}

std::string build_row(const std::vector<std::string>& cells, bool strip,
                      const std::vector<int>& widths,
                      const std::vector<bool>& right_aligned) {
  std::string line = "|";
  for (std::size_t col = 0; col < widths.size(); ++col) {
    const std::string cell = col < cells.size()
                                 ? (strip ? stripped(cells[col]) : cells[col])
                                 : std::string();
    line.append(" ");
    line.append(pad(cell, widths[col], right_aligned[col]));
    line.append(" |");
  }
  return line;
}

std::string build_rule(const std::vector<int>& widths) {
  std::string rule = "|";
  for (const int width : widths) {
    rule.append(static_cast<std::size_t>(width) + 2, '-');
    rule.push_back('|');
  }
  return rule;
}

}  // namespace

std::vector<std::vector<std::string>> table_rows(
    const docv1::TableData& data, const CellTextResolver& resolve_ref) {
  const auto grid = derived_table_grid(data);
  std::vector<std::vector<std::string>> out;
  out.reserve(grid.size());
  for (const auto& row : grid) {
    std::vector<std::string> texts;
    texts.reserve(row.size());
    for (const auto* cell : row) {
      std::string text;
      if (cell != nullptr) {
        text = cell->has_ref() ? resolve_ref(cell->ref().ref()) : cell->text();
      }
      texts.push_back(row_safe(text));
    }
    out.push_back(std::move(texts));
  }
  return out;
}

std::string table_markdown(const docv1::TableData& data,
                           const CellTextResolver& resolve_ref) {
  const auto rows = table_rows(data, resolve_ref);
  if (rows.empty()) return std::string();
  const std::size_t columns = rows.front().size();
  const std::vector<bool> right_aligned = column_alignments(rows, columns);
  const std::vector<int> widths = column_widths(rows, columns);

  std::vector<std::string> lines;
  lines.push_back(build_row(rows.front(), false, widths, right_aligned));
  lines.push_back(build_rule(widths));
  for (std::size_t row = 1; row < rows.size(); ++row) {
    lines.push_back(build_row(rows[row], true, widths, right_aligned));
  }
  return join(lines, "\n");
}

}  // namespace grparse::render
