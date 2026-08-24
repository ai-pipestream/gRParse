#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

officev1::TextRun make_run(const std::string& text, long long offset = 0) {
  officev1::TextRun run;
  run.set_text(text);
  run.set_char_offset(offset);
  run.set_char_length(static_cast<long long>(text.size()));
  return run;
}

officev1::StreamPagesResponse paragraph_event(const std::string& text,
                                              const std::string& style = "",
                                              int outline_level = 0,
                                              int list_level = -1) {
  officev1::StreamPagesResponse event;
  officev1::Paragraph* paragraph = event.mutable_paragraph();
  paragraph->set_style(style);
  paragraph->set_outline_level(outline_level);
  paragraph->set_list_level(list_level);
  paragraph->set_char_offset(0);
  *paragraph->add_runs() = make_run(text);
  return event;
}

// A two-page DocumentInfo whose second page starts at y=20000, so
// document-absolute geometry on page two must have that origin subtracted.
officev1::StreamPagesResponse document_info_event() {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("report.docx");
  info->set_source_format("docx");
  info->set_page_count(2);
  info->set_document_type("text");
  officev1::PageRect* first = info->add_page_rects();
  first->set_x_twips(0);
  first->set_y_twips(0);
  first->set_width_twips(12240);
  first->set_height_twips(15840);
  officev1::PageRect* second = info->add_page_rects();
  second->set_x_twips(0);
  second->set_y_twips(20000);
  second->set_width_twips(12240);
  second->set_height_twips(15840);
  return event;
}

void verify_fresh_mapper_builds_valid_skeleton() {
  grparse::DoclingMapper mapper;
  const docv1::Document& document = mapper.document();
  require(document.schema_name() == "docling_document_v2",
          "fresh document carries the docling v2 schema name");
  require(document.version() == "1.10.0",
          "fresh document carries the mirrored schema version");
  require(document.body().self_ref() == "#/body" &&
              document.furniture().self_ref() == "#/furniture",
          "fresh document has body and furniture roots");
  require(!mapper.finished(), "mapper is not finished before RenderStatus");
  require(grparse::docling_integrity_errors(document).empty(),
          "empty document is well formed");
}

void verify_document_info_maps_identity_and_pages() {
  grparse::DoclingMapper mapper;
  mapper.consume(document_info_event());
  const docv1::Document& document = mapper.document();
  require(document.name() == "report.docx",
          "document name falls back to the document id");
  require(document.origin().filename() == "report.docx",
          "origin filename echoes the document id");
  require(document.origin().mimetype() ==
              "application/vnd.openxmlformats-officedocument"
              ".wordprocessingml.document",
          "docx extension resolves to its mimetype");
  require(document.pages().size() == 2,
          "one PageItem per page rectangle");
  require(document.pages().at(1).page_no() == 1 &&
              document.pages().at(1).size().width() == 12240.0 &&
              document.pages().at(1).size().height() == 15840.0,
          "page one keeps its twips size under one-based numbering");
  require(document.pages().at(2).page_no() == 2,
          "page two is numbered from its zero-based index plus one");
}

void verify_unknown_extension_falls_back_to_octet_stream() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  event.mutable_document_info()->set_source_format("weird");
  mapper.consume(event);
  require(mapper.document().origin().mimetype() == "application/octet-stream",
          "unknown source format maps to application/octet-stream");
}

void verify_status_finishes_and_keeps_warnings() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::RenderStatus* status = event.mutable_status();
  status->set_state(officev1::RenderStatus::STATE_OK);
  status->add_warnings("substituted font");
  mapper.consume(event);
  require(mapper.finished(), "RenderStatus finishes the stream");
  require(mapper.warnings().size() == 1 &&
              mapper.warnings()[0] == "substituted font",
          "terminal warnings are kept verbatim");
}

void verify_unset_event_is_ignored() {
  grparse::DoclingMapper mapper;
  mapper.consume(officev1::StreamPagesResponse());
  require(!mapper.finished() && mapper.document().texts_size() == 0,
          "an EVENT_NOT_SET response changes nothing");
}

