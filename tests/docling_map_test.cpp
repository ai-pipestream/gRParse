#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

using grparse_test::require;

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
  require(first.charspan().start() == 0 && first.charspan().end() == 5,
          "measured lines narrow their item-relative charspan");
  require(base.prov(1).charspan().start() == 0 &&
              base.prov(1).charspan().end() == 11,
          "unmeasured lines keep the full item span");
}

// A document-absolute box on a page whose rectangle never arrived cannot be
// reduced to page-local, and the emitted box says page-local regardless. The
// fold says so instead of leaving the consumer to trust it, and says it once
// per page rather than once per box.
void verify_unknown_page_rect_warns_instead_of_stamping_silently() {
  grparse::DoclingMapper mapper;
  // No DocumentInfo, so no page rectangle is known for any page.
  officev1::StreamPagesResponse event;
  officev1::Paragraph* paragraph = event.mutable_paragraph();
  paragraph->set_list_level(-1);  // wire contract: -1 means not a list item
  paragraph->set_char_offset(0);
  *paragraph->add_runs() = make_run("unplaced");
  for (int line = 0; line < 2; ++line) {
    officev1::LineBox* box = paragraph->add_line_rects();
    box->set_page_index(2);
    box->set_x_twips(1000);
    box->set_y_twips(21000 + line * 200);
    box->set_width_twips(500);
    box->set_height_twips(200);
    box->set_char_start(-1);
    box->set_char_end(-1);
  }
  mapper.consume(event);

  const auto& base = mapper.document().texts(0).text().base();
  require(base.prov_size() == 2, "the item keeps its provenance, page number included");
  require(base.prov(0).bbox().t() == 21000.0,
          "with no page rectangle the box is left exactly as it came");
  require(mapper.warnings().size() == 1,
          "one warning per unresolved page, not one per box");
  require(mapper.warnings()[0].contains("page 3") &&
              mapper.warnings()[0].contains("document-absolute"),
          "the warning names the page and the coordinate space: " + mapper.warnings()[0]);

  // A second page with no rectangle is its own warning.
  officev1::StreamPagesResponse other;
  officev1::Paragraph* elsewhere = other.mutable_paragraph();
  elsewhere->set_list_level(-1);
  elsewhere->set_page_index(4);
  elsewhere->mutable_start()->set_x(10);
  elsewhere->mutable_start()->set_y(20);
  elsewhere->mutable_end()->set_x(30);
  elsewhere->mutable_end()->set_y(40);
  *elsewhere->add_runs() = make_run("also unplaced");
  mapper.consume(other);
  require(mapper.warnings().size() == 2 && mapper.warnings()[1].contains("page 5"),
          "each unresolved page warns once");
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
  // Every link reaches the item as an inline span carrying its own range,
  // and adjacent runs agreeing on everything coalesce into one span.
  int link_spans = 0;
  for (const docv1::InlineSpan& span : base.spans()) {
    if (span.hyperlink() != "https://example.test/a") continue;
    link_spans++;
    require(span.range().start() == 0 && span.range().end() == 10,
            "the merged link span covers both runs' characters");
  }
  require(link_spans == 1,
          "adjacent runs of the same link coalesce into one span");
  require(base.meta().custom_fields().empty(),
          "links no longer need a value map");

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
  // A split cell anchors at the base-grid cell its office name starts from.
  officev1::TableCellData* split = table->add_cells();
  split->set_row(-1);
  split->set_column(-1);
  split->set_name("B2.1");
  split->set_text("split text");
  // A name that anchors nowhere still keeps its text.
  officev1::TableCellData* stray = table->add_cells();
  stray->set_row(-1);
  stray->set_column(-1);
  stray->set_name("?");
  stray->set_text("stray text");
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.tables_size() == 1, "one TableData folds into one table");
  const docv1::TableItem& item = document.tables(0);
  require(item.data().num_rows() == 2 && item.data().num_cols() == 2,
          "grid dimensions survive the fold");
  require(item.data().table_cells_size() == 5,
          "an anchored split cell is a cell, not a custom field");
  require(item.data().grid_size() == 2 &&
              item.data().grid(1).cells(0).text() == "c",
          "the materialized grid places cell text by offsets");
  require(item.data().grid(1).cells(1).text() == "d",
          "a base cell keeps its grid slot against a later split cell");
  bool split_placed = false;
  for (const docv1::TableCell& cell : item.data().table_cells()) {
    if (cell.text() != "split text") continue;
    split_placed = cell.start_row_offset_idx() == 1 &&
                   cell.start_col_offset_idx() == 1;
  }
  require(split_placed, "the split cell anchors where its name says");
  require(item.meta().custom_fields().count("cell:B2.1") == 0,
          "an anchored split cell needs no custom field");
  require(item.meta().custom_fields().at("cell:?").string_value() ==
              "stray text",
          "a cell name anchoring nowhere still keeps its text");
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
  require(table.data().table_cells(0).value().number() == 42.5,
          "a numeric cell carries its number on the cell");
  require(table.data().table_cells(1).value().formula() == "=A2*2",
          "a formula cell carries its expression on the cell");
  require(table.meta().custom_fields().count("A2") == 0,
          "no A1-keyed side map beside the grid");
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
  require(!document.groups(0).sheet().visible(),
          "visibility is a typed sheet attribute on the group");
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
  const docv1::DocumentMeta& source_meta = document.source_meta();
  require(source_meta.authors_size() == 1 && source_meta.authors(0) == "Ada",
          "the author is typed on the document metadata");
  require(source_meta.keywords_size() == 2 &&
              source_meta.keywords(1) == "q3",
          "keywords keep their order");
  require(document.body().meta().keywords().values_size() == 2,
          "keywords also reach the body keywords field");
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

