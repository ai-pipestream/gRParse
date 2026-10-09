#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/arena.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"
#include "../src/chunking/chunker.h"
#include "grparse/docling_map.h"
#include "grparse/document_assembly.h"
#include "grparse/document_repair.h"
#include "grparse/region_geometry.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::OcrLine line(std::string text, int top) {
  return grparse::OcrLine{std::move(text), {{10, top}, {90, top}, {90, top + 10}, {10, top + 10}},
                          0.875F};
}

grparse::OcrLine sized_line(std::string text, int top, int height) {
  return grparse::OcrLine{std::move(text),
                          {{10, top}, {90, top}, {90, top + height}, {10, top + height}},
                          0.875F};
}

// The base of whichever arm assembly chose for the item; the choice itself
// is asserted where it matters.
const ai::pipestream::document::v1::TextItemBase& item_base(
    const ai::pipestream::document::v1::BaseTextItem& item) {
  namespace docv1 = ai::pipestream::document::v1;
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return item.section_header().base();
    case docv1::BaseTextItem::kListItem: return item.list_item().base();
    case docv1::BaseTextItem::kFormula: return item.formula().base();
    case docv1::BaseTextItem::kText: return item.text().base();
    default: throw std::runtime_error("unexpected text item arm");
  }
}

void verify_contract_shape() {
  const auto* document = ai::pipestream::document::v1::Document::descriptor();
  require(document->FindFieldByName("texts")->number() == 7, "Document.texts field changed");
  require(document->FindFieldByName("pictures")->number() == 8, "Document.pictures field changed");
  require(document->FindFieldByName("tables")->number() == 9, "Document.tables field changed");
  require(document->FindFieldByName("pages")->number() == 12, "Document.pages field changed");

  const auto* page = ai::pipestream::parse::v1::PageData::descriptor();
  require(page->FindFieldByName("texts")->number() == 3, "PageData.texts field changed");
  require(page->FindFieldByName("text_offsets")->number() == 6, "PageData.text_offsets field changed");
  const auto* offset = ai::pipestream::parse::v1::TextOffset::descriptor();
  require(offset->FindFieldByName("confidence")->number() == 4, "TextOffset.confidence field changed");
  require(offset->FindFieldByName("source")->number() == 5, "TextOffset.source field changed");
}

void verify_offsets_and_provenance() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage first{100, 200, {line("h\xC3\xA9", 10), line("\xE4\xB8\x96\xE7\x95\x8C", 30)}};
  grparse::OcrPage second{100, 200, {line("x", 10)}};

  google::protobuf::Arena arena;
  auto* first_page = google::protobuf::Arena::Create<ai::pipestream::parse::v1::PageData>(&arena);
  grparse::append_page_data(first, 1, &cursor, first_page);

  require(first_page->texts_size() == 2, "first page text count");
  require(first_page->text_offsets_size() == 2, "first page offset count");
  require(first_page->texts(0).text().base().self_ref() == "#/texts/0", "first stable reference");
  require(first_page->texts(1).text().base().self_ref() == "#/texts/1", "second stable reference");
  require(first_page->texts(0).text().base().parent().ref() == "#/body", "stream parent reference");
  require(first_page->texts(0).text().base().prov(0).charspan().start() == 0, "local charspan start");
  require(first_page->texts(0).text().base().prov(0).charspan().end() == 2, "UTF charspan length");
  require(first_page->text_offsets(0).utf_start() == 0 && first_page->text_offsets(0).utf_end() == 2,
          "first running offset");
  require(first_page->text_offsets(1).utf_start() == 3 && first_page->text_offsets(1).utf_end() == 5,
          "second running offset includes separator");
  require(first_page->text_offsets(0).has_confidence() &&
              first_page->text_offsets(0).confidence() == 0.875F,
          "OCR confidence metadata");
  require(first_page->text_offsets(0).source() == ai::pipestream::parse::v1::TEXT_SOURCE_OCR,
          "OCR source metadata");

  auto* second_page = google::protobuf::Arena::Create<ai::pipestream::parse::v1::PageData>(&arena);
  grparse::append_page_data(second, 2, &cursor, second_page);
  require(second_page->texts(0).text().base().self_ref() == "#/texts/2", "cross-page stable reference");
  require(second_page->text_offsets(0).utf_start() == 6 && second_page->text_offsets(0).utf_end() == 7,
          "cross-page running offset");
}

void verify_whitespace_only_line_is_no_item() {
  // The recognizer read a stamp on court/4074526 p520 as a lone space; that
  // line is no text, so it takes no item and no index.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{100, 200, {line("before", 10), line(" ", 30), line("\t \n", 50),
                                   line("after", 70)}};
  google::protobuf::Arena arena;
  auto* data = google::protobuf::Arena::Create<ai::pipestream::parse::v1::PageData>(&arena);
  grparse::append_page_data(page, 1, &cursor, data);
  require(data->texts_size() == 2, "the blank lines leave no item");
  require(data->texts(0).text().base().text() == "before", "first text kept");
  require(data->texts(1).text().base().text() == "after", "last text kept");
  require(data->texts(1).text().base().self_ref() == "#/texts/1", "refs stay dense");
  require(data->text_offsets_size() == 2, "one offset per kept item");
}

