#include "grparse/office_fold/chart_fold.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "grparse/data_totals.h"
#include "grparse/office_fold/grid_cells.h"
#include "grparse/office_fold/shape_fold.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// The name the composite goes by: the embedded object's, or the sheet
// event's when only that one names it.
std::string chart_name(const officev1::EmbeddedObject* object,
                       const officev1::SheetChart* sheet_chart) {
  if (object != nullptr && !object->name().empty()) return object->name();
  if (sheet_chart != nullptr) return sheet_chart->name();
  return std::string();
}

// The rendered chart the container stored beside the data.
void set_replacement_image(const officev1::EmbeddedObject& object,
                           docv1::PictureItem* picture) {
  if (object.replacement_image().empty()) return;
  docv1::ImageRef* ref = picture->mutable_image();
  ref->set_mimetype(object.replacement_mime_type());
  ref->mutable_size()->set_width(static_cast<double>(object.width_twips()));
  ref->mutable_size()->set_height(static_cast<double>(object.height_twips()));
  ref->set_uri(data_uri(object.replacement_mime_type(),
                        object.replacement_image()));
}

// Where the chart's data came from, as grid spans on the sheet it sits on.
void set_chart_sources(const officev1::SheetChart& chart,
                       const std::string& sheet, docv1::ChartMeta* sources) {
  for (const officev1::SheetRangeRef& range : chart.ranges()) {
    set_grid_span(range, sheet, sources->add_sources());
  }
  sources->set_has_column_headers(chart.has_column_headers());
  sources->set_has_row_headers(chart.has_row_headers());
}

// The bar annotation is single-series by shape, so a clustered chart gets
// one annotation per series, in series order (the stacked-bar slot carries
// integer values only and would round the data). The axis labels stay the
// chart's own axis titles on every annotation; the series names live on the
// bound table's label row, in the same order. A typed multi-series slot is
// a schema follow-on.
void add_bar_annotations(const officev1::EmbeddedChart& chart,
                         docv1::PictureItem* picture) {
  for (const officev1::EmbeddedChartSeries& one : chart.series()) {
    docv1::PictureBarChartData* bars =
        picture->add_annotations()->mutable_bar_chart();
    bars->set_kind("bar_chart_data");
    bars->set_title(chart.title());
    bars->set_x_axis_label(chart.x_axis_title());
    bars->set_y_axis_label(chart.y_axis_title());
    for (int i = 0; i < one.values_y_size(); i++) {
      docv1::ChartBar* bar = bars->add_bars();
      bar->set_label(i < chart.categories_size() ? chart.categories(i)
                                                 : std::to_string(i + 1));
      bar->set_values(one.values_y(i));
    }
  }
}

void add_line_annotation(const officev1::EmbeddedChart& chart,
                         docv1::PictureItem* picture) {
  docv1::PictureLineChartData* lines =
      picture->add_annotations()->mutable_line_chart();
  lines->set_kind("line_chart_data");
  lines->set_title(chart.title());
  lines->set_x_axis_label(chart.x_axis_title());
  lines->set_y_axis_label(chart.y_axis_title());
  for (const officev1::EmbeddedChartSeries& one : chart.series()) {
    docv1::ChartLine* line = lines->add_lines();
    line->set_label(one.label());
    for (int i = 0; i < one.values_y_size(); i++) {
      docv1::FloatPair* pair = line->add_values();
      pair->set_first(i < one.values_x_size() ? one.values_x(i)
                                              : static_cast<double>(i));
      pair->set_second(one.values_y(i));
    }
  }
}

void add_pie_annotation(const officev1::EmbeddedChart& chart,
                        docv1::PictureItem* picture) {
  const auto& series = chart.series();
  if (series.empty()) return;
  docv1::PicturePieChartData* pie =
      picture->add_annotations()->mutable_pie_chart();
  pie->set_kind("pie_chart_data");
  pie->set_title(chart.title());
  for (int i = 0; i < series[0].values_y_size(); i++) {
    docv1::ChartSlice* slice = pie->add_slices();
    slice->set_label(i < chart.categories_size() ? chart.categories(i)
                                                 : std::to_string(i + 1));
    slice->set_value(series[0].values_y(i));
  }
}