// The page style each page carries lands on that page and resolves into
// the style catalogue, which streams in after the page images do.
void verify_per_page_style() {
  auto page_event = [](int index, const std::string& style) {
    officev1::StreamPagesResponse event;
    officev1::PageImage* image = event.mutable_page_image();
    image->set_index(index);
    image->set_width_px(816);
    image->set_height_px(1056);
    image->set_dpi(96);
    image->set_png("pngbytes");
    image->set_format(officev1::PAGE_IMAGE_FORMAT_PNG);
    image->set_page_style(style);
    return event;
  };
  auto style_event = [](const std::string& name) {
    officev1::StreamPagesResponse event;
    officev1::PageStyleInfo* style = event.mutable_page_style();
    style->set_name(name);
    style->set_width_twips(11906);
    style->set_height_twips(16838);
    style->set_columns(1);
    return event;
  };
  auto status_event = []() {
    officev1::StreamPagesResponse event;
    event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
    return event;
  };

  {
    // Two pages, two styles, both declared: each page names its own.
    grparse::DoclingMapper mapper;
    mapper.consume(document_info_event());
    mapper.consume(page_event(0, "First Page"));
    mapper.consume(page_event(1, "Standard"));
    mapper.consume(style_event("First Page"));
    mapper.consume(style_event("Standard"));
    mapper.consume(status_event());
    const docv1::Document& document = mapper.document();
    require(document.pages().at(1).style_name() == "First Page",
            "the first page names the style in force on it");
    require(document.pages().at(2).style_name() == "Standard",
            "the style change lands on the page it starts");
    require(mapper.warnings().empty(),
            "a resolved catalogue warns about nothing");
  }
  {
    // A page the office core named nothing for stays unnamed rather than
    // inheriting its neighbour.
    grparse::DoclingMapper mapper;
    mapper.consume(document_info_event());
    mapper.consume(page_event(0, "Standard"));
    mapper.consume(page_event(1, ""));
    mapper.consume(style_event("Standard"));
    mapper.consume(status_event());
    require(!mapper.document().pages().at(2).has_style_name(),
            "an unnamed page stays unnamed");
  }
  {
    // A name the catalogue does not declare is kept, because it is still
    // what the layout reported, and the divergence is named in a warning.
    grparse::DoclingMapper mapper;
    mapper.consume(document_info_event());
    mapper.consume(page_event(0, "Envelope"));
    mapper.consume(style_event("Standard"));
    mapper.consume(status_event());
    require(mapper.document().pages().at(1).style_name() == "Envelope",
            "an undeclared name is kept, not dropped");
    require(mapper.warnings().size() == 1 &&
                mapper.warnings()[0].contains("Envelope") &&
                mapper.warnings()[0].contains("does not declare"),
            "an undeclared name is reported once");
  }
  {
    // Without the catalogue there is nothing to resolve against, so the
    // name rides through unchecked and unremarked.
    grparse::DoclingMapper mapper;
    mapper.consume(document_info_event());
    mapper.consume(page_event(0, "Envelope"));
    mapper.consume(status_event());
    require(mapper.document().pages().at(1).style_name() == "Envelope",
            "no catalogue still keeps the name");
    require(mapper.warnings().empty(),
            "no catalogue means no divergence to report");
  }
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

// True when some error mentions every fragment given.
bool reported(const std::vector<std::string>& errors,
              const std::vector<std::string>& fragments) {
  for (const std::string& error : errors) {
    bool all = true;
    for (const std::string& fragment : fragments) {
      if (!error.contains(fragment)) {
        all = false;
        break;
      }
    }
    if (all) return true;
  }
  return false;
}

// The four form arenas link like every other arena, so they are held to the
// same contract: self_ref shape, parent resolution, children resolution, and
// the item_refs their graph cells point at.
void verify_integrity_errors_cover_the_form_arenas() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");

  // A key-value item under a parent that does not exist, with a graph cell
  // pointing at an absent text item.
  auto* key_value = document.add_key_value_items();
  key_value->set_self_ref("#/key_value_items/0");
  key_value->mutable_parent()->set_ref("#/groups/7");
  auto* cell = key_value->mutable_graph()->add_cells();
  cell->set_cell_id(1);
  cell->mutable_item_ref()->set_ref("#/texts/4");

  // A form item whose child does not exist.
  auto* form = document.add_form_items();
  form->set_self_ref("#/form_items/0");
  form->mutable_parent()->set_ref("#/body");
  form->add_children()->set_ref("#/field_items/9");
  document.mutable_body()->add_children()->set_ref("#/form_items/0");

  // A field region with no self_ref at all.
  document.add_field_regions()->mutable_parent()->set_ref("#/body");

  // A field item duplicating a self_ref already taken.
  auto* field = document.add_field_items();
  field->set_self_ref("#/form_items/0");
  field->add_prov()->set_page_no(0);

  const auto errors = grparse::docling_integrity_errors(document);
  require(reported(errors, {"#/groups/7", "#/key_value_items/0", "does not resolve"}),
          "an unresolvable key-value parent is reported");
  require(reported(errors, {"graph cell item_ref", "#/texts/4", "does not resolve"}),
          "an unresolvable graph-cell item_ref is reported");
  require(reported(errors, {"#/field_items/9", "#/form_items/0", "does not resolve"}),
          "an unresolvable form child is reported");
  require(reported(errors, {"empty self_ref"}),
          "a field region with no self_ref is reported");
  require(reported(errors, {"duplicate self_ref", "#/form_items/0"}),
          "a field item reusing a taken self_ref is reported");
  require(reported(errors, {"page_no 0", "1-based"}),
          "provenance on the proto3 default page is reported");

  // The same shapes, wired correctly, are clean.
  docv1::Document sound;
  sound.mutable_body()->set_self_ref("#/body");
  sound.mutable_furniture()->set_self_ref("#/furniture");
  auto* text = sound.add_texts()->mutable_text()->mutable_base();
  text->set_self_ref("#/texts/0");
  text->mutable_parent()->set_ref("#/form_items/0");
  text->add_prov()->set_page_no(1);
  auto* sound_form = sound.add_form_items();
  sound_form->set_self_ref("#/form_items/0");
  sound_form->mutable_parent()->set_ref("#/body");
  sound_form->add_children()->set_ref("#/texts/0");
  sound_form->add_prov()->set_page_no(1);
  auto* sound_cell = sound_form->mutable_graph()->add_cells();
  sound_cell->set_cell_id(1);
  sound_cell->mutable_item_ref()->set_ref("#/texts/0");
  sound_cell->mutable_prov()->set_page_no(1);
  auto* region = sound.add_field_regions();
  region->set_self_ref("#/field_regions/0");
  region->mutable_parent()->set_ref("#/body");
  auto* sound_field = sound.add_field_items();
  sound_field->set_self_ref("#/field_items/0");
  sound_field->mutable_parent()->set_ref("#/body");
  auto* pair = sound.add_key_value_items();
  pair->set_self_ref("#/key_value_items/0");
  pair->mutable_parent()->set_ref("#/body");
  sound.mutable_body()->add_children()->set_ref("#/form_items/0");
  sound.mutable_body()->add_children()->set_ref("#/field_regions/0");
  sound.mutable_body()->add_children()->set_ref("#/field_items/0");
  sound.mutable_body()->add_children()->set_ref("#/key_value_items/0");
  const auto clean = grparse::docling_integrity_errors(sound);
  require(clean.empty(),
          "a sound form arena reports nothing: " +
              (clean.empty() ? std::string() : clean.front()));
}