void verify_layout_regions_map_labels_and_emit_items() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {line("Heading", 10), line("body text", 100), line("cell", 300)}};
  page.regions = {
      {"title", 0.9F, 0, 0, 1000, 40},
      {"table", 0.8F, 0, 250, 1000, 400},
      {"picture", 0.7F, 0, 500, 1000, 800},
  };

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 2, "layout page text count: the table carries its own line");
  require(data.texts(0).has_title() &&
              item_base(data.texts(0)).label() ==
                  ai::pipestream::document::v1::DOC_ITEM_LABEL_TITLE,
          "a line inside a title region becomes a TITLE item on the title arm");
  require(data.texts(1).text().base().label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_TEXT,
          "a line outside every region stays TEXT");
  require(data.tables_size() == 1 && data.tables(0).self_ref() == "#/tables/0" &&
              data.tables(0).label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_TABLE,
          "table region must become a TableItem");
  require(data.tables(0).prov_size() == 1 && data.tables(0).prov(0).page_no() == 1 &&
              data.tables(0).prov(0).bbox().t() == 250,
          "table item carries region provenance");
  require(data.pictures_size() == 1 && data.pictures(0).self_ref() == "#/pictures/0" &&
              data.pictures(0).label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_PICTURE,
          "figure region must become a PictureItem");
  require(!data.pictures(0).has_image(), "no captured bytes means no ImageRef");

  const auto& table_data = data.tables(0).data();
  require(table_data.num_rows() == 1 && table_data.num_cols() == 1,
          "single line in a table region yields a 1x1 geometry grid");
  require(table_data.table_cells_size() == 1 && table_data.table_cells(0).text() == "cell",
          "cell text comes from the bound line");
  require(table_data.grid_size() == 1 && table_data.grid(0).cells_size() == 1 &&
              table_data.grid(0).cells(0).text() == "cell",
          "row grid mirrors the flat cell list");
  const auto& cell = table_data.table_cells(0);
  require(cell.row_span() == 1 && cell.col_span() == 1 && cell.start_row_offset_idx() == 0 &&
              cell.end_row_offset_idx() == 1 && cell.start_col_offset_idx() == 0 &&
              cell.end_col_offset_idx() == 1,
          "geometry cells carry unit spans and grid offsets");
  require(cell.bbox().l() == 10 && cell.bbox().t() == 300 && cell.bbox().r() == 90 &&
              cell.bbox().b() == 310,
          "cell bbox is the bound line box");
  require(!cell.column_header() && !cell.row_header(),
          "geometry structure must not guess header roles");

  // The unary document path carries the same items and references them.
  grparse::AssemblyCursor document_cursor;
  ai::pipestream::document::v1::Document document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &document_cursor, &document, &plain_text);
  require(document.tables_size() == 1 && document.pictures_size() == 1,
          "document must own the region items");
  require(document.tables(0).data().num_rows() == 1 &&
              document.tables(0).data().table_cells(0).text() == "cell",
          "unary path carries the same table cells as the stream");
  require(document.body().children_size() == 4, "body references texts, table, and picture");
  require(document.body().children(0).ref() == "#/texts/0" &&
              document.body().children(1).ref() == "#/texts/1" &&
              document.body().children(2).ref() == "#/tables/0" &&
              document.body().children(3).ref() == "#/pictures/0",
          "floats join the body graph at their reading-order anchors");
  require(plain_text == "Heading\nbody text",
          "table interior text lives in the table, not the text stream");
}

// A region label outside every known vocabulary falls back to TEXT, but
// the raw spelling is kept on label_raw and the fallback is named in the
// warnings channel; a structural label (table/picture/text) keeping TEXT
// by design is neither a fallback nor a warning.
void verify_unknown_region_label_falls_back_loudly() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("mystery block", 10)}};
  page.regions = {{"aside", 0.9F, 0, 0, 1000, 40}};

  std::vector<std::string> warnings;
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data, &warnings);
  require(data.texts_size() == 1, "one region, one block");
  const auto& base = data.texts(0).text().base();
  require(base.label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_TEXT,
          "an unknown label lands on the TEXT catch-all");
  require(base.has_label_raw() && base.label_raw() == "aside",
          "the raw spelling rides label_raw, so version skew loses nothing");
  require(warnings.size() == 1 && warnings[0].contains("unknown region label") &&
              warnings[0].contains("'aside'"),
          "the fallback is named, once: "
          + (warnings.empty() ? std::string("no warning") : warnings[0]));

  grparse::OcrPage plain{1000, 1000, {line("inside", 10)}};
  plain.regions = {{"text", 0.9F, 0, 0, 1000, 40}};
  warnings.clear();
  ai::pipestream::parse::v1::PageData second;
  grparse::append_page_data(plain, 2, &cursor, &second, &warnings);
  require(warnings.empty() && !second.texts(0).text().base().has_label_raw(),
          "a structural label keeping TEXT by design is not a fallback");
}

// Model-structured cells override the geometry grid: spans and header flags
// come from the model, text binds by cell box, the row grid repeats spanning
// cells across covered positions and blank-fills unclaimed ones.
void verify_structured_cells_override_geometry() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("Region", 300), line("North", 360)}};
  grparse::LayoutRegion table{"table", 0.8F, 0, 250, 1000, 450};
  table.structured_cells = {
      {0, 0, 1, 2, true, 0, 290, 500, 340},
      {1, 0, 1, 1, false, 0, 350, 200, 400},
  };
  page.regions = {table};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  const auto& table_data = data.tables(0).data();
  require(table_data.num_rows() == 2 && table_data.num_cols() == 2,
          "grid extents come from spans, not cell count");
  require(table_data.table_cells_size() == 2, "flat list holds each cell once");
  const auto& header = table_data.table_cells(0);
  require(header.text() == "Region" && header.col_span() == 2 && header.column_header() &&
              header.start_col_offset_idx() == 0 && header.end_col_offset_idx() == 2,
          "header cell keeps its span and thead flag");
  require(header.bbox().t() == 290 && header.bbox().r() == 500,
          "structured cell bbox is the model box");
  require(table_data.table_cells(1).text() == "North" && !table_data.table_cells(1).column_header(),
          "body cell binds its line and stays unflagged");
  require(table_data.grid_size() == 2 && table_data.grid(0).cells_size() == 2,
          "grid stays rectangular");
  require(table_data.grid(0).cells(0).text() == "Region" &&
              table_data.grid(0).cells(1).text() == "Region",
          "spanning cells repeat across covered grid positions");
  require(table_data.grid(1).cells(1).text().empty() &&
              table_data.grid(1).cells(1).start_col_offset_idx() == 1,
          "unclaimed positions blank-fill with their own offsets");
}