void verify_paragraph_classification() {
  grparse::DoclingMapper mapper;
  mapper.consume(paragraph_event("The Title", "Title"));
  mapper.consume(paragraph_event("Heading", "Heading 2", 2));
  mapper.consume(paragraph_event("bullet", "", 0, 0));
  mapper.consume(paragraph_event("plain body", ""));
  const docv1::Document& document = mapper.document();
  require(document.texts_size() == 4, "four paragraphs map to four texts");
  require(document.texts(0).has_title() &&
              document.texts(0).title().base().label() ==
                  docv1::DOC_ITEM_LABEL_TITLE,
          "Title style maps to the title variant");
  require(document.texts(1).has_section_header() &&
              document.texts(1).section_header().level() == 2,
          "outline level maps to a section header carrying its level");
  require(document.texts(2).has_list_item(),
          "list level zero and up maps to a list item");
  require(document.texts(3).has_text() &&
              document.texts(3).text().base().text() == "plain body" &&
              document.texts(3).text().base().orig() == "plain body",
          "plain paragraph keeps text and orig");
  require(document.body().children_size() == 4 &&
              document.body().children(0).ref() == "#/texts/0",
          "every paragraph links under the body in order");
  const auto& source = document.texts(3).text().base().source();
  require(source.size() == 1 && source[0].has_collector() &&
              source[0].collector().collector() == "libreoffice" &&
              source[0].collector().model() == "lok",
          "every mapped item is stamped with the libreoffice collector");
  require(grparse::docling_integrity_errors(document).empty(),
          "paragraph mapping stays well formed");
}

void verify_line_prov_is_page_local_with_charspans() {
  grparse::DoclingMapper mapper;
  mapper.consume(document_info_event());
  officev1::StreamPagesResponse event;
  officev1::Paragraph* paragraph = event.mutable_paragraph();
  paragraph->set_list_level(-1);  // wire contract: -1 means not a list item
  paragraph->set_char_offset(100);
  *paragraph->add_runs() = make_run("hello world", 100);
  // A measured line on page two: absolute y minus the 20000 page origin.
  officev1::LineBox* measured = paragraph->add_line_rects();
  measured->set_page_index(1);
  measured->set_x_twips(1000);
  measured->set_y_twips(21000);
  measured->set_width_twips(500);
  measured->set_height_twips(200);
  measured->set_char_start(0);
  measured->set_char_end(5);
  // An unmeasured line keeps the full item span.
  officev1::LineBox* unmeasured = paragraph->add_line_rects();
  unmeasured->set_page_index(1);
  unmeasured->set_x_twips(1000);
  unmeasured->set_y_twips(21200);
  unmeasured->set_width_twips(500);
  unmeasured->set_height_twips(200);
  unmeasured->set_char_start(-1);
  unmeasured->set_char_end(-1);
  mapper.consume(event);
  const auto& base = mapper.document().texts(0).text().base();
  require(base.prov_size() == 2, "one provenance item per line box");
  const docv1::ProvenanceItem& first = base.prov(0);
  require(first.page_no() == 2, "line provenance carries one-based page");
  require(first.bbox().l() == 1000.0 && first.bbox().t() == 1000.0 &&
              first.bbox().r() == 1500.0 && first.bbox().b() == 1200.0,
          "document-absolute line boxes become page-local");
  require(first.bbox().coord_origin() == docv1::COORD_ORIGIN_TOPLEFT,
          "provenance boxes are top-left origin");
  require(first.charspan().start() == 100 && first.charspan().end() == 105,
          "measured lines narrow their charspan offset from the item span");
  require(base.prov(1).charspan().start() == 100 &&
              base.prov(1).charspan().end() == 111,
          "unmeasured lines keep the full item span");
}

void verify_caret_prov_fallback_and_unresolved_page() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::Paragraph* paragraph = event.mutable_paragraph();
  paragraph->set_list_level(-1);  // wire contract: -1 means not a list item
  paragraph->set_page_index(0);
  paragraph->mutable_start()->set_x(900);
  paragraph->mutable_start()->set_y(400);
  paragraph->mutable_end()->set_x(300);
  paragraph->mutable_end()->set_y(700);
  *paragraph->add_runs() = make_run("anchored");
  mapper.consume(event);
  const auto& base = mapper.document().texts(0).text().base();
  require(base.prov_size() == 1, "no line rects falls back to caret anchors");
  require(base.prov(0).bbox().l() == 300.0 && base.prov(0).bbox().t() == 400.0 &&
              base.prov(0).bbox().r() == 900.0 && base.prov(0).bbox().b() == 700.0,
          "caret provenance normalizes min and max corners");

  officev1::StreamPagesResponse unresolved;
  officev1::Paragraph* off_page = unresolved.mutable_paragraph();
  off_page->set_list_level(-1);  // wire contract: -1 means not a list item
  off_page->set_page_index(-1);
  *off_page->add_runs() = make_run("nowhere");
  mapper.consume(unresolved);
  require(mapper.document().texts(1).text().base().prov_size() == 0,
          "an unresolved page appends no provenance");
}