// ---- charts and sheet headers ---------------------------------------------

officev1::StreamPagesResponse sheet_event(int index, const std::string& name,
                                          int used_end_row, int used_end_column) {
  officev1::StreamPagesResponse event;
  officev1::Sheet* sheet = event.mutable_sheet();
  sheet->set_index(index);
  sheet->set_name(name);
  sheet->set_visible(true);
  sheet->set_tab_color_rgb(-1);
  sheet->set_used_end_row(used_end_row);
  sheet->set_used_end_column(used_end_column);
  return event;
}

officev1::SheetCell* text_cell(officev1::SheetRow* row, int column,
                               const std::string& text) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_TEXT);
  cell->set_display(text);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
  return cell;
}

officev1::SheetCell* number_cell(officev1::SheetRow* row, int column, double value) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
  cell->set_number(value);
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%g", value);
  cell->set_display(buffer);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
  return cell;
}

officev1::StreamPagesResponse row_event(int sheet_index, int row) {
  officev1::StreamPagesResponse event;
  event.mutable_sheet_row()->set_sheet_index(sheet_index);
  event.mutable_sheet_row()->set_row(row);
  return event;
}

// A three-category, two-series column chart as the office collector emits
// it off a sheet or slide draw page: page-local position, a replacement
// graphic, typed series, and the tabular projection.
officev1::StreamPagesResponse chart_object_event(int page_index, const std::string& name,
                                                 long long x, long long y) {
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
  officev1::TableData* tabular = chart->mutable_tabular();
  tabular->set_rows(4);
  tabular->set_columns(3);
  return event;
}