// A figure region carrying captured PNG bytes becomes an ImageRef data URI
// whose pixel size is read from the PNG header itself.
void verify_captured_figure_bytes_become_image_refs() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("body", 100)}};
  // Minimal PNG prefix: 8-byte signature, IHDR length/tag, width 300, height 200.
  page.regions = {{"picture", 0.7F, 0, 500, 1000, 800,
                   {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D',
                    'R', 0, 0, 0x01, 0x2C, 0, 0, 0, 0xC8}}};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 1 && data.pictures(0).has_image(),
          "captured bytes must attach an ImageRef");
  const auto& image = data.pictures(0).image();
  require(image.mimetype() == "image/png", "image mimetype");
  require(image.size().width() == 300 && image.size().height() == 200,
          "image size comes from the PNG header");
  require(image.uri() == "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAASwAAADI",
          "image URI must be the base64 PNG data URI");
  require(data.pictures(0).annotations_size() == 0, "no classes means no annotations");

  grparse::AssemblyCursor classified_cursor;
  grparse::OcrPage classified{1000, 1000, {line("caption", 100)}};
  grparse::LayoutRegion figure{"picture", 0.7F, 0, 500, 1000, 800};
  figure.figure_classes = {{"bar_chart", 0.9F}, {"other", 0.1F}};
  classified.regions = {figure};
  ai::pipestream::parse::v1::PageData classified_data;
  grparse::append_page_data(classified, 1, &classified_cursor, &classified_data);
  const auto& annotation = classified_data.pictures(0).annotations(0).classification();
  require(annotation.kind() == "classification" &&
              annotation.provenance() == "figure-classifier",
          "classification annotation carries its provenance");
  require(annotation.predicted_classes_size() == 2 &&
              annotation.predicted_classes(0).class_name() == "bar_chart" &&
              annotation.predicted_classes(0).confidence() > 0.89,
          "predicted classes keep their order and confidence");
}

// A captured page preview becomes the page's own ImageRef; its pixel size
// comes from the PNG header while the page size keeps its coordinate space.
void verify_page_preview_becomes_page_image() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{612, 792, {line("body", 100)}};
  // Minimal PNG prefix: 8-byte signature, IHDR length/tag, width 300, height 200.
  page.preview_png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D,
                      'I',  'H', 'D', 'R', 0,    0,    0x01, 0x2C, 0, 0, 0, 0xC8};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.page_meta().has_image(), "the preview must attach to the page");
  const auto& image = data.page_meta().image();
  require(image.mimetype() == "image/png", "preview mimetype");
  require(image.size().width() == 300 && image.size().height() == 200,
          "preview pixel size comes from the PNG header");
  require(image.uri().starts_with("data:image/png;base64,"),
          "preview URI must be a base64 PNG data URI");
  require(data.page_meta().size().width() == 612 && data.page_meta().size().height() == 792,
          "the page size keeps the page's own coordinate space");

  grparse::AssemblyCursor bare_cursor;
  grparse::OcrPage bare{612, 792, {line("body", 100)}};
  ai::pipestream::parse::v1::PageData bare_data;
  grparse::append_page_data(bare, 1, &bare_cursor, &bare_data);
  require(!bare_data.page_meta().has_image(), "no preview, no page image");
}

// Decoded barcode payloads become misc annotations with a machine-readable
// struct, one annotation per payload, alongside any classification.
void verify_barcode_payloads_become_misc_annotations() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("caption", 100)}};
  grparse::LayoutRegion figure{"picture", 0.7F, 0, 500, 1000, 800};
  figure.figure_classes = {{"qr_code", 0.95F}};
  figure.barcodes = {{"QRCode", "https://example.com/a"}, {"Code128", "SKU-1234"}};
  page.regions = {figure};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 1, "figure region must emit a picture");
  const auto& picture = data.pictures(0);
  require(picture.annotations_size() == 5,
          "classification plus a typed arm and a legacy misc per payload");
  require(picture.annotations(0).has_classification(), "classification annotation stays first");
  const auto& typed = picture.annotations(1).barcode();
  require(picture.annotations(1).has_barcode() && typed.format() == "QRCode" &&
              typed.value() == "https://example.com/a" && typed.provenance() == "zxing-cpp",
          "the typed arm carries format, value, and provenance");
  const auto& first = picture.annotations(2).misc();
  require(first.kind() == "barcode", "misc annotation kind");
  const auto& fields = first.content().fields();
  require(fields.at("format").string_value() == "QRCode" &&
              fields.at("value").string_value() == "https://example.com/a" &&
              fields.at("provenance").string_value() == "zxing-cpp",
          "the legacy misc struct stays emitted alongside");
  require(picture.annotations(3).barcode().value() == "SKU-1234" &&
              picture.annotations(4).misc().content().fields().at("value").string_value() ==
                  "SKU-1234",
          "every payload gets both shapes");
  require(!picture.has_meta() ||
              picture.meta().custom_fields().count("pipestream__barcodes") == 0,
          "the wire carries barcodes typed only; exporters derive the "
          "dialect projection themselves");
  const auto& classified_meta = picture.meta().classification();
  require(classified_meta.predictions_size() == 1 &&
              classified_meta.predictions(0).class_name() == "qr_code" &&
              classified_meta.predictions(0).created_by() == "figure-classifier",
          "classification lands in meta as well as the wire annotation");
}