void add_scatter_annotation(const officev1::EmbeddedChart& chart,
                            docv1::PictureItem* picture) {
  docv1::PictureScatterChartData* scatter =
      picture->add_annotations()->mutable_scatter_chart();
  scatter->set_kind("scatter_chart_data");
  scatter->set_title(chart.title());
  scatter->set_x_axis_label(chart.x_axis_title());
  scatter->set_y_axis_label(chart.y_axis_title());
  for (const officev1::EmbeddedChartSeries& one : chart.series()) {
    int points = std::min(one.values_x_size(), one.values_y_size());
    for (int i = 0; i < points; i++) {
      docv1::FloatPair* pair = scatter->add_points()->mutable_value();
      pair->set_first(one.values_x(i));
      pair->set_second(one.values_y(i));
    }
  }
}

// A series without a label is named by its position, in the schema and on
// the label row alike, so the two never disagree.
std::string series_label(const officev1::EmbeddedChart& chart, int column) {
  std::string label = chart.series(column).label();
  if (!label.empty()) return label;
  return chart.series_size() == 1 && !chart.y_axis_title().empty()
             ? chart.y_axis_title()
             : "Series " + std::to_string(column + 1);
}

// The schema: the first column is the category (or x) axis, every other one
// a series; the axis titles name them when the chart states them.
void add_series_columns(const officev1::EmbeddedChart& chart, bool scatter,
                        docv1::TableData* data) {
  docv1::TableColumnSchema* axis = data->add_columns();
  if (!chart.x_axis_title().empty()) axis->set_name(chart.x_axis_title());
  axis->set_declared_type(scatter ? "number" : "text");
  for (int column = 0; column < chart.series_size(); column++) {
    docv1::TableColumnSchema* schema = data->add_columns();
    schema->set_name(series_label(chart, column));
    schema->set_declared_type("number");
  }
}

// The body: categories (or x values) down the first column, one series per
// further column, numbers typed.
void add_series_rows(const officev1::EmbeddedChart& chart, bool scatter,
                     int body_rows, docv1::TableData* data) {
  const auto& series = chart.series();
  for (int row = 0; row < body_rows; row++) {
    docv1::TableCell* head;
    if (row < chart.categories_size()) {
      head = place_cell(data, row + 1, 0, chart.categories(row));
    } else if (scatter && !series.empty() && row < series[0].values_x_size()) {
      head = place_cell(data, row + 1, 0, double_text(series[0].values_x(row)));
      head->mutable_value()->set_number(series[0].values_x(row));
    } else {
      head = place_cell(data, row + 1, 0, std::to_string(row + 1));
    }
    head->set_row_header(true);
    for (int column = 0; column < series.size(); column++) {
      const officev1::EmbeddedChartSeries& one = series[column];
      if (row >= one.values_y_size()) continue;
      docv1::TableCell* cell =
          place_cell(data, row + 1, column + 1, double_text(one.values_y(row)));
      cell->mutable_value()->set_number(one.values_y(row));
    }
  }
}

// The tabular projection of a chart bound from its own tabular arm: the
// first row heads the columns, the first column heads the rows.
void mark_tabular_headers(docv1::TableData* data) {
  for (docv1::TableCell& cell : *data->mutable_table_cells()) {
    if (cell.start_row_offset_idx() == 0) cell.set_column_header(true);
    if (cell.start_col_offset_idx() == 0 && cell.start_row_offset_idx() > 0) {
      cell.set_row_header(true);
    }
  }
}

}  // namespace

void ChartFold::hold(const officev1::EmbeddedObject& object) {
  pending_charts_[object.page_index()].push_back(object);
}

bool ChartFold::take_pending(int page_index, const officev1::TwipsPoint* at,
                             officev1::EmbeddedObject* out) {
  auto found = pending_charts_.find(page_index);
  if (found == pending_charts_.end() || found->second.empty()) return false;
  std::deque<officev1::EmbeddedObject>& waiting = found->second;
  auto picked = waiting.end();
  if (at == nullptr) {
    picked = waiting.begin();
  } else {
    // Positions come from the same model geometry on both events; a twip
    // of slack covers unit rounding.
    for (auto it = waiting.begin(); it != waiting.end(); ++it) {
      if (std::llabs(it->position().x() - at->x()) <= 1 &&
          std::llabs(it->position().y() - at->y()) <= 1) {
        picked = it;
        break;
      }
    }
  }
  if (picked == waiting.end()) return false;
  *out = std::move(*picked);
  waiting.erase(picked);
  if (waiting.empty()) pending_charts_.erase(found);
  return true;
}