void verify_uniform_formatting_and_hyperlinks() {
  grparse::DoclingMapper mapper;
  // Two bold runs, the second split from the first only by a hyperlink
  // boundary change back to plain, both bold: formatting stays uniform.
  officev1::StreamPagesResponse event;
  officev1::Paragraph* paragraph = event.mutable_paragraph();
  paragraph->set_list_level(-1);  // wire contract: -1 means not a list item
  officev1::TextRun bold_link = make_run("click", 0);
  bold_link.set_weight(150.0f);
  bold_link.set_hyperlink_url("https://example.test/a");
  officev1::TextRun bold_link_tail = make_run(" here", 5);
  bold_link_tail.set_weight(150.0f);
  bold_link_tail.set_hyperlink_url("https://example.test/a");
  officev1::TextRun bold_plain = make_run(" now", 10);
  bold_plain.set_weight(150.0f);
  *paragraph->add_runs() = bold_link;
  *paragraph->add_runs() = bold_link_tail;
  *paragraph->add_runs() = bold_plain;
  mapper.consume(event);
  const auto& base = mapper.document().texts(0).text().base();
  require(base.has_formatting() && base.formatting().bold() &&
              !base.formatting().italic(),
          "uniformly bold runs set item-level bold formatting");
  require(base.hyperlink() == "https://example.test/a",
          "the first hyperlink lands in the docling hyperlink slot");
  const auto& links =
      base.meta().custom_fields().at("hyperlinks").list_value();
  require(links.values_size() == 1,
          "adjacent runs of the same link merge into one entry");
  const auto& link = links.values(0).struct_value().fields();
  require(link.at("char_start").number_value() == 0.0 &&
              link.at("char_end").number_value() == 10.0,
          "the merged link covers both runs' characters");

  // Mixed formatting keeps the item's formatting unset.
  grparse::DoclingMapper mixed;
  officev1::StreamPagesResponse mixed_event;
  officev1::Paragraph* mixed_paragraph = mixed_event.mutable_paragraph();
  mixed_paragraph->set_list_level(-1);  // wire contract: -1 means not a list item
  officev1::TextRun italic = make_run("a");
  italic.set_italic(true);
  *mixed_paragraph->add_runs() = italic;
  *mixed_paragraph->add_runs() = make_run("b", 1);
  mixed.consume(mixed_event);
  require(!mixed.document().texts(0).text().base().has_formatting(),
          "mixed-format runs leave formatting unset");

  // All-plain runs also leave formatting unset (the flags carry no signal).
  grparse::DoclingMapper plain;
  plain.consume(paragraph_event("plain"));
  require(!plain.document().texts(0).text().base().has_formatting(),
          "all-default runs leave formatting unset");
}

void verify_table_fold_grid_and_off_grid_cells() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::TableData* table = event.mutable_table();
  table->set_rows(2);
  table->set_columns(2);
  const char* texts[2][2] = {{"a", "b"}, {"c", "d"}};
  for (int row = 0; row < 2; row++) {
    for (int column = 0; column < 2; column++) {
      officev1::TableCellData* cell = table->add_cells();
      cell->set_row(row);
      cell->set_column(column);
      cell->set_text(texts[row][column]);
    }
  }
  // A split cell outside the base grid stays addressable by office name.
  officev1::TableCellData* split = table->add_cells();
  split->set_row(-1);
  split->set_column(-1);
  split->set_name("B2.1");
  split->set_text("split text");
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.tables_size() == 1, "one TableData folds into one table");
  const docv1::TableItem& item = document.tables(0);
  require(item.data().num_rows() == 2 && item.data().num_cols() == 2,
          "grid dimensions survive the fold");
  require(item.data().table_cells_size() == 4,
          "off-grid cells are excluded from table_cells");
  require(item.data().grid_size() == 2 &&
              item.data().grid(1).cells(0).text() == "c",
          "the materialized grid places cell text by offsets");
  require(item.meta().custom_fields().at("cell:B2.1").string_value() ==
              "split text",
          "off-grid cell text rides custom_fields keyed by office name");
  require(grparse::docling_integrity_errors(document).empty(),
          "table mapping stays well formed");
}