// The detector's full vocabulary has to survive assembly: every structural
// label reaches the Document as its own DocItemLabel instead of collapsing
// into plain text.
void verify_every_region_label_reaches_the_document() {
  namespace docv1 = ai::pipestream::document::v1;
  const struct {
    const char* region;
    docv1::DocItemLabel label;
  } kExpected[] = {
      {"caption", docv1::DOC_ITEM_LABEL_CAPTION},
      {"footnote", docv1::DOC_ITEM_LABEL_FOOTNOTE},
      {"formula", docv1::DOC_ITEM_LABEL_FORMULA},
      {"list_item", docv1::DOC_ITEM_LABEL_LIST_ITEM},
      {"page_footer", docv1::DOC_ITEM_LABEL_PAGE_FOOTER},
      {"page_header", docv1::DOC_ITEM_LABEL_PAGE_HEADER},
      {"section_header", docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"text", docv1::DOC_ITEM_LABEL_TEXT},
      {"title", docv1::DOC_ITEM_LABEL_TITLE},
      {"document_index", docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX},
      {"code", docv1::DOC_ITEM_LABEL_CODE},
      {"checkbox_selected", docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED},
      {"checkbox_unselected", docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED},
      {"form", docv1::DOC_ITEM_LABEL_FORM},
      {"key_value_region", docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION},
      {"list", docv1::DOC_ITEM_LABEL_LIST_ITEM},
  };
  for (const auto& expected : kExpected) {
    grparse::AssemblyCursor cursor;
    grparse::OcrPage page{1000, 1000, {line("labelled line", 100)}};
    page.regions = {{expected.region, 0.9F, 0, 90, 1000, 130}};
    ai::pipestream::parse::v1::PageData data;
    grparse::append_page_data(page, 1, &cursor, &data);
    require(data.texts_size() == 1, std::string("one item for a ") + expected.region + " region");
    if (expected.label == docv1::DOC_ITEM_LABEL_CODE) {
      // CodeItem keeps its fields inline instead of a nested base.
      require(data.texts(0).has_code() && data.texts(0).code().label() == expected.label,
              "a line inside a code region becomes a CodeItem");
      continue;
    }
    require(item_base(data.texts(0)).label() == expected.label,
            std::string("a line inside a ") + expected.region + " region takes that label");
  }
}

// Running headers and footers are furniture: they carry the furniture layer,
// parent the furniture group, and link there rather than into the body, so a
// renderer walking the body never picks up a page number.
void verify_headers_and_footers_are_furniture() {
  namespace docv1 = ai::pipestream::document::v1;
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {line("running header", 10), line("body text", 500),
                         line("page 3", 950)}};
  page.regions = {
      {"page_header", 0.9F, 0, 0, 1000, 40},
      {"text", 0.9F, 0, 480, 1000, 540},
      {"page_footer", 0.9F, 0, 930, 1000, 1000},
  };

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 3, "every line still becomes an item");
  const auto& header = data.texts(0).text().base();
  const auto& body = data.texts(1).text().base();
  const auto& footer = data.texts(2).text().base();
  require(header.content_layer() == docv1::CONTENT_LAYER_FURNITURE &&
              footer.content_layer() == docv1::CONTENT_LAYER_FURNITURE,
          "headers and footers land on the furniture layer");
  require(header.parent().ref() == "#/furniture" && footer.parent().ref() == "#/furniture",
          "furniture items parent the furniture group");
  require(body.content_layer() == docv1::CONTENT_LAYER_BODY && body.parent().ref() == "#/body",
          "body prose is untouched");

  docv1::Document document;
  grparse::AssemblyCursor document_cursor;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &document_cursor, &document, &plain_text);
  require(document.body().children_size() == 1 &&
              document.body().children(0).ref() == "#/texts/1",
          "only the body item links into the body");
  require(document.furniture().children_size() == 2 &&
              document.furniture().children(0).ref() == "#/texts/0" &&
              document.furniture().children(1).ref() == "#/texts/2",
          "both furniture items link into the furniture group");
}

// Every emitted item is attributable: texts, tables, and pictures carry a
// CollectorSource naming grparse and the engine that produced them, so
// additive merges with other collectors never collide silently.
void verify_items_carry_collector_sources() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("ocr line", 100), line("digital line", 200)}};
  page.lines[1].origin = grparse::TextOrigin::kDigitalPdf;
  grparse::LayoutRegion geometry_table{"table", 0.8F, 0, 250, 1000, 400};
  grparse::LayoutRegion structured_table{"table", 0.85F, 0, 420, 1000, 480};
  structured_table.structured_cells = {{0, 0, 1, 1, false, 0, 420, 1000, 480}};
  grparse::LayoutRegion figure{"picture", 0.7F, 0, 500, 1000, 800};
  page.regions = {geometry_table, structured_table, figure};
  page.layout_model = "layout-heron";

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);

  const auto& ocr_source = data.texts(0).text().base().source(0).collector();
  require(data.texts(0).text().base().source_size() == 1 &&
              ocr_source.collector() == "grparse" && ocr_source.model() == "rapidocr" &&
              ocr_source.has_confidence() && ocr_source.confidence() > 0.87,
          "OCR text names rapidocr with its line confidence");
  const auto& digital_source = data.texts(1).text().base().source(0).collector();
  require(digital_source.model() == "pdf-text",
          "digital text names the PDF text layer");
  require(data.tables(0).source(0).collector().model() == "geometry" &&
              data.tables(1).source(0).collector().model() == "slanet-plus",
          "tables name geometry or the structure model by cell origin");
  require(data.tables(1).source(0).collector().confidence() > 0.84,
          "table source carries the region confidence");
  require(data.pictures(0).source(0).collector().collector() == "grparse" &&
              data.pictures(0).source(0).collector().model() == "layout-heron",
          "pictures name the layout detector");
}

// Consecutive lines of one prose region merge into a single item whose
// provenance keeps every member line's box and charspan; the offset row
// spans the merged text and carries the weakest member confidence.
void verify_region_lines_merge_into_one_item() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("line one", 100), line("line two", 120)}};
  page.lines[1].confidence = 0.5F;
  page.regions = {{"text", 0.9F, 0, 90, 1000, 140}};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 1, "one region, one item");
  const auto& base = data.texts(0).text().base();
  require(base.text() == "line one line two", "members join with a space");
  require(base.prov_size() == 2, "one provenance entry per member line");
  require(base.prov(0).charspan().start() == 0 && base.prov(0).charspan().end() == 8,
          "first member charspan");
  require(base.prov(1).charspan().start() == 9 && base.prov(1).charspan().end() == 17,
          "second member charspan skips the separator");
  require(base.prov(0).bbox().t() == 100 && base.prov(1).bbox().t() == 120,
          "each member keeps its own box");
  require(data.text_offsets_size() == 1, "one offset row for the merged item");
  require(data.text_offsets(0).utf_start() == 0 && data.text_offsets(0).utf_end() == 17,
          "offset row spans the merged text");
  require(data.text_offsets(0).confidence() == 0.5F,
          "the row is as trustworthy as its shakiest member");
}