officev1::StreamPagesResponse sheet_chart_event(int sheet_index, const std::string& name,
                                                int end_row, int end_column) {
  officev1::StreamPagesResponse event;
  officev1::SheetChart* chart = event.mutable_sheet_chart();
  chart->set_sheet_index(sheet_index);
  chart->set_name(name);
  officev1::SheetRangeRef* range = chart->add_ranges();
  range->set_start_row(0);
  range->set_start_column(0);
  range->set_end_row(end_row);
  range->set_end_column(end_column);
  chart->set_has_column_headers(true);
  chart->set_has_row_headers(true);
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

officev1::StreamPagesResponse spreadsheet_info_event() {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("book.xlsx");
  info->set_source_format("xlsx");
  info->set_page_count(1);
  info->set_document_type("spreadsheet");
  officev1::PageRect* page = info->add_page_rects();
  page->set_width_twips(24000);
  page->set_height_twips(15000);
  return event;
}

// The sheet behind the chart: a header row and three data rows.
void feed_sales_sheet(grparse::DoclingMapper* mapper) {
  mapper->consume(sheet_event(0, "Sales", 3, 2));
  officev1::StreamPagesResponse header = row_event(0, 0);
  text_cell(header.mutable_sheet_row(), 0, "Region");
  text_cell(header.mutable_sheet_row(), 1, "Q1");
  text_cell(header.mutable_sheet_row(), 2, "Q2");
  mapper->consume(header);
  const char* regions[] = {"North", "South", "West"};
  const double q1[] = {120.0, 80.0, 64.0};
  const double q2[] = {135.5, 97.0, 70.25};
  for (int i = 0; i < 3; i++) {
    officev1::StreamPagesResponse row = row_event(0, i + 1);
    text_cell(row.mutable_sheet_row(), 0, regions[i]);
    number_cell(row.mutable_sheet_row(), 1, q1[i]);
    number_cell(row.mutable_sheet_row(), 2, q2[i]);
    mapper->consume(row);
  }
}

const docv1::TableCell* cell_at(const docv1::TableData& data, int row, int column) {
  for (const docv1::TableCell& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == row && cell.start_col_offset_idx() == column) {
      return &cell;
    }
  }
  return nullptr;
}

std::string caption_text(const docv1::Document& document, const docv1::PictureItem& picture) {
  require(picture.captions_size() == 1, "the chart carries one caption");
  const std::string& ref = picture.captions(0).ref();
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.has_text() && item.text().base().self_ref() == ref) {
      require(item.text().base().label() == docv1::DOC_ITEM_LABEL_CAPTION,
              "the caption is a CAPTION item");
      require(item.text().base().parent().ref() == picture.self_ref(),
              "the caption parents under the chart");
      return item.text().base().text();
    }
  }
  throw std::runtime_error("caption " + ref + " is not a text item");
}

