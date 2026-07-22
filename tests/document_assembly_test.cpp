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

}  // namespace

int main() {
  try {
    verify_contract_shape();
    verify_offsets_and_provenance();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "document-assembly-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