// A line whose center misses every recognized cell of a structured table is
// not carried by the table item and must stay ordinary body text.
void verify_unclaimed_table_line_stays_body_text() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("inside", 300), line("stray", 380)}};
  grparse::LayoutRegion table{"table", 0.8F, 0, 250, 1000, 450};
  // One recognized cell covering only the first line's center.
  table.structured_cells = {{0, 0, 1, 1, false, 0, 290, 1000, 340}};
  page.regions = {table};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 1 && data.texts(0).text().base().text() == "stray",
          "the unclaimed line survives as body text");
  require(data.tables(0).data().table_cells(0).text() == "inside",
          "the claimed line lives in its cell");
}

// A caption binds to the nearest float it visually labels and leaves the
// body graph; a caption with no float in reach stays free prose.
void verify_captions_attach_to_nearest_float() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 2000,
                        {line("Figure 1: shoreline", 820), line("Orphan caption", 1900)}};
  page.regions = {
      {"picture", 0.7F, 0, 500, 1000, 800},
      {"caption", 0.9F, 0, 810, 1000, 840},
      {"caption", 0.9F, 0, 1890, 1000, 1930},
  };

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 1 && data.texts_size() == 2, "one picture, two captions");
  const auto& attached = data.texts(0).text().base();
  require(attached.label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_CAPTION,
          "caption label survives attachment");
  require(attached.parent().ref() == "#/pictures/0", "the near caption re-parents to its float");
  require(data.pictures(0).captions_size() == 1 &&
              data.pictures(0).captions(0).ref() == attached.self_ref(),
          "the float claims the caption by reference");
  require(data.pictures(0).children_size() == 1 &&
              data.pictures(0).children(0).ref() == attached.self_ref(),
          "the float lists the caption it parents among its children");
  const auto& orphan = data.texts(1).text().base();
  require(orphan.parent().ref() == "#/body", "a caption with no float in reach stays body prose");

  grparse::AssemblyCursor document_cursor;
  ai::pipestream::document::v1::Document document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &document_cursor, &document, &plain_text);
  bool claimed_in_body = false;
  for (const auto& child : document.body().children()) {
    if (child.ref() == attached.self_ref()) claimed_in_body = true;
  }
  require(!claimed_in_body, "a claimed caption never doubles as a body child");
  require(plain_text == "Figure 1: shoreline\nOrphan caption",
          "captions keep their place in the text stream");
  require(grparse::docling_integrity_errors(document).empty(),
          "a claimed picture caption leaves the document well linked");
}

// A parent link is written on both sides: every caption a table or a
// picture claims names the float as its parent AND sits in the float's
// children list. A caption whose parent does not list it is the one-sided
// link the integrity check reports ("parent #/tables/0 does not list
// #/texts/1 as a child"), and the opt-in repair_referenced_orphans pass must
// find nothing to mend in a document this assembler produced.
void verify_claimed_captions_are_listed_by_their_float() {
  grparse::OcrPage page{1000, 2000,
                        {line("Table 1: yields", 280), line("Region", 320),
                         line("Figure 2: the site", 1420), line("Closing prose", 1700)}};
  page.regions = {
      {"caption", 0.9F, 0, 270, 1000, 300},
      {"table", 0.9F, 0, 310, 1000, 600},
      {"picture", 0.8F, 0, 900, 1000, 1400},
      {"caption", 0.9F, 0, 1410, 1000, 1440},
  };

  grparse::AssemblyCursor cursor;
  ai::pipestream::document::v1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &cursor, &document, &plain_text);
  require(document.tables_size() == 1 && document.pictures_size() == 1,
          "one table and one picture");

  const auto lists = [](const auto& refs, const std::string& ref) {
    for (const auto& entry : refs) {
      if (entry.ref() == ref) return true;
    }
    return false;
  };
  const auto& table = document.tables(0);
  const auto& picture = document.pictures(0);
  require(table.captions_size() == 1 && picture.captions_size() == 1,
          "each float claims the caption beside it");
  const std::string table_caption = table.captions(0).ref();
  const std::string picture_caption = picture.captions(0).ref();
  require(table_caption != picture_caption, "the two captions bind to different floats");
  for (const auto& text : document.texts()) {
    const auto& base = text.text().base();
    if (base.self_ref() == table_caption) {
      require(base.parent().ref() == table.self_ref(), "the table caption names the table");
    } else if (base.self_ref() == picture_caption) {
      require(base.parent().ref() == picture.self_ref(), "the picture caption names the picture");
    } else {
      require(base.parent().ref() == "#/body", "other prose stays on the body");
    }
  }
  require(lists(table.children(), table_caption),
          "the table lists its caption among its children");
  require(lists(picture.children(), picture_caption),
          "the picture lists its caption among its children");
  require(!lists(document.body().children(), table_caption) &&
              !lists(document.body().children(), picture_caption),
          "a claimed caption is not also a body child");

  const auto errors = grparse::docling_integrity_errors(document);
  std::string joined;
  for (const auto& error : errors) joined += error + "; ";
  require(errors.empty(), "the assembled page is well linked: " + joined);
  ai::pipestream::document::v1::Document repaired = document;
  require(grparse::repair_referenced_orphans(&repaired) == 0,
          "the orphan repair finds no one-sided caption link to mend");
}

