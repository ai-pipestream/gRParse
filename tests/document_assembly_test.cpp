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

  // The unary document path carries the same items and references them.
  grparse::AssemblyCursor document_cursor;
  ai::docling::core::v1::DoclingDocument document;
  std::string plain_text;
  grparse::append_page_to_document(page, 1, &document_cursor, &document, &plain_text);
  require(document.tables_size() == 1 && document.pictures_size() == 1,
          "document must own the region items");
  require(document.body().children_size() == 5, "body references texts, table, and picture");
  require(document.body().children(3).ref() == "#/tables/0" &&
              document.body().children(4).ref() == "#/pictures/0",
          "region item refs must join the body graph");
  require(plain_text == "Heading\nbody text\ncell", "regions must not disturb the text stream");
}

}  // namespace

int main() {
  try {
    verify_contract_shape();
    verify_offsets_and_provenance();
    verify_layout_regions_map_labels_and_emit_items();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "document-assembly-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