void verify_sheet_chart_pairs_with_its_embedded_object() {
  grparse::DoclingMapper mapper;
  mapper.consume(spreadsheet_info_event());
  // The draw-page object arrives before the sheet, as the collector emits it.
  mapper.consume(chart_object_event(0, "Chart 1", 4779, 299));
  const docv1::Document& streaming = mapper.document();
  require(streaming.pictures_size() == 0,
          "a sheet chart waits for the sheet that places it");
  feed_sales_sheet(&mapper);
  mapper.consume(sheet_chart_event(0, "Object 1", 3, 2));
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1, "one chart, one picture: no empty twin");
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.label() == docv1::DOC_ITEM_LABEL_CHART, "the picture is a CHART item");
  require(picture.parent().ref() == document.groups(0).self_ref(),
          "the chart sits under its sheet group");
  require(picture.has_image() && picture.image().mimetype() == "image/png",
          "the replacement graphic rides on the chart");
  require(picture.has_chart() && picture.chart().sources_size() == 1 &&
              picture.chart().sources(0).end().row() == 3 &&
              picture.chart().sources(0).start().sheet() == "Sales" &&
              picture.chart().has_column_headers(),
          "the sheet ranges are the chart's typed provenance");
  require(picture.prov_size() == 1 && picture.prov(0).page_no() == 1 &&
              picture.prov(0).bbox().l() == 4779 && picture.prov(0).bbox().r() == 12779,
          "the chart keeps the object's laid-out box");
  require(picture.shape().name() == "Chart 1", "the object's name is the shape name");
  require(caption_text(document, picture) == "Revenue by region",
          "the chart title is the caption");

  // The bound table: a child of the chart, the series as columns.
  require(document.tables_size() == 2, "the sheet table and the chart's own table");
  const docv1::TableItem& table = document.tables(1);
  require(table.parent().ref() == picture.self_ref(), "the data table parents under the chart");
  bool listed = false;
  for (const docv1::RefItem& child : picture.children()) {
    if (child.ref() == table.self_ref()) listed = true;
  }
  require(listed, "the chart lists its data table among its children");
  const docv1::TableData& data = table.data();
  require(data.num_rows() == 4 && data.num_cols() == 3, "categories down, series across");
  const docv1::TableCell* corner = cell_at(data, 0, 0);
  const docv1::TableCell* q2 = cell_at(data, 0, 2);
  const docv1::TableCell* south = cell_at(data, 2, 0);
  const docv1::TableCell* value = cell_at(data, 3, 2);
  require(corner != nullptr && corner->text() == "Region" && corner->column_header(),
          "the axis title heads the category column");
  require(q2 != nullptr && q2->text() == "Q2" && q2->column_header(),
          "series labels are column headers");
  require(south != nullptr && south->text() == "South" && south->row_header(),
          "categories are row headers");
  require(value != nullptr && value->text() == "70.25" && value->value().number() == 70.25,
          "values stay numeric beside their display text");
  require(data.columns_size() == 3 && data.columns(0).name() == "Region" &&
              data.columns(2).name() == "Q2" && data.columns(2).declared_type() == "number",
          "the column schema names the axis and the series");
  require(data.grid_size() == 4 && data.grid(0).cells(1).column_header(),
          "the grid mirrors the header flags");
  require(data.row_prov_size() == 4 && data.row_prov(0).grid().row() == 0 &&
              data.row_prov(3).grid().row() == 3 && data.row_prov(3).grid().sheet() == "Sales",
          "each table row points back at its sheet row");
  require(table.prov_size() == 1 && table.prov(0).bbox().l() == 4779,
          "the table shares the chart's box");
  require(table.source_size() == 1 && table.source(0).collector().collector() == "libreoffice",
          "the table is attributed like every mapped item");

  bool bar = false;
  bool tabular = false;
  for (const docv1::PictureAnnotation& annotation : picture.annotations()) {
    if (annotation.has_bar_chart()) bar = annotation.bar_chart().y_axis_label() == "kUSD";
    if (annotation.has_tabular_chart()) tabular = true;
  }
  require(bar && tabular, "the typed annotations stay on the chart");
  require(document.attachments_size() == 1 && document.attachments(0).item_ref() == picture.self_ref(),
          "the embedded object is registered once, against the chart");
  require(grparse::docling_integrity_errors(document).empty(),
          "the chart composite stays well formed");
}

void verify_sheet_chart_without_object_folds_the_sheet_cells() {
  grparse::DoclingMapper mapper;
  mapper.consume(spreadsheet_info_event());
  feed_sales_sheet(&mapper);
  mapper.consume(sheet_chart_event(0, "Object 1", 3, 2));
  mapper.consume(status_event());
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1 && document.tables_size() == 2,
          "a sheet chart with no object still binds a table");
  const docv1::TableData& data = document.tables(1).data();
  require(data.num_rows() == 4 && data.num_cols() == 3, "the table is the source range");
  const docv1::TableCell* header = cell_at(data, 0, 1);
  const docv1::TableCell* label = cell_at(data, 2, 0);
  const docv1::TableCell* value = cell_at(data, 1, 2);
  require(header != nullptr && header->text() == "Q1" && header->column_header(),
          "has_column_headers marks the first range row");
  require(label != nullptr && label->text() == "South" && label->row_header(),
          "has_row_headers marks the first range column");
  require(value != nullptr && value->value().number() == 135.5,
          "sheet values keep their typed value in the chart table");
  require(!document.pictures(0).has_image(), "no object, no image to carry");
  require(document.pictures(0).captions_size() == 0, "no title known, no caption minted");
  require(grparse::docling_integrity_errors(document).empty(), "the fallback stays well formed");
}

void verify_unplaced_chart_flushes_under_its_sheet_at_the_end() {
  grparse::DoclingMapper mapper;
  mapper.consume(spreadsheet_info_event());
  mapper.consume(chart_object_event(0, "Chart 1", 100, 200));
  mapper.consume(sheet_event(0, "Empty", 0, 0));
  require(mapper.document().pictures_size() == 0, "still waiting after the sheet header");
  mapper.consume(status_event());
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1 &&
              document.pictures(0).parent().ref() == document.groups(0).self_ref(),
          "the stream end places a chart no SheetChart claimed under its sheet");
  require(document.tables_size() == 2 && cell_at(document.tables(1).data(), 1, 1) != nullptr,
          "the flushed chart binds its series table");
  require(grparse::docling_integrity_errors(document).empty(), "the flush stays well formed");
}