// Heading depth comes from clustering heights across the document: the
// tallest cluster is level 1, each visibly smaller cluster one deeper, and
// levels a producer already set stay untouched.
void verify_section_header_levels() {
  namespace docv1 = ai::pipestream::document::v1;
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 2000,
                        {sized_line("Chapter", 100, 40), sized_line("Subsection", 300, 20),
                         sized_line("Another chapter", 600, 40), line("prose", 900)}};
  page.regions = {
      {"section_header", 0.9F, 0, 90, 1000, 150},
      {"section_header", 0.9F, 0, 290, 1000, 330},
      {"section_header", 0.9F, 0, 590, 1000, 650},
  };

  ai::pipestream::document::v1::Document document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &cursor, &document, &plain_text);
  // A producer-set level, as an office collector would emit it.
  auto* preset = document.add_texts()->mutable_section_header();
  preset->mutable_base()->set_self_ref("#/texts/9");
  preset->set_level(3);

  grparse::assign_section_header_levels(&document);
  require(document.texts(0).section_header().level() == 1, "tallest cluster is level 1");
  require(document.texts(1).section_header().level() == 2, "smaller heights go one deeper");
  require(document.texts(2).section_header().level() == 1, "equal heights share a level");
  require(document.texts(4).section_header().level() == 3, "producer-set levels stay untouched");
}

// Numbering in the heading text sets depth; a taller title block that
// opens the first page sits above the numbered sections.
void verify_numbered_section_header_levels() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 2000,
                        {sized_line("Report Title", 100, 60), sized_line("1 Scope", 400, 30),
                         sized_line("1.1 Purpose", 600, 30), sized_line("Overview", 800, 30),
                         line("prose", 900)}};
  page.regions = {
      {"section_header", 0.9F, 0, 90, 1000, 170},
      {"section_header", 0.9F, 0, 390, 1000, 440},
      {"section_header", 0.9F, 0, 590, 1000, 640},
      {"section_header", 0.9F, 0, 790, 1000, 840},
  };
  ai::pipestream::document::v1::Document document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &cursor, &document, &plain_text);
  grparse::assign_section_header_levels(&document);
  require(document.texts(0).item_case() == ai::pipestream::document::v1::BaseTextItem::kTitle,
          "the taller heading block opening page 1 becomes the title item");
  require(document.texts(1).section_header().level() == 1, "a depth-1 number is level 1");
  require(document.texts(2).section_header().level() == 2, "a depth-2 number is level 2");
  require(document.texts(3).section_header().level() == 1,
          "an unnumbered heading the size of the depth-1 cluster joins it");
}

// A rotated line keeps its exact quad in provenance; an axis-aligned line
// adds no polygon because the box already says everything.
void verify_rotated_lines_keep_their_quad() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000, {line("straight", 100)}};
  page.lines.push_back(grparse::OcrLine{
      "slanted", {{10, 200}, {90, 220}, {86, 250}, {6, 230}}, 0.9F});

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 2, "both lines emit");
  require(data.texts(0).text().base().prov(0).polygon_size() == 0,
          "axis-aligned lines carry no polygon");
  const auto& prov = data.texts(1).text().base().prov(0);
  require(prov.polygon_size() == 4, "rotated lines keep all four vertices");
  require(prov.polygon(0).x() == 10 && prov.polygon(0).y() == 200 &&
              prov.polygon(1).x() == 90 && prov.polygon(1).y() == 220,
          "vertices arrive in source order");
  require(prov.bbox().l() == 6 && prov.bbox().r() == 90 && prov.bbox().t() == 200 &&
              prov.bbox().b() == 250,
          "bbox stays the axis-aligned hull");
}

// PageData names the page's body order directly, and a text-less float on a
// two-column page anchors into its own column rather than the left one.
void verify_body_order_and_column_anchoring() {
  const auto column_line = [](std::string text, int left, int top) {
    return grparse::OcrLine{
        std::move(text),
        {{left, top}, {left + 80, top}, {left + 80, top + 10}, {left, top + 10}},
        0.9F};
  };
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {column_line("left one", 10, 100), column_line("left two", 10, 400),
                         column_line("right one", 510, 100),
                         column_line("right two", 510, 400)}};
  // A picture with no interior text, sitting in the right column between
  // that column's two lines.
  page.regions = {{"picture", 0.8F, 500, 200, 1000, 350}};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  std::vector<std::string> order;
  for (const auto& ref : data.body_order()) order.push_back(ref.ref());
  require(order.size() == 5, "body order names every body item");
  require(order[0] == "#/texts/0" && order[1] == "#/texts/1",
          "the left column reads first");
  require(order[2] == "#/texts/2" && order[3] == "#/pictures/0" &&
              order[4] == "#/texts/3",
          "the float lands between its own column's lines");
}

// A text-less float with nothing beside it (a corner logo above a title that
// never reaches its column) heads the page's body order instead of closing
// it, and a signature image between a line above and a dated line below
// reads between them.
void verify_lonely_floats_anchor_by_their_top_edge() {
  const auto wide_line = [](std::string text, int left, int top, int width, int height) {
    return grparse::OcrLine{std::move(text),
                            {{left, top}, {left + width, top}, {left + width, top + height},
                             {left, top + height}},
                            0.9F};
  };
  grparse::AssemblyCursor cursor;
  grparse::OcrPage slide{1000, 1500,
                         {wide_line("EU Network Codes", 60, 300, 500, 40),
                          wide_line("Mike Thorne", 60, 650, 300, 20)}};
  slide.regions = {{"picture", 0.8F, 760, 60, 960, 180}};
  ai::pipestream::parse::v1::PageData slide_data;
  grparse::append_page_data(slide, 1, &cursor, &slide_data);
  std::vector<std::string> order;
  for (const auto& ref : slide_data.body_order()) order.push_back(ref.ref());
  require(order == std::vector<std::string>{"#/pictures/0", "#/texts/0", "#/texts/1"},
          "a corner logo above the title reads first");

  grparse::OcrPage signed_page{1000, 1500,
                               {wide_line("SO ORDERED.", 90, 700, 150, 20),
                                wide_line("DATE: August 5, 2016", 60, 1200, 300, 20)}};
  signed_page.regions = {{"picture", 0.8F, 450, 800, 780, 950}};
  ai::pipestream::parse::v1::PageData signed_data;
  grparse::append_page_data(signed_page, 2, &cursor, &signed_data);
  order.clear();
  for (const auto& ref : signed_data.body_order()) order.push_back(ref.ref());
  require(order == std::vector<std::string>{"#/texts/2", "#/pictures/1", "#/texts/3"},
          "a signature image reads between the order and the date");
}

}  // namespace

