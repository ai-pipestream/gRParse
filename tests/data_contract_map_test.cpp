// Anti-drift: the data contract of a mapped office document.
//
// The office fold is where a chart, a sheet and a text document with inline
// drawings become typed Document nodes. The shape it produces is a contract
// downstream readers rely on, and it is not enough that each piece exists:
// the composite has to be exactly one of each thing. This pins that
// exclusivity (one chart picture, one bound table under it, one caption from
// the title), the header and span marking a sheet keeps, the rule that typed
// facts never degrade into colon-keyed strings on custom_fields, and that two
// identical event streams fold to identical bytes.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/data_totals.h"
#include "grparse/docling_map.h"
#include "grparse/document_render.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

// The office-side fold under test and its structural integrity walk, aliased
// once each so the rest of the file reads as what it is.
using OfficeMapper = grparse::DoclingMapper;
const auto integrity_errors = [](const docv1::Document& document) {
  return grparse::docling_integrity_errors(document);
};

using grparse_test::require;

// ---- event builders -------------------------------------------------------

officev1::StreamPagesResponse spreadsheet_info_event() {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("quarter.xlsx");
  info->set_source_format("xlsx");
  info->set_page_count(1);
  info->set_document_type("spreadsheet");
  officev1::PageRect* page = info->add_page_rects();
  page->set_width_twips(24000);
  page->set_height_twips(15000);
  return event;
}

officev1::StreamPagesResponse writer_info_event() {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("report.docx");
  info->set_source_format("docx");
  info->set_page_count(1);
  info->set_document_type("text");
  officev1::PageRect* page = info->add_page_rects();
  page->set_width_twips(11906);
  page->set_height_twips(16838);
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

officev1::StreamPagesResponse sheet_event(int index, const std::string& name, int end_row,
                                          int end_column) {
  officev1::StreamPagesResponse event;
  officev1::Sheet* sheet = event.mutable_sheet();
  sheet->set_index(index);
  sheet->set_name(name);
  sheet->set_visible(true);
  sheet->set_tab_color_rgb(-1);
  sheet->set_used_end_row(end_row);
  sheet->set_used_end_column(end_column);
  return event;
}

officev1::StreamPagesResponse row_event(int sheet_index, int row) {
  officev1::StreamPagesResponse event;
  event.mutable_sheet_row()->set_sheet_index(sheet_index);
  event.mutable_sheet_row()->set_row(row);
  return event;
}

officev1::SheetCell* text_cell(officev1::SheetRow* row, int column, const std::string& text) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_TEXT);
  cell->set_display(text);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
  return cell;
}

officev1::SheetCell* number_cell(officev1::SheetRow* row, int column, double value,
                                 const std::string& display) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
  cell->set_number(value);
  cell->set_display(display);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
  return cell;
}

// A two-series, three-category column chart with a title and axis titles.
officev1::StreamPagesResponse chart_object_event(int page_index) {
  officev1::StreamPagesResponse event;
  officev1::EmbeddedObject* object = event.mutable_embedded_object();
  object->set_index(0);
  object->set_kind(officev1::EMBEDDED_OBJECT_KIND_CHART);
  object->set_page_index(page_index);
  object->set_name("Chart 1");
  object->mutable_position()->set_x(4779);
  object->mutable_position()->set_y(299);
  object->set_width_twips(8000);
  object->set_height_twips(4000);
  object->set_replacement_mime_type("image/png");
  object->set_replacement_image("\x89PNG-bytes");
  officev1::EmbeddedChart* chart = object->mutable_chart();
  chart->set_kind(officev1::EMBEDDED_CHART_KIND_COLUMN);
  chart->set_chart_type_service("com.sun.star.chart2.ColumnChartType");
  chart->set_title("Revenue by region");
  chart->set_x_axis_title("Region");
  chart->set_y_axis_title("kUSD");
  for (const char* category : {"North", "South", "West"}) chart->add_categories(category);
  officev1::EmbeddedChartSeries* q1 = chart->add_series();
  q1->set_label("Q1");
  for (double value : {120.0, 80.0, 64.0}) q1->add_values_y(value);
  officev1::EmbeddedChartSeries* q2 = chart->add_series();
  q2->set_label("Q2");
  for (double value : {135.5, 97.0, 70.25}) q2->add_values_y(value);
  return event;
}