void ChartFold::add_annotations(const officev1::EmbeddedChart& chart,
                                docv1::PictureItem* picture) {
  switch (chart.kind()) {
    case officev1::EMBEDDED_CHART_KIND_BAR:
    case officev1::EMBEDDED_CHART_KIND_COLUMN:
      add_bar_annotations(chart, picture);
      break;
    case officev1::EMBEDDED_CHART_KIND_LINE:
    case officev1::EMBEDDED_CHART_KIND_AREA:
      add_line_annotation(chart, picture);
      break;
    case officev1::EMBEDDED_CHART_KIND_PIE:
      add_pie_annotation(chart, picture);
      break;
    case officev1::EMBEDDED_CHART_KIND_SCATTER:
    case officev1::EMBEDDED_CHART_KIND_BUBBLE:
      add_scatter_annotation(chart, picture);
      break;
    default:
      break;
  }
  // The tabular projection is always attached: any chart family stays
  // representable, including kinds with no typed variant above.
  docv1::PictureTabularChartData* tabular =
      picture->add_annotations()->mutable_tabular_chart();
  tabular->set_kind("tabular_chart_data");
  tabular->set_title(chart.title());
  docv1::TableItem scratch;
  arena_.fold_table(chart.tabular(), &scratch);
  *tabular->mutable_chart_data() = scratch.data();
}

void ChartFold::fold_series(const officev1::EmbeddedChart& chart,
                            docv1::TableData* data) {
  bool scatter = false;
  int body_rows = chart.categories_size();
  for (const officev1::EmbeddedChartSeries& one : chart.series()) {
    if (one.values_x_size() > 0) scatter = true;
    body_rows = std::max(body_rows, one.values_y_size());
  }
  data->set_num_rows(body_rows + 1);
  data->set_num_cols(chart.series_size() + 1);
  add_series_columns(chart, scatter, data);
  // The label row: the axis title over the categories, a series label over
  // each value column.
  place_cell(data, 0, 0, chart.x_axis_title())->set_column_header(true);
  for (int column = 0; column < chart.series_size(); column++) {
    place_cell(data, 0, column + 1, series_label(chart, column))
        ->set_column_header(true);
  }
  add_series_rows(chart, scatter, body_rows, data);
  fill_grid_from_cells(data);
}

void ChartFold::name_corner(const officev1::SheetChart& chart,
                            docv1::TableData* data) {
  // A chart without a category axis title (a pie, an untitled axis) leaves
  // the corner blank; the sheet's own header cell over the category column
  // names it when the source range starts on a header row. Nothing is
  // invented: the text is the sheet's, at the range's top-left cell.
  if (!chart.has_column_headers() || chart.ranges().empty()) return;
  docv1::TableCell* corner = nullptr;
  for (docv1::TableCell& cell : *data->mutable_table_cells()) {
    if (cell.start_row_offset_idx() == 0 && cell.start_col_offset_idx() == 0) {
      corner = &cell;
    }
  }
  if (corner == nullptr || !corner->text().empty()) return;
  const docv1::TableData* sheet = sheets_.sheet_data(chart.sheet_index());
  if (sheet == nullptr) return;
  const officev1::SheetRangeRef& range = chart.ranges(0);
  for (const docv1::TableCell& cell : sheet->table_cells()) {
    if (cell.start_row_offset_idx() != range.start_row() ||
        cell.start_col_offset_idx() != range.start_column()
        || cell.text().empty()) {
      continue;
    }
    corner->set_text(cell.text());
    if (data->columns_size() > 0 && data->columns(0).name().empty()) {
      data->mutable_columns(0)->set_name(cell.text());
    }
    // The grid was materialized from the cells already; its corner slot
    // mirrors the change.
    if (data->grid_size() > 0 && data->grid(0).cells_size() > 0) {
      data->mutable_grid(0)->mutable_cells(0)->set_text(cell.text());
    }
    return;
  }
}