// A turn orientation recovery applied reaches the page's typed quality
// slot; a page read as it came carries no quality block for it.
void verify_recovered_rotation_reaches_page_quality() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage turned{200, 100, {line("upright now", 10)}};
  turned.rotation_degrees = 270;
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(turned, 1, &cursor, &data);
  require(data.page_meta().has_quality() && data.page_meta().quality().has_rotation_degrees(),
          "a recovered turn must land in PageItem.quality");
  require(data.page_meta().quality().rotation_degrees() == 270.0, "the turn is the degrees kept");
  require(data.page_meta().size().width() == 200 && data.page_meta().size().height() == 100,
          "the page size is the upright raster's");

  grparse::AssemblyCursor plain_cursor;
  grparse::OcrPage plain{100, 200, {line("as fed", 10)}};
  ai::pipestream::parse::v1::PageData plain_data;
  grparse::append_page_data(plain, 1, &plain_cursor, &plain_data);
  require(!plain_data.page_meta().has_quality() ||
              !plain_data.page_meta().quality().has_rotation_degrees(),
          "an unturned page claims no rotation");
}

// Consecutive body list items become one LIST group, in body order and
// across what were page boundaries; a single item gets its own group, and
// everything else stays where it was.
void verify_list_items_join_a_list_group() {
  namespace docv1 = ai::pipestream::document::v1;
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto add = [&document](bool list_item) {
    const std::string ref = "#/texts/" + std::to_string(document.texts_size());
    auto* text = document.add_texts();
    docv1::TextItemBase* base = list_item ? text->mutable_list_item()->mutable_base()
                                          : text->mutable_text()->mutable_base();
    base->set_self_ref(ref);
    base->mutable_parent()->set_ref("#/body");
    base->set_label(list_item ? docv1::DOC_ITEM_LABEL_LIST_ITEM : docv1::DOC_ITEM_LABEL_TEXT);
    document.mutable_body()->add_children()->set_ref(ref);
  };
  for (const bool list_item : {false, true, true, false, true}) add(list_item);
  require(grparse::group_list_items(&document) == 2, "two runs make two groups");
  const auto& body = document.body().children();
  require(body.size() == 4 && body[0].ref() == "#/texts/0" && body[1].ref() == "#/groups/0" &&
              body[2].ref() == "#/texts/3" && body[3].ref() == "#/groups/1",
          "each run is replaced in the body by its group");
  const auto& first = document.groups(0);
  require(first.label() == docv1::GROUP_LABEL_LIST && first.parent().ref() == "#/body" &&
              first.children_size() == 2 && first.children(0).ref() == "#/texts/1" &&
              first.children(1).ref() == "#/texts/2",
          "the group holds its run in order");
  require(document.texts(1).list_item().base().parent().ref() == "#/groups/0" &&
              document.texts(4).list_item().base().parent().ref() == "#/groups/1",
          "the items name their group as parent");
  require(document.texts(0).text().base().parent().ref() == "#/body",
          "other items keep their parent");
  require(grparse::group_list_items(&document) == 0, "a grouped document is left as it is");
}

// The table the parse derives from a finished document is the one the CV
// path records while it assembles pages: same items, same spans, furniture
// included, across page boundaries. The CV rows only add how each item was
// read.
void verify_derived_offsets_match_the_assembled_rows() {
  grparse::OcrPage first{1000, 1000,
                         {line("running header", 10), line("café body", 500),
                          line("page 1", 950)}};
  first.regions = {
      {"page_header", 0.9F, 0, 0, 1000, 40},
      {"text", 0.9F, 0, 480, 1000, 540},
      {"page_footer", 0.9F, 0, 930, 1000, 1000},
  };
  grparse::OcrPage second{1000, 1000, {line("second page", 100), line("ends here", 300)}};

  ai::pipestream::document::v1::Document document;
  grparse::AssemblyCursor cursor;
  std::string plain_text;
  google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::TextOffset> rows;
  grparse::append_page_to_document(first, 1, &cursor, &document, &plain_text, &rows);
  grparse::append_page_to_document(second, 2, &cursor, &document, &plain_text, &rows);
  require(rows.size() == document.texts_size() && rows.size() == 5,
          "the CV path records one row per text item");

  auto derived = grparse::chunking::derive_offsets(document);
  require(static_cast<int>(derived.size()) == rows.size(), "one derived row per text item");
  for (const auto& row : rows) {
    const auto entry = derived.find(row.self_ref());
    require(entry != derived.end() && entry->second.start == row.utf_start() &&
                entry->second.end == row.utf_end(),
            "the derived span equals the assembled span for " + row.self_ref());
  }
  grparse::chunking::overlay_sources(rows, &derived);
  for (const auto& row : rows) {
    require(derived.at(row.self_ref()).source == row.source(),
            "every assembled row lends its source to the derived table");
  }
}

// The scanned general-ledger pages of a court filing, assembled the way
// the scheduler hands them over: detections settled by
// resolve_region_overlaps first. Each page reads top to bottom, its table
// first and the lines no cell claims after it in the order they sit.
grparse::OcrLine boxed_line(std::string text, int left, int top, int right, int bottom) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                          0.9F};
}

std::vector<std::string> body_refs(const ai::pipestream::parse::v1::PageData& data) {
  std::vector<std::string> order;
  for (const auto& ref : data.body_order()) order.push_back(ref.ref());
  return order;
}