void verify_out_of_grid_cell_offsets_do_not_write_the_grid() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::TableData* table = event.mutable_table();
  // The declared grid is smaller than the reported cell offsets: an
  // irregular office table can place a cell beyond rows x columns.
  table->set_rows(1);
  table->set_columns(1);
  officev1::TableCellData* inside = table->add_cells();
  inside->set_row(0);
  inside->set_column(0);
  inside->set_text("in");
  officev1::TableCellData* beyond_rows = table->add_cells();
  beyond_rows->set_row(2);
  beyond_rows->set_column(0);
  beyond_rows->set_text("row overflow");
  officev1::TableCellData* beyond_columns = table->add_cells();
  beyond_columns->set_row(0);
  beyond_columns->set_column(3);
  beyond_columns->set_text("column overflow");
  mapper.consume(event);
  const docv1::TableData& data = mapper.document().tables(0).data();
  require(data.table_cells_size() == 3,
          "overflowing cells stay addressable in table_cells");
  require(data.grid_size() == 1 && data.grid(0).cells_size() == 1,
          "the materialized grid keeps its declared dimensions");
  require(data.grid(0).cells(0).text() == "in",
          "only in-bounds cells land in the grid");
}

void verify_huge_grid_keeps_cells_only() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::TableData* table = event.mutable_table();
  // 65 x 64 = 4160 cells, just over the 4096 materialization bound.
  table->set_rows(65);
  table->set_columns(64);
  officev1::TableCellData* cell = table->add_cells();
  cell->set_row(0);
  cell->set_column(0);
  cell->set_text("corner");
  mapper.consume(event);
  const docv1::TableData& data = mapper.document().tables(0).data();
  require(data.grid_size() == 0,
          "grids above the cell bound skip materialization");
  require(data.table_cells_size() == 1 && data.table_cells(0).text() == "corner",
          "sparse cells are kept even when the grid is skipped");
}

void verify_sheet_rows_fold_into_the_sheet_table() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse sheet_event;
  officev1::Sheet* sheet = sheet_event.mutable_sheet();
  sheet->set_index(0);
  sheet->set_name("Data");
  sheet->set_visible(true);
  sheet->set_tab_color_rgb(-1);
  sheet->set_used_end_row(1);
  sheet->set_used_end_column(1);
  mapper.consume(sheet_event);

  officev1::StreamPagesResponse row_event;
  officev1::SheetRow* row = row_event.mutable_sheet_row();
  row->set_sheet_index(0);
  row->set_row(1);
  officev1::SheetCell* value_cell = row->add_cells();
  value_cell->set_column(0);
  value_cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
  value_cell->set_number(42.5);
  value_cell->set_display("42.5");
  officev1::SheetCell* formula_cell = row->add_cells();
  formula_cell->set_column(1);
  formula_cell->set_type(officev1::SHEET_CELL_TYPE_FORMULA);
  formula_cell->set_formula("=A2*2");
  formula_cell->set_number(85.0);
  formula_cell->set_display("85");
  mapper.consume(row_event);

  const docv1::Document& document = mapper.document();
  require(document.groups_size() == 1 &&
              document.groups(0).label() == docv1::GROUP_LABEL_SHEET &&
              document.groups(0).name() == "Data",
          "a sheet becomes a named sheet group");
  require(document.tables_size() == 1, "the sheet's grid is one table");
  const docv1::TableItem& table = document.tables(0);
  require(table.parent().ref() == document.groups(0).self_ref(),
          "the sheet table parents under the sheet group");
  require(table.data().num_rows() == 2 && table.data().num_cols() == 2,
          "used bounds size the sheet grid");
  require(table.data().table_cells_size() == 2 &&
              table.data().table_cells(0).text() == "42.5",
          "sheet cells keep their display text");
  require(table.meta().custom_fields().at("A2").number_value() == 42.5,
          "numeric cells keep their number keyed by A1 name");
  const auto& formula =
      table.meta().custom_fields().at("B2").struct_value().fields();
  require(formula.at("formula").string_value() == "=A2*2" &&
              formula.at("number").number_value() == 85.0,
          "formula cells keep expression and value keyed by A1 name");
  require(grparse::docling_integrity_errors(document).empty(),
          "sheet mapping stays well formed");
}

void verify_row_for_unknown_sheet_is_dropped() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse row_event;
  officev1::SheetRow* row = row_event.mutable_sheet_row();
  row->set_sheet_index(7);
  row->set_row(0);
  row->add_cells()->set_display("orphan");
  mapper.consume(row_event);
  require(mapper.document().tables_size() == 0,
          "a row for a never-announced sheet maps to nothing");
}

void verify_hidden_sheet_maps_to_invisible_layer() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::Sheet* sheet = event.mutable_sheet();
  sheet->set_index(0);
  sheet->set_name("Hidden");
  sheet->set_visible(false);
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.groups(0).content_layer() == docv1::CONTENT_LAYER_INVISIBLE,
          "hidden sheets land on the invisible layer");
  require(document.tables(0).content_layer() == docv1::CONTENT_LAYER_INVISIBLE,
          "the hidden sheet's table inherits the layer");
  require(document.groups(0).meta().custom_fields().at("visible").bool_value()
              == false,
          "visibility is recorded on the group");
}