officev1::StreamPagesResponse sheet_chart_event(int sheet_index) {
  officev1::StreamPagesResponse event;
  officev1::SheetChart* chart = event.mutable_sheet_chart();
  chart->set_sheet_index(sheet_index);
  chart->set_name("Object 1");
  officev1::SheetRangeRef* range = chart->add_ranges();
  range->set_start_row(0);
  range->set_start_column(0);
  range->set_end_row(3);
  range->set_end_column(2);
  chart->set_has_column_headers(true);
  chart->set_has_row_headers(true);
  return event;
}

// ---- streams --------------------------------------------------------------

// A sheet whose data a chart plots: a header row and three data rows, with
// the chart object arriving before the sheet as the collector emits it.
std::vector<officev1::StreamPagesResponse> chart_workbook_stream() {
  std::vector<officev1::StreamPagesResponse> events;
  events.push_back(spreadsheet_info_event());
  events.push_back(chart_object_event(0));
  events.push_back(sheet_event(0, "Sales", 3, 2));
  officev1::StreamPagesResponse header = row_event(0, 0);
  text_cell(header.mutable_sheet_row(), 0, "Region");
  text_cell(header.mutable_sheet_row(), 1, "Q1");
  text_cell(header.mutable_sheet_row(), 2, "Q2");
  events.push_back(header);
  struct Row {
    const char* region;
    double q1;
    const char* q1_display;
    double q2;
    const char* q2_display;
  };
  const Row rows[] = {{"North", 120.0, "120", 135.5, "135.5"},
                      {"South", 80.0, "80", 97.0, "97"},
                      {"West", 64.0, "64", 70.25, "70.25"}};
  int row_number = 1;
  for (const Row& row : rows) {
    officev1::StreamPagesResponse event = row_event(0, row_number++);
    text_cell(event.mutable_sheet_row(), 0, row.region);
    number_cell(event.mutable_sheet_row(), 1, row.q1, row.q1_display);
    number_cell(event.mutable_sheet_row(), 2, row.q2, row.q2_display);
    events.push_back(event);
  }
  events.push_back(sheet_chart_event(0));
  events.push_back(status_event());
  return events;
}

// A sheet whose header row sits under a merged title and above rows carrying
// a merged label cell: the marking and the spans have to survive together.
std::vector<officev1::StreamPagesResponse> header_sheet_stream() {
  std::vector<officev1::StreamPagesResponse> events;
  events.push_back(spreadsheet_info_event());
  events.push_back(sheet_event(0, "Report", 3, 2));
  officev1::StreamPagesResponse title = row_event(0, 0);
  text_cell(title.mutable_sheet_row(), 0, "Quarterly totals")->set_merged_columns(3);
  events.push_back(title);
  officev1::StreamPagesResponse header = row_event(0, 1);
  text_cell(header.mutable_sheet_row(), 0, "Item");
  text_cell(header.mutable_sheet_row(), 1, "Units");
  text_cell(header.mutable_sheet_row(), 2, "Price");
  events.push_back(header);
  officev1::StreamPagesResponse widget = row_event(0, 2);
  text_cell(widget.mutable_sheet_row(), 0, "Widget");
  number_cell(widget.mutable_sheet_row(), 1, 10, "10");
  number_cell(widget.mutable_sheet_row(), 2, 2.5, "2.5");
  events.push_back(widget);
  officev1::StreamPagesResponse bundle = row_event(0, 3);
  text_cell(bundle.mutable_sheet_row(), 0, "Bundle")->set_merged_rows(2);
  number_cell(bundle.mutable_sheet_row(), 1, 1, "1");
  events.push_back(bundle);
  events.push_back(status_event());
  return events;
}