bool ChartFold::fold_sheet_range(const officev1::SheetChart& chart,
                                 docv1::TableData* data) {
  const docv1::TableData* sheet = sheets_.sheet_data(chart.sheet_index());
  if (sheet == nullptr || chart.ranges().empty()) return false;
  int top = chart.ranges(0).start_row();
  int left = chart.ranges(0).start_column();
  int bottom = chart.ranges(0).end_row();
  int right = chart.ranges(0).end_column();
  for (const officev1::SheetRangeRef& range : chart.ranges()) {
    top = std::min(top, range.start_row());
    left = std::min(left, range.start_column());
    bottom = std::max(bottom, range.end_row());
    right = std::max(right, range.end_column());
  }
  if (bottom < top || right < left) return false;
  data->set_num_rows(bottom - top + 1);
  data->set_num_cols(right - left + 1);
  for (const docv1::TableCell& cell : sheet->table_cells()) {
    const int row = cell.start_row_offset_idx();
    const int column = cell.start_col_offset_idx();
    if (row < top || row > bottom || column < left || column > right) continue;
    docv1::TableCell* out = data->add_table_cells();
    *out = cell;
    out->set_start_row_offset_idx(row - top);
    out->set_end_row_offset_idx(
        std::min(cell.end_row_offset_idx(), bottom + 1) - top);
    out->set_start_col_offset_idx(column - left);
    out->set_end_col_offset_idx(
        std::min(cell.end_col_offset_idx(), right + 1) - left);
    out->set_row_span(out->end_row_offset_idx() - out->start_row_offset_idx());
    out->set_col_span(out->end_col_offset_idx() - out->start_col_offset_idx());
    if (chart.has_column_headers() && row == top) out->set_column_header(true);
    if (chart.has_row_headers() && column == left) out->set_row_header(true);
  }
  fill_grid_from_cells(data);
  return true;
}

docv1::PictureItem* ChartFold::add_chart_picture(
    const officev1::EmbeddedObject* object,
    const officev1::SheetChart* sheet_chart, const std::string& name,
    const std::string& sheet, const std::string& parent_ref,
    docv1::ContentLayer layer, bool page_local, int page_index, double l,
    double t, double r, double b, std::string* picture_ref) {
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_CHART, layer, parent_ref, picture_ref);
  if (!name.empty()) picture->mutable_shape()->set_name(name);
  if (object != nullptr) {
    attachments_.register_object(*object, *picture_ref);
    set_replacement_image(*object, picture);
  }
  arena_.add_prov(picture->mutable_prov(), page_index, page_local, l, t, r, b,
                  0, 0);
  if (sheet_chart != nullptr) {
    set_chart_sources(*sheet_chart, sheet, picture->mutable_chart());
  }
  return picture;
}

bool ChartFold::bind_chart_data(const officev1::EmbeddedObject* object,
                                const officev1::SheetChart* sheet_chart,
                                bool typed, docv1::TableItem* table) {
  if (typed && !object->chart().series().empty()) {
    fold_series(object->chart(), table->mutable_data());
    if (sheet_chart != nullptr) name_corner(*sheet_chart, table->mutable_data());
    return true;
  }
  if (typed && object->chart().has_tabular()
      && object->chart().tabular().rows() > 0) {
    arena_.fold_table(object->chart().tabular(), table);
    mark_tabular_headers(table->mutable_data());
    return true;
  }
  if (sheet_chart == nullptr) return false;
  return fold_sheet_range(*sheet_chart, table->mutable_data());
}

void ChartFold::add_row_provenance(const officev1::SheetChart& chart,
                                   const std::string& sheet,
                                   docv1::TableItem* table) {
  const officev1::SheetRangeRef& range = chart.ranges(0);
  const int first_sheet_row =
      range.start_row() - (chart.has_column_headers() ? 0 : 1);
  for (int row = 0; row < table->data().num_rows(); row++) {
    const int sheet_row = first_sheet_row + row;
    if (sheet_row < range.start_row() || sheet_row > range.end_row()) continue;
    docv1::ProvenanceItem* row_prov = table->mutable_data()->add_row_prov();
    row_prov->set_page_no(chart.sheet_index() + 1);
    docv1::GridCell* grid = row_prov->mutable_grid();
    grid->set_row(sheet_row);
    grid->set_col(range.start_column());
    if (!sheet.empty()) grid->set_sheet(sheet);
  }
}

