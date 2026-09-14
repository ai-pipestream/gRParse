#include "table_markdown.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
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

using Rows = std::vector<std::vector<std::string>>;

// The alignment of every column: right when the body cells below the header
// fold to a number.
std::vector<bool> column_alignments(const Rows& body, std::size_t columns) {
  std::vector<bool> right_aligned(columns, false);
  if (body.empty()) return right_aligned;
  for (std::size_t col = 0; col < columns; ++col) {
    std::vector<std::string> values;
    values.reserve(body.size());
    for (const auto& row : body) {
      values.push_back(col < row.size() ? row[col] : std::string());
    }
    right_aligned[col] = column_is_numeric(values);
  }
  return right_aligned;
}

// The width of every column: the header width plus the minimum padding, never
// narrower than the widest stripped body cell.
std::vector<int> column_widths(const std::vector<std::string>& headers, const Rows& body,
                               std::size_t columns) {
  std::vector<int> widths(columns, 0);
  for (std::size_t col = 0; col < columns; ++col) {
    widths[col] = display_width(headers[col]) + kMinTablePadding;
    for (const auto& row : body) {
      if (col >= row.size()) continue;
      widths[col] = std::max(widths[col], display_width(stripped(row[col])));
    }
  }
  return widths;
}

// The reference's HEADER_ROW_SEPARATOR: what joins the cells of a stacked
// column header into the one header row GFM allows.
constexpr std::string_view kHeaderRowSeparator = " - ";

// Per column, the header rows' texts joined top to bottom, an empty text
// and a repeat of the text just above it (a row-spanning cell the grid
// repeats into every row it covers) dropped.
std::vector<std::string> flatten_header_rows(const Rows& header_rows, std::size_t columns) {
  std::vector<std::string> flattened(columns);
  if (header_rows.empty()) return flattened;
  for (std::size_t col = 0; col < columns; ++col) {
    std::vector<std::string> parts;
    for (const auto& row : header_rows) {
      const std::string text = col < row.size() ? row[col] : std::string();
      if (!text.empty() && (parts.empty() || parts.back() != text)) parts.push_back(text);
    }
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) joined.append(kHeaderRowSeparator);
      joined.append(parts[i]);
    }
    flattened[col] = std::move(joined);
  }
  return flattened;
}

// docling-core's _compact_table over the padded text: every cell stripped,
// the rule row reduced to one dash per column with its alignment marks kept.
std::string compact_table(const std::string& padded) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  std::size_t line_index = 0;
  while (start <= padded.size()) {
    const std::size_t end = padded.find('\n', start);
    const std::string line =
        padded.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const std::size_t index = line_index++;
    start = end == std::string::npos ? padded.size() + 1 : end + 1;
    if (line.empty()) continue;
    // The cells between the outer pipes.
    std::vector<std::string> cells;
    std::size_t cell_start = line.find('|');
    if (cell_start == std::string::npos) continue;
    ++cell_start;
    while (true) {
      const std::size_t pipe = line.find('|', cell_start);
      if (pipe == std::string::npos) break;
      cells.push_back(line.substr(cell_start, pipe - cell_start));
      cell_start = pipe + 1;
    }
    std::string compact = "|";
    for (std::size_t i = 0; i < cells.size(); ++i) {
      std::string cell = stripped(cells[i]);
      if (index == 1) {
        const bool left = cell.starts_with(':');
        const bool right = cell.ends_with(':');
        cell = left && right ? ":-:" : left ? ":-" : right ? "-:" : "-";
      }
      compact.append(" ").append(cell).append(" |");
    }
    lines.push_back(std::move(compact));
  }
  return join(lines, "\n");
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

std::size_t count_header_rows(const std::vector<std::vector<const docv1::TableCell*>>& grid) {
  const auto starts_header_on = [](const std::vector<const docv1::TableCell*>& row,
                                   std::size_t row_idx) {
    return std::ranges::any_of(row, [row_idx](const docv1::TableCell* cell) {
      return cell != nullptr && cell->column_header() &&
             cell->start_row_offset_idx() == static_cast<int>(row_idx);
    });
  };
  const auto any_header_below_first = [&grid] {
    for (std::size_t row_idx = 1; row_idx < grid.size(); ++row_idx) {
      for (const docv1::TableCell* cell : grid[row_idx]) {
        if (cell != nullptr && cell->column_header()) return true;
      }
    }
    return false;
  };
  std::size_t num_headers = 0;
  for (std::size_t row_idx = 0; row_idx < grid.size(); ++row_idx) {
    if (starts_header_on(grid[row_idx], row_idx)) {
      ++num_headers;
      continue;
    }
    if (row_idx == 0 && !any_header_below_first()) return 1;
    break;
  }
  return num_headers;
}

std::string table_markdown(const docv1::TableData& data,
                           const CellTextResolver& resolve_ref, bool compact) {
  const Rows rows = table_rows(data, resolve_ref);
  if (rows.empty()) return std::string();
  const std::size_t columns = rows.front().size();
  const std::size_t num_headers = std::min(count_header_rows(derived_table_grid(data)), rows.size());
  const Rows header_rows(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(num_headers));
  const Rows body(rows.begin() + static_cast<std::ptrdiff_t>(num_headers), rows.end());
  const std::vector<std::string> headers = flatten_header_rows(header_rows, columns);
  const std::vector<bool> right_aligned = column_alignments(body, columns);
  const std::vector<int> widths = column_widths(headers, body, columns);

  std::vector<std::string> lines;
  lines.push_back(build_row(headers, false, widths, right_aligned));
  lines.push_back(build_rule(widths));
  for (const auto& row : body) {
    lines.push_back(build_row(row, true, widths, right_aligned));
  }
  const std::string padded = join(lines, "\n");
  return compact ? compact_table(padded) : padded;
}

}  // namespace grparse::render