// A text document with two inline drawings, each anchored in the empty
// paragraph that holds it, plus prose around them. The drawings arrive after
// every paragraph, as the collector emits them.
std::vector<officev1::StreamPagesResponse> inline_drawings_stream() {
  const auto paragraph = [](const std::string& text, long long caret_y) {
    officev1::StreamPagesResponse event;
    officev1::Paragraph* item = event.mutable_paragraph();
    item->set_page_index(0);
    item->set_char_offset(0);
    item->set_outline_level(0);
    item->set_list_level(-1);
    item->mutable_start()->set_y(caret_y);
    item->mutable_end()->set_y(caret_y);
    if (!text.empty()) {
      officev1::TextRun* run = item->add_runs();
      run->set_text(text);
      run->set_char_offset(0);
      run->set_char_length(static_cast<long long>(text.size()));
    }
    return event;
  };
  const auto drawing = [](int index, long long y, long long height) {
    officev1::StreamPagesResponse event;
    officev1::EmbeddedImage* image = event.mutable_embedded_image();
    image->set_index(index);
    image->set_page_index(0);
    image->set_name("Picture " + std::to_string(index + 1));
    image->set_mime_type("image/png");
    image->set_data("png");
    image->set_width_twips(4000);
    image->set_height_twips(height);
    image->mutable_anchor()->set_x(1000);
    image->mutable_anchor()->set_y(y);
    return event;
  };

  std::vector<officev1::StreamPagesResponse> events;
  events.push_back(writer_info_event());
  events.push_back(paragraph("The survey opened at dawn.", 2000));
  events.push_back(paragraph("", 4000));
  events.push_back(paragraph("The second traverse followed the ridge.", 6000));
  events.push_back(paragraph("", 9000));
  events.push_back(drawing(0, 3000, 1500));
  events.push_back(drawing(1, 8000, 1500));
  events.push_back(status_event());
  return events;
}

docv1::Document fold(const std::vector<officev1::StreamPagesResponse>& events) {
  OfficeMapper mapper;
  for (const officev1::StreamPagesResponse& event : events) mapper.consume(event);
  return mapper.take();
}

// ---- shared assertions ----------------------------------------------------

// Every custom_fields key the document carries, with the arena it sits on,
// so a failure names where a keyed string appeared.
std::vector<std::string> custom_field_keys(const docv1::Document& document) {
  std::vector<std::string> keys;
  const auto collect = [&keys](const std::string& where, const auto& fields) {
    for (const auto& [key, value] : fields) keys.push_back(where + ": " + key);
  };
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.has_text()) collect("texts", item.text().base().meta().custom_fields());
    if (item.has_title()) collect("texts", item.title().base().meta().custom_fields());
    if (item.has_section_header()) {
      collect("texts", item.section_header().base().meta().custom_fields());
    }
    if (item.has_list_item()) collect("texts", item.list_item().base().meta().custom_fields());
  }
  for (const docv1::TableItem& item : document.tables()) collect("tables", item.meta().custom_fields());
  for (const docv1::PictureItem& item : document.pictures()) {
    collect("pictures", item.meta().custom_fields());
  }
  for (const docv1::GroupItem& item : document.groups()) collect("groups", item.meta().custom_fields());
  return keys;
}

// Typed facts belong in the typed nodes and in Document.claims; a key with a
// namespace punched into it with a colon is the shape they degrade into, and
// it is not allowed to appear on its own.
void require_no_colon_keys(const docv1::Document& document, const std::string& what) {
  for (const std::string& key : custom_field_keys(document)) {
    require(!key.substr(key.find(": ") + 2).contains(':'),
            what + " minted a colon-keyed custom field: " + key);
  }
}

const docv1::TableCell* cell_at(const docv1::TableData& data, int row, int column) {
  for (const docv1::TableCell& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == row && cell.start_col_offset_idx() == column) return &cell;
  }
  return nullptr;
}

// ---- the contract ---------------------------------------------------------