void verify_metadata_maps_name_language_and_fields() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::DocumentMetadata* meta = event.mutable_metadata();
  meta->set_title("Quarterly Report");
  meta->set_author("Ada");
  meta->add_keywords("finance");
  meta->add_keywords("q3");
  meta->set_language("en-US");
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.name() == "Quarterly Report",
          "the stored title names the document");
  const auto& fields = document.body().meta().custom_fields();
  require(fields.at("author").string_value() == "Ada",
          "author rides the body custom fields");
  require(fields.at("keywords").list_value().values_size() == 2 &&
              fields.at("keywords").list_value().values(1).string_value() ==
                  "q3",
          "keywords keep their order in a list value");
  const docv1::LanguageMetaField& language = document.body().meta().language();
  require(language.code_raw() == "en-US",
          "the raw language tag is preserved");
  require(language.code() == docv1::HUMAN_LANGUAGE_LABEL_EN,
          "the primary subtag parses to the typed language label");
}

void verify_header_footer_lands_in_furniture() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  officev1::HeaderFooter* block = event.mutable_header_footer();
  block->set_footer(true);
  block->set_page_style("Default");
  officev1::Paragraph* paragraph = block->add_paragraphs();
  *paragraph->add_runs() = make_run("page 1 of 9");
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.texts_size() == 1, "each header/footer paragraph is a text");
  const auto& base = document.texts(0).text().base();
  require(base.label() == docv1::DOC_ITEM_LABEL_PAGE_FOOTER,
          "the footer flag picks the footer label");
  require(base.content_layer() == docv1::CONTENT_LAYER_FURNITURE &&
              base.parent().ref() == "#/furniture",
          "headers and footers parent under furniture");
  require(document.furniture().children_size() == 1,
          "the furniture root lists the footer text");
  require(grparse::docling_integrity_errors(document).empty(),
          "furniture mapping stays well formed");
}

void verify_take_moves_the_document_out() {
  grparse::DoclingMapper mapper;
  mapper.consume(paragraph_event("kept"));
  docv1::Document taken = mapper.take();
  require(taken.texts_size() == 1 &&
              taken.texts(0).text().base().text() == "kept",
          "take() hands over the accumulated document");
}

void verify_integrity_errors_flag_broken_references() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  // A dangling child under the body.
  document.mutable_body()->add_children()->set_ref("#/texts/9");
  // A text whose parent never lists it.
  auto* base = document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/body");
  const auto errors = grparse::docling_integrity_errors(document);
  bool dangling = false;
  bool unlisted = false;
  for (const std::string& error : errors) {
    if (error.find("#/texts/9") != std::string::npos &&
        error.find("does not resolve") != std::string::npos) {
      dangling = true;
    }
    if (error.find("does not list #/texts/0") != std::string::npos) {
      unlisted = true;
    }
  }
  require(dangling, "a dangling child reference is reported");
  require(unlisted, "a parent not listing its child is reported");

  docv1::Document empty_ref;
  empty_ref.add_texts()->mutable_text()->mutable_base();
  require(!grparse::docling_integrity_errors(empty_ref).empty(),
          "an item with an empty self_ref is reported");
}

}  // namespace

int main() {
  try {
    verify_fresh_mapper_builds_valid_skeleton();
    verify_document_info_maps_identity_and_pages();
    verify_unknown_extension_falls_back_to_octet_stream();
    verify_status_finishes_and_keeps_warnings();
    verify_unset_event_is_ignored();
    verify_paragraph_classification();
    verify_line_prov_is_page_local_with_charspans();
    verify_caret_prov_fallback_and_unresolved_page();
    verify_uniform_formatting_and_hyperlinks();
    verify_table_fold_grid_and_off_grid_cells();
    verify_out_of_grid_cell_offsets_do_not_write_the_grid();
    verify_huge_grid_keeps_cells_only();
    verify_sheet_rows_fold_into_the_sheet_table();
    verify_row_for_unknown_sheet_is_dropped();
    verify_hidden_sheet_maps_to_invisible_layer();
    verify_metadata_maps_name_language_and_fields();
    verify_header_footer_lands_in_furniture();
    verify_take_moves_the_document_out();
    verify_integrity_errors_flag_broken_references();
    std::println("docling-map-test: all checks passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "docling-map-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