void verify_slide_ole_shape_places_the_chart() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse info_event;
  officev1::DocumentInfo* info = info_event.mutable_document_info();
  info->set_document_id("deck.pptx");
  info->set_source_format("pptx");
  info->set_page_count(1);
  info->set_document_type("presentation");
  info->add_page_rects()->set_width_twips(14400);
  mapper.consume(info_event);
  mapper.consume(chart_object_event(0, "Chart 2", 1440, 2160));

  officev1::StreamPagesResponse slide_event;
  slide_event.mutable_slide()->set_index(0);
  slide_event.mutable_slide()->set_name("Trend");
  mapper.consume(slide_event);

  officev1::StreamPagesResponse title_event;
  officev1::SlideShape* title = title_event.mutable_slide_shape();
  title->set_slide_index(0);
  title->set_shape_type("com.sun.star.presentation.TitleTextShape");
  title->set_placeholder_role(officev1::PLACEHOLDER_ROLE_TITLE);
  *title->add_paragraphs()->add_runs() = make_run("Trend");
  mapper.consume(title_event);

  // The chart's own shape: an OLE2 shape with one empty run, as the office
  // core reports object shapes.
  officev1::StreamPagesResponse ole_event;
  officev1::SlideShape* ole = ole_event.mutable_slide_shape();
  ole->set_slide_index(0);
  ole->set_z_order(1);
  ole->set_shape_type("com.sun.star.drawing.OLE2Shape");
  ole->mutable_position()->set_x(1440);
  ole->mutable_position()->set_y(2160);
  ole->set_width_twips(8000);
  ole->set_height_twips(4000);
  ole->add_paragraphs()->add_runs()->set_char_offset(-1);
  mapper.consume(ole_event);
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1, "the OLE2 shape and the object are one chart");
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.label() == docv1::DOC_ITEM_LABEL_CHART &&
              picture.parent().ref() == document.groups(0).self_ref(),
          "the chart sits under its slide, where the shape walk placed it");
  const docv1::GroupItem& slide = document.groups(0);
  require(slide.children_size() == 2 && slide.children(1).ref() == picture.self_ref(),
          "the chart follows the title in slide order");
  int empty_texts = 0;
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.has_text() && item.text().base().text().empty()) empty_texts++;
  }
  require(empty_texts == 0, "the object shape's empty run makes no text item");
  require(caption_text(document, picture) == "Revenue by region", "the slide chart is captioned");
  require(document.tables_size() == 1 &&
              document.tables(0).parent().ref() == picture.self_ref(),
          "the slide chart binds its table");
  require(grparse::docling_integrity_errors(document).empty(), "the slide chart stays well formed");
}

void verify_writer_chart_binds_on_arrival() {
  grparse::DoclingMapper mapper;
  mapper.consume(document_info_event());
  officev1::StreamPagesResponse event = chart_object_event(0, "Object 3", 0, 0);
  officev1::EmbeddedObject* object = event.mutable_embedded_object();
  object->clear_position();
  object->mutable_anchor()->set_x(1000);
  object->mutable_anchor()->set_y(2000);
  mapper.consume(event);
  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1 && document.pictures(0).parent().ref() == "#/body",
          "a text document's chart is placed by its anchor as it arrives");
  require(document.tables_size() == 1 && document.tables(0).data().num_rows() == 4,
          "and binds its series table at once");
  require(document.pictures(0).prov(0).bbox().t() == 2000,
          "the anchor is the box's origin on its page");
}

officev1::StreamPagesResponse image_event(int index, int page_index, long long x,
                                          long long y, long long height) {
  officev1::StreamPagesResponse event;
  officev1::EmbeddedImage* image = event.mutable_embedded_image();
  image->set_index(index);
  image->set_page_index(page_index);
  image->set_name("Picture " + std::to_string(index + 1));
  image->set_mime_type("image/png");
  image->set_data("png");
  image->set_width_twips(4000);
  image->set_height_twips(height);
  image->mutable_anchor()->set_x(x);
  image->mutable_anchor()->set_y(y);
  return event;
}

officev1::StreamPagesResponse empty_paragraph_event(int page_index, long long caret_y,
                                                    const std::string& text = "") {
  officev1::StreamPagesResponse event = paragraph_event(text);
  event.mutable_paragraph()->set_page_index(page_index);
  event.mutable_paragraph()->mutable_start()->set_y(caret_y);
  event.mutable_paragraph()->mutable_end()->set_y(caret_y);
  return event;
}

