// Hardens the office chart composite (a CHART picture, its bound data
// table, a caption from the title) against the shapes the chart fixtures
// exercise: several series on one chart, a pie with no title and no axis
// titles, numeric-looking categories, typed numeric cells with integer
// display text, header flags on the label row only, unlabelled series, a
// chart that carries no data, and byte-identical output across repeat runs.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Series {
  std::string label;
  std::vector<double> values;
};

struct ChartShape {
  officev1::EmbeddedChartKind kind = officev1::EMBEDDED_CHART_KIND_COLUMN;
  std::string title;
  std::string x_title;
  std::string y_title;
  std::vector<std::string> categories;
  std::vector<Series> series;
};

officev1::StreamPagesResponse info_event(const std::string& type, const std::string& name) {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id(name);
  info->set_source_format(type == "spreadsheet" ? "xlsx" : "pptx");
  info->set_page_count(1);
  info->set_document_type(type);
  officev1::PageRect* page = info->add_page_rects();
  page->set_width_twips(24000);
  page->set_height_twips(15000);
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

officev1::StreamPagesResponse sheet_event(int index, const std::string& name, int rows,
                                          int columns) {
  officev1::StreamPagesResponse event;
  officev1::Sheet* sheet = event.mutable_sheet();
  sheet->set_index(index);
  sheet->set_name(name);
  sheet->set_visible(true);
  sheet->set_tab_color_rgb(-1);
  sheet->set_used_end_row(rows - 1);
  sheet->set_used_end_column(columns - 1);
  return event;
}

officev1::StreamPagesResponse slide_event(int index, const std::string& name) {
  officev1::StreamPagesResponse event;
  event.mutable_slide()->set_index(index);
  event.mutable_slide()->set_name(name);
  return event;
}

void add_text_cell(officev1::SheetRow* row, int column, const std::string& text) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_TEXT);
  cell->set_display(text);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
}

void add_number_cell(officev1::SheetRow* row, int column, double value) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
  cell->set_number(value);
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%g", value);
  cell->set_display(buffer);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
}

// The sheet behind a chart: a header row (the category header plus one
// header per series) and one row per category.
void feed_sheet(grparse::DoclingMapper* mapper, int sheet_index, const std::string& name,
                const std::string& category_header, const ChartShape& shape) {
  const int columns = static_cast<int>(shape.series.size()) + 1;
  mapper->consume(sheet_event(sheet_index, name,
                              static_cast<int>(shape.categories.size()) + 1, columns));
  officev1::StreamPagesResponse header;
  header.mutable_sheet_row()->set_sheet_index(sheet_index);
  header.mutable_sheet_row()->set_row(0);
  add_text_cell(header.mutable_sheet_row(), 0, category_header);
  for (size_t column = 0; column < shape.series.size(); column++) {
    add_text_cell(header.mutable_sheet_row(), static_cast<int>(column) + 1,
                  shape.series[column].label);
  }
  mapper->consume(header);
  for (size_t row = 0; row < shape.categories.size(); row++) {
    officev1::StreamPagesResponse event;
    event.mutable_sheet_row()->set_sheet_index(sheet_index);
    event.mutable_sheet_row()->set_row(static_cast<int>(row) + 1);
    add_text_cell(event.mutable_sheet_row(), 0, shape.categories[row]);
    for (size_t column = 0; column < shape.series.size(); column++) {
      add_number_cell(event.mutable_sheet_row(), static_cast<int>(column) + 1,
                      shape.series[column].values[row]);
    }
    mapper->consume(event);
  }
}

officev1::StreamPagesResponse chart_object_event(int page_index, const std::string& name,
                                                 const ChartShape& shape, long long x = 4000,
                                                 long long y = 300) {
  officev1::StreamPagesResponse event;
  officev1::EmbeddedObject* object = event.mutable_embedded_object();
  object->set_index(0);
  object->set_kind(officev1::EMBEDDED_OBJECT_KIND_CHART);
  object->set_page_index(page_index);
  object->set_name(name);
  object->mutable_position()->set_x(x);
  object->mutable_position()->set_y(y);
  object->set_width_twips(8000);
  object->set_height_twips(4000);
  object->set_replacement_mime_type("image/png");
  object->set_replacement_image("\x89PNG-bytes");
  officev1::EmbeddedChart* chart = object->mutable_chart();
  chart->set_kind(shape.kind);
  chart->set_title(shape.title);
  chart->set_x_axis_title(shape.x_title);
  chart->set_y_axis_title(shape.y_title);
  for (const std::string& category : shape.categories) chart->add_categories(category);
  for (const Series& one : shape.series) {
    officev1::EmbeddedChartSeries* series = chart->add_series();
    series->set_label(one.label);
    for (double value : one.values) series->add_values_y(value);
  }
  chart->mutable_tabular()->set_rows(static_cast<int>(shape.categories.size()) + 1);
  chart->mutable_tabular()->set_columns(static_cast<int>(shape.series.size()) + 1);
  return event;
}