// One chart is one picture with one table and one caption: no empty twin
// picture, no second projection of the same data, no caption per series.
void verify_the_chart_composite_is_exactly_one_of_each() {
  const grparse::DataTotals before = grparse::data_totals();
  const docv1::Document document = fold(chart_workbook_stream());
  const grparse::DataTotals after = grparse::data_totals();

  require(document.pictures_size() == 1, "one chart makes one picture");
  const docv1::PictureItem& chart = document.pictures(0);
  require(chart.label() == docv1::DOC_ITEM_LABEL_CHART, "the picture is a CHART item");
  require(chart.children_size() == 2,
          "the chart owns exactly two children, its data table and its caption");
  require(chart.children(0).ref() == "#/tables/1", "the first child is the bound table");
  require(chart.captions_size() == 1, "the chart title makes exactly one caption");
  require(chart.children(1).ref() == chart.captions(0).ref(),
          "the second child is that same caption, listed once");

  const docv1::TableItem& bound = document.tables(1);
  require(bound.parent().ref() == chart.self_ref(), "the bound table parents under the chart");
  const docv1::TableData& data = bound.data();
  require(data.num_rows() == 4 && data.num_cols() == 3,
          "one label row over three categories, one column per series");
  require(cell_at(data, 0, 0) != nullptr && cell_at(data, 0, 0)->column_header() &&
              cell_at(data, 0, 1)->column_header() && cell_at(data, 0, 2)->column_header(),
          "the whole label row is column headers");
  require(cell_at(data, 1, 0)->row_header() && cell_at(data, 2, 0)->row_header() &&
              cell_at(data, 3, 0)->row_header(),
          "the categories are row headers");
  require(!cell_at(data, 1, 1)->column_header() && !cell_at(data, 1, 1)->row_header(),
          "a value cell is neither kind of header");
  require(cell_at(data, 1, 1)->value().number() == 120.0 &&
              cell_at(data, 3, 2)->value().number() == 70.25,
          "series values stay typed numbers, not only display text");
  require(data.columns_size() == 3 && data.columns(1).declared_type() == "number" &&
              data.columns(2).declared_type() == "number",
          "the series columns declare a numeric type");

  // The caption is the chart's title and nothing else claims it.
  int captions = 0;
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.has_text() && item.text().base().label() == docv1::DOC_ITEM_LABEL_CAPTION) {
      captions++;
      require(item.text().base().text() == "Revenue by region",
              "the caption text is the chart title");
      require(item.text().base().parent().ref() == chart.self_ref(),
              "the caption parents under the chart");
    }
  }
  require(captions == 1, "one caption in the whole document");

  require(after.charts_bound - before.charts_bound == 1, "one chart binding is counted");
  require(after.chart_captions - before.chart_captions == 1, "one minted caption is counted");
  require_no_colon_keys(document, "the chart fold");
  require(integrity_errors(document).empty(), "the chart composite is well formed");
}

// The sheet's header row is marked, the merged title above it is a section
// row rather than a header, and every merge span survives the marking.
void verify_sheet_header_marking_keeps_the_spans() {
  const grparse::DataTotals before = grparse::data_totals();
  const docv1::Document document = fold(header_sheet_stream());
  const grparse::DataTotals after = grparse::data_totals();

  require(document.tables_size() == 1, "one sheet, one table");
  const docv1::TableData& data = document.tables(0).data();
  const docv1::TableCell* title = cell_at(data, 0, 0);
  require(title != nullptr && title->row_section() && !title->column_header(),
          "a merged title spanning the width is a section row, not the header");
  require(title->col_span() == 3 && title->end_col_offset_idx() == 3,
          "the title keeps the column span it arrived with");
  require(cell_at(data, 1, 0)->column_header() && cell_at(data, 1, 1)->column_header() &&
              cell_at(data, 1, 2)->column_header(),
          "the label row above the quantities is the header row");
  require(!cell_at(data, 2, 0)->column_header(), "a data row is not a header");
  const docv1::TableCell* merged = cell_at(data, 3, 0);
  require(merged != nullptr && merged->row_span() == 2 && merged->end_row_offset_idx() == 5,
          "a row-merged label keeps its span through the marking");
  require(cell_at(data, 2, 2)->value().number() == 2.5, "sheet quantities stay typed");
  require(after.sheet_header_rows - before.sheet_header_rows == 1,
          "one marked header row is counted");
  require_no_colon_keys(document, "the sheet fold");
  require(integrity_errors(document).empty(), "the sheet stays well formed");
}