void verify_inline_pictures_take_their_anchor_paragraphs_place() {
  grparse::DoclingMapper mapper;
  mapper.consume(document_info_event());
  mapper.consume(paragraph_event("Documents may contain images."));
  // The anchor paragraph: one empty run, caret at the picture's baseline,
  // below the picture's top by its height.
  mapper.consume(empty_paragraph_event(0, 14108));
  mapper.consume(paragraph_event("Alt text should communicate meaning."));
  mapper.consume(empty_paragraph_event(0, 15000, " "));  // a spacer
  mapper.consume(paragraph_event("Tables follow."));
  // Images arrive after every paragraph, as the collector emits them.
  mapper.consume(image_event(0, 0, 3929, 12398, 1958));
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  require(document.texts_size() == 3, "empty and whitespace paragraphs make no text items");
  for (const docv1::BaseTextItem& item : document.texts()) {
    require(!item.text().base().text().empty(), "no text item is empty");
  }
  const auto& body = document.body().children();
  require(body.size() == 4, "three paragraphs and one picture in the body");
  require(body[0].ref() == "#/texts/0" && body[1].ref() == "#/pictures/0" &&
              body[2].ref() == "#/texts/1" && body[3].ref() == "#/texts/2",
          "the picture sits where its anchor paragraph was");
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.prov_size() == 1 && picture.prov(0).page_no() == 1 &&
              picture.prov(0).bbox().l() == 3929 && picture.prov(0).bbox().t() == 12398 &&
              picture.prov(0).bbox().r() == 7929 && picture.prov(0).bbox().b() == 14356,
          "the picture carries its page and box");
  require(grparse::docling_integrity_errors(document).empty(), "placement stays well formed");
}

void verify_unanchored_picture_still_trails_the_body() {
  grparse::DoclingMapper mapper;
  mapper.consume(document_info_event());
  mapper.consume(paragraph_event("Only text here."));
  mapper.consume(empty_paragraph_event(1, 21000));  // wrong page for the picture
  mapper.consume(image_event(0, 0, 100, 5000, 1000));
  mapper.consume(status_event());
  const auto& body = mapper.document().body().children();
  require(body.size() == 2 && body[1].ref() == "#/pictures/0",
          "a picture with no matching slot keeps its arrival position");
}

void verify_slide_titles_after_the_first_are_section_headings() {
  grparse::DoclingMapper mapper;
  officev1::StreamPagesResponse info_event;
  officev1::DocumentInfo* info = info_event.mutable_document_info();
  info->set_document_type("presentation");
  info->set_page_count(2);
  info->add_page_rects()->set_width_twips(14400);
  info->add_page_rects()->set_width_twips(14400);
  mapper.consume(info_event);
  for (int slide_index = 0; slide_index < 2; slide_index++) {
    officev1::StreamPagesResponse slide_event;
    slide_event.mutable_slide()->set_index(slide_index);
    mapper.consume(slide_event);
    officev1::StreamPagesResponse title_event;
    officev1::SlideShape* title = title_event.mutable_slide_shape();
    title->set_slide_index(slide_index);
    title->set_shape_type("com.sun.star.presentation.TitleTextShape");
    title->set_placeholder_role(officev1::PLACEHOLDER_ROLE_TITLE);
    *title->add_paragraphs()->add_runs() = make_run(slide_index == 0 ? "Deck" : "Agenda");
    mapper.consume(title_event);
  }
  const docv1::Document& document = mapper.document();
  require(document.texts_size() == 2, "two slide titles");
  require(document.texts(0).has_title() && document.texts(0).title().base().text() == "Deck",
          "the first slide's title is the deck title");
  require(document.texts(1).has_section_header() &&
              document.texts(1).section_header().level() == 1 &&
              document.texts(1).section_header().base().text() == "Agenda",
          "a later slide's title is a level-1 section heading");
}