officev1::StreamPagesResponse sheet_chart_event(int sheet_index, const ChartShape& shape) {
  officev1::StreamPagesResponse event;
  officev1::SheetChart* chart = event.mutable_sheet_chart();
  chart->set_sheet_index(sheet_index);
  chart->set_name("Object 1");
  officev1::SheetRangeRef* range = chart->add_ranges();
  range->set_start_row(0);
  range->set_start_column(0);
  range->set_end_row(static_cast<int>(shape.categories.size()));
  range->set_end_column(static_cast<int>(shape.series.size()));
  chart->set_has_column_headers(true);
  chart->set_has_row_headers(true);
  return event;
}

const docv1::TableCell* cell_at(const docv1::TableData& data, int row, int column) {
  for (const docv1::TableCell& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == row && cell.start_col_offset_idx() == column) {
      return &cell;
    }
  }
  return nullptr;
}

ChartShape revenue_chart() {
  return ChartShape{
      .kind = officev1::EMBEDDED_CHART_KIND_COLUMN,
      .title = "Revenue by region",
      .x_title = "Region",
      .y_title = "kUSD",
      .categories = {"North", "South", "East", "West"},
      .series = {{"Q1", {120, 80, 143, 88}}, {"Q2", {135.5, 97, 70.25, 101}}},
  };
}

ChartShape share_pie() {
  return ChartShape{
      .kind = officev1::EMBEDDED_CHART_KIND_PIE,
      .title = "",
      .x_title = "",
      .y_title = "",
      .categories = {"Alpha", "Beta", "Gamma", "Delta"},
      .series = {{"Share", {45, 30, 15, 10}}},
  };
}

// One spreadsheet: the chart object first (as the collector emits draw-page
// objects), then the sheet, then the SheetChart that places it.
grparse::DoclingMapper spreadsheet_with(const ChartShape& shape,
                                        const std::string& category_header) {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event("spreadsheet", "book.xlsx"));
  mapper.consume(chart_object_event(0, "Chart 1", shape));
  feed_sheet(&mapper, 0, "Data", category_header, shape);
  mapper.consume(sheet_chart_event(0, shape));
  mapper.consume(status_event());
  return mapper;
}

void verify_multi_series_bar_gets_one_annotation_per_series() {
  grparse::DoclingMapper mapper = spreadsheet_with(revenue_chart(), "Region");
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1, "one chart, one picture");
  std::vector<const docv1::PictureBarChartData*> bars;
  for (const docv1::PictureAnnotation& annotation : document.pictures(0).annotations()) {
    if (annotation.has_bar_chart()) bars.push_back(&annotation.bar_chart());
  }
  require(bars.size() == 2, "a two-series column chart carries two bar annotations, in series order");
  require(bars[0]->bars(0).values() == 120 && bars[1]->bars_size() == 4 &&
              bars[1]->bars(2).label() == "East" && bars[1]->bars(2).values() == 70.25,
          "the second annotation is the second series, not the first series again");
  require(bars[0]->title() == "Revenue by region" && bars[1]->title() == "Revenue by region" &&
              bars[0]->x_axis_label() == "Region" && bars[1]->y_axis_label() == "kUSD",
          "title and axis titles are the chart's own on every annotation");
  const docv1::TableData& data = document.tables(1).data();
  require(data.num_rows() == 5 && data.num_cols() == 3,
          "the bound table has the label row plus one row per category");
  require(cell_at(data, 0, 2)->text() == "Q2" && cell_at(data, 4, 2)->text() == "101",
          "both series land as columns");
}

void verify_single_series_bar_keeps_the_axis_title() {
  ChartShape shape = revenue_chart();
  shape.series.pop_back();
  grparse::DoclingMapper mapper = spreadsheet_with(shape, "Region");
  int count = 0;
  std::string y_label;
  for (const docv1::PictureAnnotation& annotation : mapper.document().pictures(0).annotations()) {
    if (annotation.has_bar_chart()) {
      ++count;
      y_label = annotation.bar_chart().y_axis_label();
    }
  }
  require(count == 1 && y_label == "kUSD",
          "a single series is one annotation with the chart's value axis title");
}