// Inline drawings take the place of the empty paragraph that anchored them,
// so the reading order runs prose, drawing, prose, drawing.
void verify_inline_drawings_keep_their_place_and_their_types() {
  const docv1::Document document = fold(inline_drawings_stream());
  require(document.pictures_size() == 2, "two drawings, two pictures");
  std::vector<std::string> order;
  for (const docv1::RefItem& child : document.body().children()) {
    if (child.ref().starts_with("#/pictures/")) {
      order.push_back("picture");
    } else if (child.ref().starts_with("#/texts/")) {
      const int index = std::stoi(child.ref().substr(std::string("#/texts/").size()));
      const docv1::BaseTextItem& item = document.texts(index);
      order.push_back(item.has_text() ? item.text().base().text() : "?");
    }
  }
  const std::vector<std::string> expected = {"The survey opened at dawn.", "picture",
                                             "The second traverse followed the ridge.", "picture"};
  require(order == expected, "the drawings sit where their anchor paragraphs were");
  for (const docv1::PictureItem& picture : document.pictures()) {
    require(picture.has_image() && picture.image().mimetype() == "image/png",
            "each drawing carries its own bytes");
    require(picture.prov_size() == 1 && picture.prov(0).page_no() == 1,
            "each drawing carries page-local provenance");
  }
  require_no_colon_keys(document, "the text document fold");
  require(integrity_errors(document).empty(),
          "the inline drawings stay well formed");
}

// The one escape that is allowed to carry a colon, pinned so it stays the
// only one: a cell whose office name anchors at no grid position has nowhere
// typed to go, and its text is kept rather than dropped.
void verify_the_only_colon_key_is_the_unplaceable_cell_escape() {
  OfficeMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::TableData* table = event.mutable_table();
  table->set_rows(1);
  table->set_columns(1);
  officev1::TableCellData* placed = table->add_cells();
  placed->set_row(0);
  placed->set_column(0);
  placed->set_text("in the grid");
  officev1::TableCellData* stray = table->add_cells();
  stray->set_row(-1);
  stray->set_column(-1);
  stray->set_name("?");
  stray->set_text("anchors nowhere");
  mapper.consume(event);
  const docv1::Document document = mapper.take();

  std::vector<std::string> colon_keys;
  for (const std::string& key : custom_field_keys(document)) {
    const std::string bare = key.substr(key.find(": ") + 2);
    if (bare.contains(':')) colon_keys.push_back(bare);
  }
  require(colon_keys == std::vector<std::string>{"cell:?"},
          "the unplaceable cell escape is the only colon-keyed custom field the fold mints");
  require(document.tables(0).meta().custom_fields().at("cell:?").string_value() ==
              "anchors nowhere",
          "the unplaceable cell still keeps its text");
}

// Two folds of one event sequence render to the same canonical bytes: no
// counter, map iteration order or arena address leaks into the output.
void verify_identical_streams_fold_to_identical_bytes() {
  const std::vector<std::vector<officev1::StreamPagesResponse>> streams = {
      chart_workbook_stream(), header_sheet_stream(), inline_drawings_stream()};
  for (std::size_t index = 0; index < streams.size(); ++index) {
    const std::string first = grparse::render_canonical_json(fold(streams[index]));
    const std::string second = grparse::render_canonical_json(fold(streams[index]));
    require(!first.empty(), "the fold rendered nothing");
    require(first == second,
            "stream " + std::to_string(index) + " folded to different canonical bytes");
  }
  // The same content built from scratch rather than replayed: the events are
  // data, not identity.
  require(grparse::render_canonical_json(fold(chart_workbook_stream())) ==
              grparse::render_canonical_json(fold(chart_workbook_stream())),
          "the chart fold depends on the identity of its events");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("data-contract-map-test", "ok", {
      verify_the_chart_composite_is_exactly_one_of_each,
      verify_sheet_header_marking_keeps_the_spans,
      verify_inline_drawings_keep_their_place_and_their_types,
      verify_the_only_colon_key_is_the_unplaceable_cell_escape,
      verify_identical_streams_fold_to_identical_bytes,
  });
}
