#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <google/protobuf/arena.h>

#include "ai/docling/core/v1/docling_document.pb.h"
#include "ai/docling/serve/v1/docling_serve_stream.pb.h"
#include "grparse/document_assembly.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::OcrLine line(std::string text, int top) {
  return grparse::OcrLine{std::move(text), {{10, top}, {90, top}, {90, top + 10}, {10, top + 10}},
                          0.875F};
}

void verify_contract_shape() {
  const auto* document = ai::docling::core::v1::DoclingDocument::descriptor();
  require(document->FindFieldByName("texts")->number() == 7, "DoclingDocument.texts field changed");
  require(document->FindFieldByName("pictures")->number() == 8, "DoclingDocument.pictures field changed");
  require(document->FindFieldByName("tables")->number() == 9, "DoclingDocument.tables field changed");
  require(document->FindFieldByName("pages")->number() == 12, "DoclingDocument.pages field changed");

  const auto* page = ai::docling::serve::v1::PageData::descriptor();
  require(page->FindFieldByName("texts")->number() == 3, "PageData.texts field changed");
  require(page->FindFieldByName("text_offsets")->number() == 6, "PageData.text_offsets field changed");
  const auto* offset = ai::docling::serve::v1::TextOffset::descriptor();
  require(offset->FindFieldByName("confidence")->number() == 4, "TextOffset.confidence field changed");
  require(offset->FindFieldByName("source")->number() == 5, "TextOffset.source field changed");
}

void verify_offsets_and_provenance() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage first{100, 200, {line("h\xC3\xA9", 10), line("\xE4\xB8\x96\xE7\x95\x8C", 30)}};
  grparse::OcrPage second{100, 200, {line("x", 10)}};

  google::protobuf::Arena arena;
  auto* first_page = google::protobuf::Arena::Create<ai::docling::serve::v1::PageData>(&arena);
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
  require(first_page->text_offsets(0).source() == ai::docling::serve::v1::TEXT_SOURCE_OCR,
          "OCR source metadata");

  auto* second_page = google::protobuf::Arena::Create<ai::docling::serve::v1::PageData>(&arena);
  grparse::append_page_data(second, 2, &cursor, second_page);
  require(second_page->texts(0).text().base().self_ref() == "#/texts/2", "cross-page stable reference");
  require(second_page->text_offsets(0).utf_start() == 6 && second_page->text_offsets(0).utf_end() == 7,
          "cross-page running offset");
}

void verify_layout_regions_map_labels_and_emit_items() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {line("Heading", 10), line("body text", 100), line("cell", 300)}};
  page.regions = {
      {"title", 0.9F, 0, 0, 1000, 40},
      {"table", 0.8F, 0, 250, 1000, 400},
      {"figure", 0.7F, 0, 500, 1000, 800},
  };

  ai::docling::serve::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.texts_size() == 3, "layout page text count");
  require(data.texts(0).text().base().label() == ai::docling::core::v1::DOC_ITEM_LABEL_TITLE,
          "a line inside a title region becomes a TITLE item");
  require(data.texts(1).text().base().label() == ai::docling::core::v1::DOC_ITEM_LABEL_TEXT,
          "a line outside every region stays TEXT");
  require(data.texts(2).text().base().label() == ai::docling::core::v1::DOC_ITEM_LABEL_TEXT,
          "table cell text stays TEXT until Epic D structures it");
  require(data.tables_size() == 1 && data.tables(0).self_ref() == "#/tables/0" &&
              data.tables(0).label() == ai::docling::core::v1::DOC_ITEM_LABEL_TABLE,
          "table region must become a TableItem");
  require(data.tables(0).prov_size() == 1 && data.tables(0).prov(0).page_no() == 1 &&
              data.tables(0).prov(0).bbox().t() == 250,
          "table item carries region provenance");
  require(data.pictures_size() == 1 && data.pictures(0).self_ref() == "#/pictures/0" &&
              data.pictures(0).label() == ai::docling::core::v1::DOC_ITEM_LABEL_PICTURE,
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
  ai::docling::core::v1::DoclingDocument document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &document_cursor, &document, &plain_text);
  require(document.tables_size() == 1 && document.pictures_size() == 1,
          "document must own the region items");
  require(document.tables(0).data().num_rows() == 1 &&
              document.tables(0).data().table_cells(0).text() == "cell",
          "unary path carries the same table cells as the stream");
  require(document.body().children_size() == 5, "body references texts, table, and picture");
  require(document.body().children(3).ref() == "#/tables/0" &&
              document.body().children(4).ref() == "#/pictures/0",
          "region item refs must join the body graph");
  require(plain_text == "Heading\nbody text\ncell", "regions must not disturb the text stream");
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

  ai::docling::serve::v1::PageData data;
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
  page.regions = {{"figure", 0.7F, 0, 500, 1000, 800,
                   {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D',
                    'R', 0, 0, 0x01, 0x2C, 0, 0, 0, 0xC8}}};

  ai::docling::serve::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 1 && data.pictures(0).has_image(),
          "captured bytes must attach an ImageRef");
  const auto& image = data.pictures(0).image();
  require(image.mimetype() == "image/png", "image mimetype");
  require(image.size().width() == 300 && image.size().height() == 200,
          "image size comes from the PNG header");
  require(image.uri() == "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAASwAAADI",
          "image URI must be the base64 PNG data URI");
}

}  // namespace

int main() {
  try {
    verify_contract_shape();
    verify_offsets_and_provenance();
    verify_layout_regions_map_labels_and_emit_items();
    verify_structured_cells_override_geometry();
    verify_captured_figure_bytes_become_image_refs();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "document-assembly-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