void verify_pie_without_title_names_corner_from_sheet_and_mints_no_caption() {
  grparse::DoclingMapper mapper = spreadsheet_with(share_pie(), "Segment");
  const docv1::Document& document = mapper.document();
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.captions_size() == 0, "no title, no caption invented");
  require(document.texts_size() == 0, "no caption text item either");
  bool pie = false;
  bool tabular_title_empty = false;
  for (const docv1::PictureAnnotation& annotation : picture.annotations()) {
    if (annotation.has_pie_chart()) pie = annotation.pie_chart().slices_size() == 4;
    if (annotation.has_tabular_chart()) tabular_title_empty = annotation.tabular_chart().title().empty();
  }
  require(pie && tabular_title_empty, "the pie annotation rides along with an untitled projection");
  const docv1::TableData& data = document.tables(1).data();
  const docv1::TableCell* corner = cell_at(data, 0, 0);
  require(corner != nullptr && corner->text() == "Segment" && corner->column_header(),
          "the blank corner takes the sheet's own header over the category column");
  require(data.columns_size() == 2 && data.columns(0).name() == "Segment",
          "the column schema names the category column the same way");
  require(data.grid_size() == 5 && data.grid(0).cells(0).text() == "Segment",
          "the materialized grid mirrors the corner");
  require(cell_at(data, 0, 1)->text() == "Share" && cell_at(data, 2, 1)->value().number() == 30,
          "the series label heads the value column and the slices stay numeric");
  require(mapper.warnings().empty(), "an untitled chart is not a warning");
  require(grparse::docling_integrity_errors(document).empty(), "the pie composite is well formed");
}

void verify_presentation_pie_without_sheet_leaves_the_corner_blank() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event("presentation", "deck.pptx"));
  mapper.consume(chart_object_event(0, "Chart 2", share_pie()));
  mapper.consume(slide_event(0, "Share"));
  mapper.consume(status_event());
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1 && document.tables_size() == 1,
          "the flushed slide chart binds its table");
  const docv1::TableCell* corner = cell_at(document.tables(0).data(), 0, 0);
  require(corner != nullptr && corner->text().empty() && corner->column_header(),
          "with no axis title and no sheet, the corner stays blank rather than guessed");
  require(document.pictures(0).captions_size() == 0, "still no caption without a title");
}

void verify_numeric_categories_and_typed_cells() {
  ChartShape shape{
      .kind = officev1::EMBEDDED_CHART_KIND_LINE,
      .title = "Yearly count",
      .x_title = "Year",
      .y_title = "Count",
      .categories = {"2019", "2020", "2021"},
      .series = {{"Count", {3, 4.5, 1000000}}},
  };
  grparse::DoclingMapper mapper = spreadsheet_with(shape, "Year");
  const docv1::TableData& data = mapper.document().tables(1).data();
  const docv1::TableCell* year = cell_at(data, 2, 0);
  require(year != nullptr && year->text() == "2020" && year->row_header() && !year->has_value(),
          "a numeric-looking category stays the category text, flagged as a row header");
  const docv1::TableCell* three = cell_at(data, 1, 1);
  require(three != nullptr && three->text() == "3" && three->value().number() == 3,
          "an integral value displays without a decimal point and keeps its number");
  const docv1::TableCell* half = cell_at(data, 2, 1);
  require(half != nullptr && half->text() == "4.5" && half->value().number() == 4.5,
          "a decimal value displays as written");
  const docv1::TableCell* million = cell_at(data, 3, 1);
  require(million != nullptr && million->text() == "1e+06" && million->value().number() == 1000000,
          "large values use the same %g display as sheet cells; the typed value is exact");
  for (const docv1::TableCell& cell : data.table_cells()) {
    require(cell.column_header() == (cell.start_row_offset_idx() == 0),
            "column_header marks the label row and nothing else");
    require(cell.row_header() == (cell.start_col_offset_idx() == 0 && cell.start_row_offset_idx() > 0),
            "row_header marks the category column below the label row and nothing else");
  }
  require(data.columns(0).declared_type() == "text" && data.columns(1).declared_type() == "number",
          "the schema types the category column as text and the series as number");
  bool line = false;
  for (const docv1::PictureAnnotation& annotation : mapper.document().pictures(0).annotations()) {
    if (annotation.has_line_chart()) {
      line = annotation.line_chart().lines_size() == 1 &&
             annotation.line_chart().lines(0).values_size() == 3 &&
             annotation.line_chart().lines(0).values(1).second() == 4.5;
    }
  }
  require(line, "the line annotation carries one line with every point");
}