void verify_sheet_header_rows_are_marked() {
  grparse::DoclingMapper mapper;
  mapper.consume(spreadsheet_info_event());
  // Sheet 0: a merged title above a header row above quantities.
  mapper.consume(sheet_event(0, "Report", 3, 2));
  officev1::StreamPagesResponse title = row_event(0, 0);
  officev1::SheetCell* merged = text_cell(title.mutable_sheet_row(), 0, "Quarterly totals");
  merged->set_merged_columns(3);
  mapper.consume(title);
  officev1::StreamPagesResponse header = row_event(0, 1);
  text_cell(header.mutable_sheet_row(), 0, "Item");
  text_cell(header.mutable_sheet_row(), 1, "Units");
  text_cell(header.mutable_sheet_row(), 2, "Price");
  mapper.consume(header);
  officev1::StreamPagesResponse widget = row_event(0, 2);
  text_cell(widget.mutable_sheet_row(), 0, "Widget");
  number_cell(widget.mutable_sheet_row(), 1, 10);
  number_cell(widget.mutable_sheet_row(), 2, 2.5);
  mapper.consume(widget);
  officev1::StreamPagesResponse bundle = row_event(0, 3);
  officev1::SheetCell* tall = text_cell(bundle.mutable_sheet_row(), 0, "Bundle");
  tall->set_merged_rows(2);
  number_cell(bundle.mutable_sheet_row(), 1, 1);
  mapper.consume(bundle);
  // Sheet 1: all text, nothing numeric below: no header claimed.
  mapper.consume(sheet_event(1, "Notes", 1, 1));
  officev1::StreamPagesResponse first = row_event(1, 0);
  text_cell(first.mutable_sheet_row(), 0, "alpha");
  text_cell(first.mutable_sheet_row(), 1, "beta");
  mapper.consume(first);
  officev1::StreamPagesResponse second = row_event(1, 1);
  text_cell(second.mutable_sheet_row(), 0, "gamma");
  text_cell(second.mutable_sheet_row(), 1, "delta");
  mapper.consume(second);
  // Sheet 2: a database range declares its header outright, even over
  // text-only rows.
  officev1::StreamPagesResponse database;
  officev1::SheetDatabaseRange* range = database.mutable_sheet_database_range();
  range->set_name("Names");
  range->set_sheet_index(2);
  range->set_contains_header(true);
  range->mutable_range()->set_start_row(0);
  range->mutable_range()->set_start_column(0);
  range->mutable_range()->set_end_row(1);
  range->mutable_range()->set_end_column(1);
  mapper.consume(database);
  mapper.consume(sheet_event(2, "Names", 1, 1));
  officev1::StreamPagesResponse names_header = row_event(2, 0);
  text_cell(names_header.mutable_sheet_row(), 0, "first");
  text_cell(names_header.mutable_sheet_row(), 1, "last");
  mapper.consume(names_header);
  officev1::StreamPagesResponse names_row = row_event(2, 1);
  text_cell(names_row.mutable_sheet_row(), 0, "Ada");
  text_cell(names_row.mutable_sheet_row(), 1, "Lovelace");
  mapper.consume(names_row);
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  const docv1::TableData& report = document.tables(0).data();
  require(cell_at(report, 0, 0)->row_section() && !cell_at(report, 0, 0)->column_header() &&
              cell_at(report, 0, 0)->col_span() == 3,
          "a merged title spanning the width is a section row");
  require(cell_at(report, 1, 0)->column_header() && cell_at(report, 1, 2)->column_header(),
          "the label row above quantities is the header row");
  require(!cell_at(report, 2, 0)->column_header() && !cell_at(report, 2, 1)->column_header(),
          "data rows are not headers");
  require(cell_at(report, 3, 0)->row_span() == 2 && cell_at(report, 3, 0)->end_row_offset_idx() == 5,
          "a merged cell keeps its row span");
  const docv1::TableData& notes = document.tables(1).data();
  for (const docv1::TableCell& cell : notes.table_cells()) {
    require(!cell.column_header() && !cell.row_section(), "text-only sheets claim no header");
  }
  const docv1::TableData& names = document.tables(2).data();
  require(cell_at(names, 0, 0)->column_header() && cell_at(names, 0, 1)->column_header() &&
              !cell_at(names, 1, 0)->column_header(),
          "a database range with a header names the header row");
  require(grparse::docling_integrity_errors(document).empty(), "header marking stays well formed");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("docling-map-test", "all checks passed", {
      verify_fresh_mapper_builds_valid_skeleton,
      verify_document_info_maps_identity_and_pages,
      verify_unknown_extension_falls_back_to_octet_stream,
      verify_status_finishes_and_keeps_warnings,
      verify_unset_event_is_ignored,
      verify_paragraph_classification,
      verify_line_prov_is_page_local_with_charspans,
      verify_unknown_page_rect_warns_instead_of_stamping_silently,
      verify_caret_prov_fallback_and_unresolved_page,
      verify_uniform_formatting_and_hyperlinks,
      verify_table_fold_grid_and_off_grid_cells,
      verify_out_of_grid_cell_offsets_do_not_write_the_grid,
      verify_huge_grid_keeps_cells_only,
      verify_sheet_rows_fold_into_the_sheet_table,
      verify_row_for_unknown_sheet_is_dropped,
      verify_hidden_sheet_maps_to_invisible_layer,
      verify_metadata_maps_name_language_and_fields,
      verify_header_footer_lands_in_furniture,
      verify_per_page_style,
      verify_take_moves_the_document_out,
      verify_integrity_errors_flag_broken_references,
      verify_integrity_errors_cover_the_form_arenas,
      verify_sheet_chart_pairs_with_its_embedded_object,
      verify_sheet_chart_without_object_folds_the_sheet_cells,
      verify_unplaced_chart_flushes_under_its_sheet_at_the_end,
      verify_slide_ole_shape_places_the_chart,
      verify_writer_chart_binds_on_arrival,
      verify_sheet_header_rows_are_marked,
      verify_inline_pictures_take_their_anchor_paragraphs_place,
      verify_unanchored_picture_still_trails_the_body,
      verify_slide_titles_after_the_first_are_section_headings,
  });
}