// p772: the detector drew a text box around one End Bal amount, scoring it
// above the table. The amount belongs to its cell, not after the table's
// stray ".00" from a lower row.
void verify_ledger_amount_in_a_nested_box_lands_in_its_cell() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{2200, 1701,
                        {boxed_line("Date", 1290, 210, 1360, 240),
                         boxed_line("Total for", 1290, 1330, 1400, 1360),
                         boxed_line("-8,020.00", 1812, 1319, 1900, 1346),
                         boxed_line(".00", 1733, 1494, 1775, 1530)}};
  grparse::LayoutRegion table{"table", 0.510F, 93, 200, 1924, 1527};
  table.structured_cells = {{0, 0, 1, 1, true, 1280, 200, 1400, 250},
                            {1, 0, 1, 1, false, 1280, 1311, 1400, 1365},
                            {1, 1, 1, 1, false, 1788, 1311, 1918, 1355}};
  page.regions = {{"text", 0.551F, 1817, 1322, 1896, 1342}, table};
  grparse::resolve_region_overlaps(page.regions, page.lines, page.width, page.height);

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 1 && data.texts(0).text().base().text() == ".00",
          "only the line no cell claims stays body text");
  require(data.tables(0).data().table_cells(2).text() == "-8,020.00",
          "the amount lives in its End Bal cell");
  require(body_refs(data) == std::vector<std::string>{"#/tables/0", "#/texts/0"},
          "the table reads before its stray line");
}

// p862: the table detected twice, one box inside the other, and text boxes
// around a GL number inside it. One table remains, and its unclaimed lines
// follow it top to bottom.
void verify_ledger_detected_twice_reads_as_one_table() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{2200, 1701,
                        {boxed_line("End Bal", 1792, 223, 1875, 254),
                         boxed_line("Lender Credits", 400, 310, 700, 340),
                         boxed_line("42400-1521 80", 124, 1276, 253, 1305),
                         boxed_line("10/06/14", 1281, 1513, 1362, 1541)}};
  grparse::LayoutRegion inner{"table", 0.564F, 313, 215, 1880, 1130};
  inner.structured_cells = {{0, 0, 1, 1, false, 380, 300, 720, 350}};
  grparse::LayoutRegion outer{"table", 0.554F, 76, 221, 1909, 1543};
  outer.structured_cells = {{0, 0, 1, 1, false, 380, 300, 720, 350}};
  page.regions = {{"text", 0.574F, 130, 1282, 249, 1300}, inner, outer,
                  {"page_header", 0.521F, 2024, 467, 2055, 1285}};
  grparse::resolve_region_overlaps(page.regions, page.lines, page.width, page.height);

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.tables_size() == 1 && data.tables(0).prov(0).bbox().l() == 76,
          "one table remains, the larger box");
  require(data.tables(0).data().table_cells(0).text() == "Lender Credits",
          "the kept table carries the shared cell");
  std::vector<std::string> texts;
  for (const auto& item : data.texts()) texts.push_back(item.text().base().text());
  require(texts == std::vector<std::string>{"End Bal", "42400-1521 80", "10/06/14"},
          "the unclaimed lines read top to bottom");
  require(body_refs(data) ==
              std::vector<std::string>{"#/tables/0", "#/texts/0", "#/texts/1", "#/texts/2"},
          "the table heads its lines");
}

// p848: the detector boxed the whole scanned ledger as one picture. The
// picture is the page; its title and text read as the page's prose.
void verify_page_sized_picture_over_a_ledger_is_dropped() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{2200, 1701,
                        {boxed_line("General Ledger by Branch", 801, 209, 1322, 260),
                         boxed_line("Company: Ameripro Funding, Inc.", 812, 253, 1315, 297),
                         boxed_line("41100-152180", 120, 520, 260, 545),
                         boxed_line("Origination Fees", 320, 520, 500, 545)}};
  page.regions = {{"section_header", 0.602F, 805, 208, 1317, 250},
                  {"picture", 0.573F, 0, 3, 2200, 1701},
                  {"text", 0.507F, 94, 260, 1896, 1499}};
  grparse::resolve_region_overlaps(page.regions, page.lines, page.width, page.height);

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 0, "the page-sized picture is gone");
  require(body_refs(data) == std::vector<std::string>{"#/texts/0", "#/texts/1"},
          "the title reads before the ledger text");
  require(data.texts(0).has_section_header() &&
              data.texts(1).text().base().text().starts_with("Company:"),
          "the title and the ledger text keep their regions");
}

int main() {
  return grparse_test::run_test_main("document-assembly-test", {
      verify_contract_shape,
      verify_offsets_and_provenance,
      verify_whitespace_only_line_is_no_item,
      verify_layout_regions_map_labels_and_emit_items,
      verify_unknown_region_label_falls_back_loudly,
      verify_every_region_label_reaches_the_document,
      verify_headers_and_footers_are_furniture,
      verify_structured_cells_override_geometry,
      verify_captured_figure_bytes_become_image_refs,
      verify_page_preview_becomes_page_image,
      verify_barcode_payloads_become_misc_annotations,
      verify_items_carry_collector_sources,
      verify_region_lines_merge_into_one_item,
      verify_unclaimed_table_line_stays_body_text,
      verify_captions_attach_to_nearest_float,
      verify_claimed_captions_are_listed_by_their_float,
      verify_section_header_levels,
      verify_numbered_section_header_levels,
      verify_rotated_lines_keep_their_quad,
      verify_body_order_and_column_anchoring,
      verify_lonely_floats_anchor_by_their_top_edge,
      verify_recovered_rotation_reaches_page_quality,
      verify_list_items_join_a_list_group,
      verify_derived_offsets_match_the_assembled_rows,
      verify_ledger_amount_in_a_nested_box_lands_in_its_cell,
      verify_ledger_detected_twice_reads_as_one_table,
      verify_page_sized_picture_over_a_ledger_is_dropped,
  });
}