void verify_unlabelled_series_get_positional_labels() {
  ChartShape shape = revenue_chart();
  shape.series[0].label.clear();
  shape.series[1].label.clear();
  grparse::DoclingMapper mapper;
  mapper.consume(info_event("presentation", "deck.pptx"));
  mapper.consume(chart_object_event(0, "Chart 1", shape));
  mapper.consume(slide_event(0, "Revenue"));
  mapper.consume(status_event());
  const docv1::TableData& data = mapper.document().tables(0).data();
  require(cell_at(data, 0, 1)->text() == "Series 1" && cell_at(data, 0, 2)->text() == "Series 2",
          "series without labels are numbered in order");
  require(data.columns(1).name() == "Series 1" && data.columns(2).name() == "Series 2",
          "the schema uses the same positional names");
}

void verify_chart_without_data_binds_an_empty_table_and_warns() {
  ChartShape shape{.kind = officev1::EMBEDDED_CHART_KIND_OTHER,
                   .title = "Empty",
                   .x_title = "",
                   .y_title = "",
                   .categories = {},
                   .series = {}};
  grparse::DoclingMapper mapper;
  mapper.consume(info_event("presentation", "deck.pptx"));
  officev1::StreamPagesResponse event = chart_object_event(0, "Chart 9", shape);
  event.mutable_embedded_object()->mutable_chart()->clear_tabular();
  mapper.consume(event);
  mapper.consume(slide_event(0, "Empty"));
  mapper.consume(status_event());
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1 && document.tables_size() == 1 &&
              document.tables(0).data().table_cells().empty(),
          "a chart with no series and no projection still binds an empty table");
  require(document.pictures(0).captions_size() == 1, "the title still captions it");
  require(mapper.warnings().size() == 1 && mapper.warnings()[0].contains("carried no data"),
          "the missing data is a warning, never invented cells");
}

// Two mappers fed the same stream must serialize the same bytes: the
// composite's order (picture, table, caption), its refs and its pending
// chart bookkeeping hold no clock, address or hash-order dependence.
void verify_repeat_runs_are_byte_identical() {
  const auto run = [] {
    grparse::DoclingMapper mapper;
    mapper.consume(info_event("spreadsheet", "book.xlsx"));
    mapper.consume(chart_object_event(0, "Chart 1", revenue_chart(), 4000, 300));
    mapper.consume(chart_object_event(1, "Chart 2", share_pie(), 2000, 500));
    feed_sheet(&mapper, 0, "Revenue", "Region", revenue_chart());
    mapper.consume(sheet_chart_event(0, revenue_chart()));
    feed_sheet(&mapper, 1, "Share", "Segment", share_pie());
    // The pie's SheetChart never arrives: the stream end places it.
    mapper.consume(status_event());
    return mapper.document().SerializeAsString();
  };
  const std::string first = run();
  const std::string second = run();
  require(!first.empty() && first == second, "repeat runs serialize identically");
  docv1::Document document;
  require(document.ParseFromString(first), "the serialized document parses back");
  require(document.pictures_size() == 2 && document.tables_size() == 4,
          "two charts, two sheet tables and two bound tables");
  require(document.pictures(1).parent().ref() == document.groups(1).self_ref(),
          "the flushed pie sits under its own sheet");
  require(cell_at(document.tables(3).data(), 0, 0)->text().empty(),
          "a flushed chart has no SheetChart to name its corner from, so it stays blank");
}

}  // namespace

int main() {
  try {
    verify_multi_series_bar_gets_one_annotation_per_series();
    verify_single_series_bar_keeps_the_axis_title();
    verify_pie_without_title_names_corner_from_sheet_and_mints_no_caption();
    verify_presentation_pie_without_sheet_leaves_the_corner_blank();
    verify_numeric_categories_and_typed_cells();
    verify_unlabelled_series_get_positional_labels();
    verify_chart_without_data_binds_an_empty_table_and_warns();
    verify_repeat_runs_are_byte_identical();
    std::println("chart-composite-test: all checks passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "chart-composite-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