void ChartFold::add_caption(const std::string& title,
                            docv1::PictureItem* picture,
                            const std::string& picture_ref,
                            docv1::ContentLayer layer, bool page_local,
                            int page_index, double l, double t, double r,
                            double b) {
  TextHandle caption = arena_.add_text(
      TextKind::kText, docv1::DOC_ITEM_LABEL_CAPTION, layer, picture_ref);
  caption.base->set_text(title);
  caption.base->set_orig(title);
  arena_.add_prov(caption.base->mutable_prov(), page_index, page_local, l, t, r,
                  b, 0, static_cast<long long>(title.size()));
  picture->add_captions()->set_ref(caption.ref);
  data_counters().chart_captions.fetch_add(1, std::memory_order_relaxed);
}

void ChartFold::emit(const officev1::EmbeddedObject* object,
                     const officev1::SheetChart* sheet_chart,
                     const std::string& parent_ref, docv1::ContentLayer layer,
                     bool page_local, int page_index, double l, double t,
                     double r, double b) {
  const std::string name = chart_name(object, sheet_chart);
  const std::string sheet = sheet_chart != nullptr
      ? sheets_.label(sheet_chart->sheet_index())
      : std::string();
  std::string picture_ref;
  docv1::PictureItem* picture =
      add_chart_picture(object, sheet_chart, name, sheet, parent_ref, layer,
                        page_local, page_index, l, t, r, b, &picture_ref);
  const bool typed = object != nullptr && object->has_chart();
  if (typed) add_annotations(object->chart(), picture);

  // The data, bound as the chart's own table.
  std::string table_ref;
  docv1::TableItem* table = arena_.add_table(layer, picture_ref, &table_ref);
  const bool folded = bind_chart_data(object, sheet_chart, typed, table);
  arena_.add_prov(table->mutable_prov(), page_index, page_local, l, t, r, b, 0,
                  0);
  if (sheet_chart != nullptr && sheet_chart->ranges_size() == 1) {
    add_row_provenance(*sheet_chart, sheet, table);
  }
  if (!folded) {
    arena_.warn("chart " + (name.empty() ? picture_ref : name)
                + " carried no data to bind");
  }

  // The chart title is the composite's caption.
  const std::string title = typed ? object->chart().title() : std::string();
  if (!title.empty()) {
    add_caption(title, picture, picture_ref, layer, page_local, page_index, l,
                t, r, b);
  }
  data_counters().charts_bound.fetch_add(1, std::memory_order_relaxed);
  data_log("chart " + (name.empty() ? picture_ref : name) + " bound to "
           + table_ref + " (" + std::to_string(table->data().num_rows()) + "x"
           + std::to_string(table->data().num_cols())
           + (folded ? "" : ", no data") + (title.empty() ? ")" : ", caption)"));
}

void ChartFold::flush(const ShapeFold& shapes) {
  std::map<int, std::deque<officev1::EmbeddedObject>> waiting;
  waiting.swap(pending_charts_);
  for (auto& [page_index, charts] : waiting) {
    std::string parent = "#/body";
    docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
    if (sheets_.has_sheet(page_index)) {
      parent = sheets_.group_ref(page_index);
      layer = sheets_.layer(page_index);
    } else if (shapes.has_slide(page_index)) {
      parent = shapes.slide_group_ref(page_index);
    }
    for (const officev1::EmbeddedObject& object : charts) {
      const double l = static_cast<double>(object.position().x());
      const double t = static_cast<double>(object.position().y());
      emit(&object, nullptr, parent, layer, true, object.page_index(), l, t,
           l + static_cast<double>(object.width_twips()),
           t + static_cast<double>(object.height_twips()));
    }
  }
}

}  // namespace grparse::office_fold
