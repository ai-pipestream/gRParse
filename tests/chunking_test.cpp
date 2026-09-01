#include <algorithm>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "../src/chunking/chunker.h"
#include "../src/chunking/sentence_rules.h"
#include "../src/chunking/token_counter.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse::chunking::ChunkOptions;
using grparse::chunking::chunk_hierarchical;
using grparse::chunking::chunk_hybrid;
using grparse::chunking::count_tokens;
using grparse::chunking::hybrid_rules_digest;
using grparse::chunking::OffsetEntry;
using grparse::chunking::OffsetTable;
using grparse::chunking::validate_hybrid_options;

namespace {

using grparse_test::require;

void require_eq(const std::string& actual, const std::string& expected,
                const std::string& message) {
  if (actual != expected) {
    throw std::runtime_error(message + "\nexpected: [" + expected + "]\nactual:   [" +
                             actual + "]");
  }
}

void require_eq(int actual, int expected, const std::string& message) {
  if (actual != expected) {
    throw std::runtime_error(message + "\nexpected: " + std::to_string(expected) +
                             "\nactual:   " + std::to_string(actual));
  }
}

// -- document builders ------------------------------------------------------

docv1::Document new_document() {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  return document;
}

std::string next_text_ref(const docv1::Document& document) {
  return "#/texts/" + std::to_string(document.texts_size());
}

void fill_base(docv1::TextItemBase* base, const std::string& ref, const std::string& text,
               int page) {
  base->set_self_ref(ref);
  base->set_text(text);
  base->set_orig(text);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->mutable_parent()->set_ref("#/body");
  if (page > 0) base->add_prov()->set_page_no(page);
}

std::string add_paragraph(docv1::Document* document, const std::string& text, int page = 0) {
  const std::string ref = next_text_ref(*document);
  fill_base(document->add_texts()->mutable_text()->mutable_base(), ref, text, page);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

void set_confidence(docv1::Document* document, const std::string& ref, double confidence) {
  const int index = std::stoi(ref.substr(std::string("#/texts/").size()));
  auto* item = document->mutable_texts(index);
  docv1::TextItemBase* base = item->has_list_item()
                                  ? item->mutable_list_item()->mutable_base()
                                  : item->mutable_text()->mutable_base();
  auto* collector = base->add_source()->mutable_collector();
  collector->set_collector("grparse");
  collector->set_confidence(confidence);
}

std::string add_title(docv1::Document* document, const std::string& text) {
  const std::string ref = next_text_ref(*document);
  fill_base(document->add_texts()->mutable_title()->mutable_base(), ref, text, 0);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_section(docv1::Document* document, const std::string& text, int level) {
  const std::string ref = next_text_ref(*document);
  auto* header = document->add_texts()->mutable_section_header();
  header->set_level(level);
  fill_base(header->mutable_base(), ref, text, 0);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

// A list group plus its items. The items are the group's children only, never
// the body's, which is the shape a list arrives in.
std::string add_list(docv1::Document* document, bool ordered,
                     const std::vector<std::string>& items, int page = 0,
                     std::vector<std::string>* item_refs = nullptr) {
  const std::string group_ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(group_ref);
  group->set_label(ordered ? docv1::GROUP_LABEL_ORDERED_LIST : docv1::GROUP_LABEL_LIST);
  group->mutable_parent()->set_ref("#/body");
  document->mutable_body()->add_children()->set_ref(group_ref);
  for (const auto& text : items) {
    const std::string ref = next_text_ref(*document);
    auto* item = document->add_texts()->mutable_list_item();
    item->set_enumerated(ordered);
    fill_base(item->mutable_base(), ref, text, page);
    item->mutable_base()->mutable_parent()->set_ref(group_ref);
    item->mutable_base()->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
    group->add_children()->set_ref(ref);
    if (item_refs != nullptr) item_refs->push_back(ref);
  }
  return group_ref;
}

struct Cell {
  std::string text;
  bool column_header = false;
  bool row_header = false;
};

std::string add_table(docv1::Document* document, const std::vector<std::vector<Cell>>& rows,
                      const std::vector<std::string>& caption_refs = {}, int page = 0) {
  const std::string ref = "#/tables/" + std::to_string(document->tables_size());
  auto* table = document->add_tables();
  table->set_self_ref(ref);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->mutable_parent()->set_ref("#/body");
  if (page > 0) table->add_prov()->set_page_no(page);
  auto* data = table->mutable_data();
  data->set_num_rows(static_cast<int>(rows.size()));
  data->set_num_cols(rows.empty() ? 0 : static_cast<int>(rows.front().size()));
  for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
    auto* grid_row = data->add_grid();
    for (int column = 0; column < static_cast<int>(rows[row].size()); ++column) {
      auto* cell = grid_row->add_cells();
      cell->set_text(rows[row][column].text);
      cell->set_column_header(rows[row][column].column_header);
      cell->set_row_header(rows[row][column].row_header);
      cell->set_start_row_offset_idx(row);
      cell->set_end_row_offset_idx(row + 1);
      cell->set_start_col_offset_idx(column);
      cell->set_end_col_offset_idx(column + 1);
    }
  }
  for (const auto& caption : caption_refs) table->add_captions()->set_ref(caption);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_picture(docv1::Document* document,
                        const std::vector<std::string>& caption_refs, int page = 0) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->mutable_parent()->set_ref("#/body");
  if (page > 0) picture->add_prov()->set_page_no(page);
  for (const auto& caption : caption_refs) picture->add_captions()->set_ref(caption);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

// Offsets for every text item in the document, laid out the way the parse
// lays them out: item texts concatenated with one separator code point
// between them.
OffsetTable offsets_for(const docv1::Document& document,
                        parsev1::TextSource source = parsev1::TEXT_SOURCE_DIGITAL_PDF) {
  OffsetTable table;
  std::uint64_t cursor = 0;
  for (const auto& item : document.texts()) {
    const auto* base = item.has_title()          ? &item.title().base()
                       : item.has_section_header() ? &item.section_header().base()
                       : item.has_list_item()      ? &item.list_item().base()
                       : item.has_text()           ? &item.text().base()
                                                   : nullptr;
    if (base == nullptr) continue;
    const std::uint64_t length =
        grparse::chunking::codepoint_length(base->text());
    table[base->self_ref()] = OffsetEntry{cursor, cursor + length, source};
    cursor += length + 1;
  }
  return table;
}

const parsev1::Chunk* find_chunk(const std::vector<parsev1::Chunk>& chunks,
                                 const std::string& needle) {
  for (const auto& chunk : chunks) {
    if (chunk.text().contains(needle)) return &chunk;
  }
  return nullptr;
}

std::vector<std::string> headings_of(const parsev1::Chunk& chunk) {
  return {chunk.headings().begin(), chunk.headings().end()};
}

// Deterministic protobuf bytes: the metadata map has no intrinsic field
// order, so byte comparisons must ask for the canonical one.
std::string serialized(const std::vector<parsev1::Chunk>& chunks) {
  parsev1::ChunkDocumentResponse response;
  for (const auto& chunk : chunks) *response.add_chunks() = chunk;
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream sink(&bytes);
    google::protobuf::io::CodedOutputStream stream(&sink);
    stream.SetSerializationDeterministic(true);
    if (!response.SerializeToCodedStream(&stream)) {
      throw std::runtime_error("chunk list serialization failed");
    }
  }
  return bytes;
}

// -- the fixture ------------------------------------------------------------

// A document with every shape the walk has a rule for: a title, two heading
// levels that shadow one another, paragraphs, a list group, a captioned
// table, and a captioned picture.
docv1::Document field_guide() {
  docv1::Document document = new_document();
  add_title(&document, "Field Guide");
  add_section(&document, "Birds", 1);
  const std::string sparrows =
      add_paragraph(&document, "Sparrows are small. They eat seeds.", 1);
  set_confidence(&document, sparrows, 0.94);
  std::vector<std::string> list_items;
  add_list(&document, false, {"nests", "songs"}, 1, &list_items);
  set_confidence(&document, list_items.at(0), 0.81);
  set_confidence(&document, list_items.at(1), 0.62);
  add_section(&document, "Nests", 2);
  add_paragraph(&document, "Twigs and grass.", 2);
  add_section(&document, "Mammals", 1);
  const std::string table_caption = add_paragraph(&document, "Table 1. Body sizes.", 2);
  add_table(&document,
            {{{"", true, false}, {"Weight", true, false}, {"Height", true, false}},
             {{"Sparrow", false, true}, {"30 g", false, false}, {"15 cm", false, false}},
             {{"Crow", false, true}, {"500 g", false, false}, {"50 cm", false, false}}},
            {table_caption}, 2);
  const std::string picture_caption = add_paragraph(&document, "Figure 1. A crow.", 3);
  add_picture(&document, {picture_caption}, 3);
  return document;
}

// -- determinism ------------------------------------------------------------

void verify_chunking_is_byte_identical_across_runs_and_threads() {
  const docv1::Document document = field_guide();
  const OffsetTable offsets = offsets_for(document);
  const ChunkOptions options{false, true};
  const std::string reference =
      serialized(chunk_hierarchical(document, offsets, options, "guide.pdf"));
  require(!reference.empty(), "the fixture must chunk to something");
  for (int run = 0; run < 3; ++run) {
    require_eq(serialized(chunk_hierarchical(document, offsets, options, "guide.pdf")),
               reference, "hierarchical chunking is not reproducible across runs");
  }

  parsev1::HybridChunkerOptions hybrid;
  hybrid.set_max_tokens(12);
  hybrid.set_include_raw_text(true);
  const std::string hybrid_reference =
      serialized(chunk_hybrid(document, offsets, hybrid, "guide.pdf"));

  std::vector<std::string> hierarchical_results(4);
  std::vector<std::string> hybrid_results(4);
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      hierarchical_results[static_cast<std::size_t>(worker)] =
          serialized(chunk_hierarchical(document, offsets, options, "guide.pdf"));
      hybrid_results[static_cast<std::size_t>(worker)] =
          serialized(chunk_hybrid(document, offsets, hybrid, "guide.pdf"));
    });
  }
  for (auto& worker : workers) worker.join();
  for (int worker = 0; worker < 4; ++worker) {
    require_eq(hierarchical_results[static_cast<std::size_t>(worker)], reference,
               "hierarchical chunking is not reproducible across threads");
    require_eq(hybrid_results[static_cast<std::size_t>(worker)], hybrid_reference,
               "hybrid chunking is not reproducible across threads");
  }
}

// -- hierarchical shape -----------------------------------------------------

void verify_heading_trail_shadows_and_pops() {
  const docv1::Document document = field_guide();
  const auto chunks = chunk_hierarchical(document, {}, {}, "guide.pdf");
  const auto* sparrows = find_chunk(chunks, "Sparrows are small");
  require(sparrows != nullptr, "the paragraph must chunk");
  require(headings_of(*sparrows) == std::vector<std::string>({"Field Guide", "Birds"}),
          "a paragraph carries the trail in force where it was emitted");

  const auto* twigs = find_chunk(chunks, "Twigs and grass");
  require(twigs != nullptr, "the nested-section paragraph must chunk");
  require(headings_of(*twigs) ==
              std::vector<std::string>({"Field Guide", "Birds", "Nests"}),
          "a deeper heading extends the trail");

  const auto* table = find_chunk(chunks, "Sparrow, Weight");
  require(table != nullptr, "the table must chunk");
  require(headings_of(*table) == std::vector<std::string>({"Field Guide", "Mammals"}),
          "a heading pops every recorded level at or below its own");

  for (const auto& chunk : chunks) {
    require(chunk.text() != "Birds" && chunk.text() != "Field Guide",
            "headings never emit chunks of their own");
    require_eq(chunk.rules_digest(), "grparse-hier/1",
               "every hierarchical chunk carries the hierarchical digest");
  }
  for (int index = 0; index < static_cast<int>(chunks.size()); ++index) {
    require_eq(chunks[static_cast<std::size_t>(index)].chunk_index(), index,
               "chunk_index is the emission ordinal");
  }
}

void verify_list_group_consumes_its_items() {
  const docv1::Document document = field_guide();
  const auto chunks = chunk_hierarchical(document, {}, {}, "guide.pdf");
  const auto* list = find_chunk(chunks, "- nests");
  require(list != nullptr, "the list group must chunk");
  require_eq(list->text(), "- nests\n- songs", "unordered list serialization");
  require_eq(static_cast<int>(list->doc_items().size()), 3,
             "the list chunk consumes the group and both items");
  require_eq(list->doc_items(0), "#/groups/0", "the group leads its own doc items");
  for (const auto& chunk : chunks) {
    require(chunk.text() != "nests" && chunk.text() != "songs",
            "a consumed list item never chunks on its own");
  }

  docv1::Document ordered = new_document();
  add_list(&ordered, true, {"first", "second", "third"});
  const auto ordered_chunks = chunk_hierarchical(ordered, {}, {}, "list.pdf");
  require_eq(static_cast<int>(ordered_chunks.size()), 1, "one chunk per list group");
  require_eq(ordered_chunks.front().text(), "1. first\n2. second\n3. third",
             "an ordered list numbers by actual index");
}

void verify_table_serialization_and_its_degradations() {
  const docv1::Document document = field_guide();
  const auto chunks = chunk_hierarchical(document, {}, {}, "guide.pdf");
  const auto* table = find_chunk(chunks, "Sparrow, Weight");
  require(table != nullptr, "the table must chunk");
  require_eq(table->text(),
             "Table 1. Body sizes.\nSparrow, Weight = 30 g. Sparrow, Height = 15 cm. "
             "Crow, Weight = 500 g. Crow, Height = 50 cm",
             "a headed table flattens to labelled triplets under its caption");
  require_eq(static_cast<int>(table->captions().size()), 1, "the caption is reported");
  require_eq(table->captions(0), "Table 1. Body sizes.", "the caption text");
  for (const auto& chunk : chunks) {
    require(chunk.text() != "Table 1. Body sizes.",
            "a claimed caption never chunks on its own");
  }

  docv1::Document single_column = new_document();
  add_table(&single_column, {{{"only", false, false}}, {{"cell", false, false}}});
  require_eq(chunk_hierarchical(single_column, {}, {}, "t.pdf").front().text(),
             "only. cell", "a single-column table degrades to its cell texts");

  docv1::Document header_only = new_document();
  add_table(&header_only, {{{"A", true, false}, {"B", true, false}}});
  require_eq(chunk_hierarchical(header_only, {}, {}, "t.pdf").front().text(), "A. B",
             "a header-only table degrades to its cell texts");

  docv1::Document headerless = new_document();
  add_table(&headerless, {{{"a", false, false}, {"b", false, false}},
                          {{"c", false, false}, {"d", false, false}}});
  require_eq(chunk_hierarchical(headerless, {}, {}, "t.pdf").front().text(), "a. b. c. d",
             "a table with no headers degrades to its cell texts");

  docv1::Document markdown = new_document();
  add_table(&markdown, {{{"Name", true, false}, {"No|te", true, false}},
                        {{"Crow", false, true}, {"loud", false, false}}});
  const ChunkOptions pipes{true, false};
  require_eq(chunk_hierarchical(markdown, {}, pipes, "t.pdf").front().text(),
             "| Name | No\\|te |\n| --- | --- |\n| Crow | loud |",
             "use_markdown_tables serializes pipe tables with escaped pipes");
}

void verify_picture_chunks_carry_captions_only() {
  const docv1::Document document = field_guide();
  const auto chunks = chunk_hierarchical(document, {}, {}, "guide.pdf");
  const auto* picture = find_chunk(chunks, "Figure 1");
  require(picture != nullptr, "a captioned picture chunks");
  require_eq(picture->text(), "Figure 1. A crow.", "a picture serializes its captions only");
  require_eq(static_cast<int>(picture->page_numbers().size()), 1, "picture page count");
  require_eq(picture->page_numbers(0), 3, "picture page number");

  docv1::Document bare = new_document();
  add_picture(&bare, {});
  require(chunk_hierarchical(bare, {}, {}, "p.pdf").empty(),
          "an uncaptioned picture emits no placeholder chunk");
}

// A caption reference two tables both name belongs to the first of them.
// Absorbing it twice would repeat the text and the reference and widen both
// spans into an overlap, which is exactly what the contiguity property
// forbids.
void verify_a_shared_caption_is_claimed_once() {
  docv1::Document document = new_document();
  const std::string caption = add_paragraph(&document, "Table 1. Shared caption.", 1);
  add_table(&document, {{{"Name", true, false}, {"Note", true, false}},
                        {{"Crow", false, true}, {"loud", false, false}}},
            {caption}, 1);
  add_table(&document, {{{"Name", true, false}, {"Note", true, false}},
                        {{"Wren", false, true}, {"small", false, false}}},
            {caption}, 2);
  add_paragraph(&document, "After the tables.", 2);
  const OffsetTable offsets = offsets_for(document);
  const auto chunks = chunk_hierarchical(document, offsets, {}, "shared.pdf");
  require_eq(static_cast<int>(chunks.size()), 3, "two tables and a paragraph chunk");

  const auto& first = chunks[0];
  require(first.text().starts_with("Table 1. Shared caption.\n"),
          "the first table folds the caption into its text");
  require_eq(static_cast<int>(first.captions().size()), 1, "the first table reports it");
  require_eq(static_cast<int>(first.doc_items().size()), 2,
             "the first table consumes the table and the caption");

  const auto& second = chunks[1];
  require(!second.text().contains("Shared caption"),
          "the second table does not repeat a caption it did not claim");
  require(second.captions().empty(), "the second table reports no caption");
  for (const auto& item : second.doc_items()) {
    require(item != caption, "the caption reference belongs to one chunk only");
  }
  require(!second.has_start_offset(),
          "the second table has no text item of its own, so it has no span");

  bool seen = false;
  std::int64_t previous_end = 0;
  for (const auto& chunk : chunks) {
    if (!chunk.has_start_offset()) continue;
    if (seen) {
      require(chunk.start_offset() >= previous_end,
              "a shared caption must not widen two chunks into an overlap");
    }
    previous_end = chunk.end_offset();
    seen = true;
  }
}

void verify_pages_items_and_offsets_propagate() {
  const docv1::Document document = field_guide();
  const OffsetTable offsets = offsets_for(document);
  const auto chunks = chunk_hierarchical(document, offsets, {}, "guide.pdf");

  const auto* sparrows = find_chunk(chunks, "Sparrows are small");
  require(sparrows != nullptr, "the paragraph must chunk");
  require(sparrows->has_start_offset() && sparrows->has_end_offset(),
          "a text chunk with an offset entry reports its span");
  const auto entry = offsets.at(sparrows->doc_items(0));
  require(static_cast<std::uint64_t>(sparrows->start_offset()) == entry.start &&
              static_cast<std::uint64_t>(sparrows->end_offset()) == entry.end,
          "the span is the item's own entry");
  require_eq(sparrows->page_numbers(0), 1, "the chunk carries the item's page");
  require_eq(sparrows->metadata().at("min_confidence"), "0.94",
             "the lowest collector confidence rides as metadata");
  require_eq(sparrows->metadata().at("text_source"), "digital",
             "the offset table's source rides as metadata");

  const auto* list = find_chunk(chunks, "- nests");
  require(list != nullptr, "the list must chunk");
  require(list->has_start_offset(), "a list chunk spans its items");
  require(static_cast<std::uint64_t>(list->start_offset()) ==
              offsets.at(list->doc_items(1)).start &&
          static_cast<std::uint64_t>(list->end_offset()) ==
              offsets.at(list->doc_items(2)).end,
          "the list span is the union of its items' entries");
  require_eq(list->metadata().at("min_confidence"), "0.62",
             "the list reports the lowest confidence of its items");

  // A single missing entry withdraws the whole span rather than reporting a
  // partial one.
  OffsetTable partial = offsets;
  partial.erase(list->doc_items(2));
  const auto partial_chunks = chunk_hierarchical(document, partial, {}, "guide.pdf");
  const auto* partial_list = find_chunk(partial_chunks, "- nests");
  require(partial_list != nullptr, "the list still chunks without its entries");
  require(!partial_list->has_start_offset() && !partial_list->has_end_offset(),
          "a chunk with one item missing its entry reports no span at all");

  const auto no_offsets = chunk_hierarchical(document, {}, {}, "guide.pdf");
  for (const auto& chunk : no_offsets) {
    require(!chunk.has_start_offset() && !chunk.has_end_offset(),
            "a document with no offset table yields no invented spans");
    require(!chunk.metadata().contains("text_source"),
            "text_source is reported only when the offset table provides it");
  }
}

void verify_mixed_text_source_is_reported() {
  docv1::Document document = new_document();
  const std::string first = add_paragraph(&document, "one", 1);
  const std::string second = add_paragraph(&document, "two", 1);
  std::vector<std::string> items;
  add_list(&document, false, {"a", "b"}, 1, &items);
  OffsetTable offsets;
  offsets[first] = OffsetEntry{0, 3, parsev1::TEXT_SOURCE_DIGITAL_PDF};
  offsets[second] = OffsetEntry{4, 7, parsev1::TEXT_SOURCE_OCR};
  offsets[items.at(0)] = OffsetEntry{8, 9, parsev1::TEXT_SOURCE_DIGITAL_PDF};
  offsets[items.at(1)] = OffsetEntry{10, 11, parsev1::TEXT_SOURCE_OCR};
  const auto chunks = chunk_hierarchical(document, offsets, {}, "mix.pdf");
  require(chunks.size() == 3, "one chunk per paragraph plus the list");
  require_eq(find_chunk(chunks, "one")->metadata().at("text_source"), "digital",
             "a digital-only chunk");
  require_eq(find_chunk(chunks, "two")->metadata().at("text_source"), "ocr",
             "an OCR-only chunk");
  require_eq(find_chunk(chunks, "- a")->metadata().at("text_source"), "mixed",
             "a chunk spanning both sources");
}

void verify_non_body_layers_are_skipped() {
  docv1::Document document = new_document();
  add_paragraph(&document, "body text");
  const std::string hidden = add_paragraph(&document, "page footer");
  const int index = std::stoi(hidden.substr(std::string("#/texts/").size()));
  document.mutable_texts(index)->mutable_text()->mutable_base()->set_content_layer(
      docv1::CONTENT_LAYER_FURNITURE);
  const auto chunks = chunk_hierarchical(document, {}, {}, "d.pdf");
  require_eq(static_cast<int>(chunks.size()), 1, "only the body layer chunks");
  require_eq(chunks.front().text(), "body text", "the body item survives");
}

void verify_chunk_spans_are_ordered_and_disjoint() {
  const docv1::Document document = field_guide();
  const OffsetTable offsets = offsets_for(document);
  const auto chunks = chunk_hierarchical(document, offsets, {}, "guide.pdf");
  bool seen = false;
  std::int64_t previous_end = 0;
  int spanned = 0;
  for (const auto& chunk : chunks) {
    if (!chunk.has_start_offset()) continue;
    ++spanned;
    require(chunk.start_offset() < chunk.end_offset(), "a span is half-open and non-empty");
    if (seen) {
      require(chunk.start_offset() >= previous_end,
              "chunk spans must not overlap and must run in document order");
    }
    previous_end = chunk.end_offset();
    seen = true;
  }
  require(spanned >= 4, "the fixture must exercise several spanned chunks");
}

// -- wordish/1 --------------------------------------------------------------

void verify_wordish_token_counts() {
  const struct {
    const char* text;
    int expected;
  } cases[] = {
      {"", 0},
      {"   \n\t ", 0},
      {"hello", 1},
      {"hello world", 2},
      {"  spaced   out  ", 2},
      {"hello, world!", 4},
      {"don't", 3},
      {"3.14", 3},
      {"a-b", 3},
      {"naive cafe", 2},
      {"na\xC3\xAFve caf\xC3\xA9", 2},
      {"\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC", 1},
      {"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80", 2},
      {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 3},
      {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xA7\xE3\x81\x99", 5},
      {"\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF world", 6},
      {"a\xC2\xA0"
       "b",
       2},
      {"\xE2\x86\x92", 1},
      {"one\ntwo\nthree", 3},
  };
  for (const auto& [text, expected] : cases) {
    require_eq(count_tokens(text), expected,
               std::string("wordish/1 count of [") + text + "]");
  }
}

// -- hybrid -----------------------------------------------------------------

parsev1::HybridChunkerOptions hybrid_options(int max_tokens) {
  parsev1::HybridChunkerOptions options;
  options.set_max_tokens(max_tokens);
  return options;
}

void verify_hybrid_rejects_undecidable_options() {
  parsev1::HybridChunkerOptions missing;
  const grpc::Status no_budget = validate_hybrid_options(missing);
  require(no_budget.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a hybrid request without a budget must be rejected");
  require(no_budget.error_message().contains("max_tokens"),
          "the rejection names the missing field: " + no_budget.error_message());

  parsev1::HybridChunkerOptions zero = hybrid_options(0);
  require(validate_hybrid_options(zero).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a non-positive budget must be rejected");

  parsev1::HybridChunkerOptions foreign = hybrid_options(64);
  foreign.set_tokenizer("bert-base-uncased");
  const grpc::Status unknown = validate_hybrid_options(foreign);
  require(unknown.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "an unknown tokenizer must be rejected");
  require(unknown.error_message().contains("wordish/1"),
          "the rejection lists the supported tokenizers: " + unknown.error_message());

  parsev1::HybridChunkerOptions accepted = hybrid_options(64);
  accepted.set_tokenizer("wordish/1");
  require(validate_hybrid_options(accepted).ok(), "the implemented tokenizer is accepted");
}

void verify_hybrid_merges_only_equal_heading_trails() {
  docv1::Document document = new_document();
  add_section(&document, "Alpha", 1);
  add_paragraph(&document, "one two");
  add_paragraph(&document, "three four");
  add_section(&document, "Beta", 1);
  add_paragraph(&document, "five six");

  const auto merged = chunk_hybrid(document, {}, hybrid_options(64), "d.pdf");
  require_eq(static_cast<int>(merged.size()), 2, "peers under one trail merge into one chunk");
  require_eq(merged.front().text(), "one two\nthree four", "merged peers join with a newline");
  require(headings_of(merged.front()) == std::vector<std::string>({"Alpha"}),
          "the merged chunk keeps the shared trail");
  require_eq(merged.back().text(), "five six", "a different trail always breaks the run");
  require_eq(merged.front().rules_digest(),
             "grparse-hybrid/1;tok=wordish/1;sent=sentence/1;max_tokens=64;merge_peers=true",
             "the hybrid digest spells out every boundary input");

  // The budget, not the trail, is what stops a merge here.
  const auto tight = chunk_hybrid(document, {}, hybrid_options(4), "d.pdf");
  require_eq(static_cast<int>(tight.size()), 3, "a budget that fits no peer merges nothing");

  parsev1::HybridChunkerOptions no_merge = hybrid_options(64);
  no_merge.set_merge_peers(false);
  const auto unmerged = chunk_hybrid(document, {}, no_merge, "d.pdf");
  require_eq(static_cast<int>(unmerged.size()), 3, "merge_peers false keeps peers apart");
  require(unmerged.front().rules_digest().contains("merge_peers=false"),
          "the digest records the merge decision");
}

void verify_hybrid_splits_oversized_chunks() {
  docv1::Document document = new_document();
  add_paragraph(&document,
                "Alpha beta gamma delta. Epsilon zeta eta theta. Iota kappa lambda mu.");
  const auto chunks = chunk_hybrid(document, {}, hybrid_options(5), "d.pdf");
  require_eq(static_cast<int>(chunks.size()), 3, "one sentence per chunk at this budget");
  require_eq(chunks[0].text(), "Alpha beta gamma delta.", "the first sentence packs alone");
  require_eq(chunks[1].text(), "Epsilon zeta eta theta.", "the second sentence packs alone");
  require_eq(chunks[2].text(), "Iota kappa lambda mu.", "the last sentence packs alone");
  for (const auto& chunk : chunks) {
    require_eq(chunk.num_tokens(), count_tokens(chunk.text()),
               "num_tokens counts the contextualized text");
    require(chunk.num_tokens() <= 5, "no piece exceeds the budget");
  }

  // Two sentences fit together, the third does not.
  const auto packed = chunk_hybrid(document, {}, hybrid_options(11), "d.pdf");
  require_eq(static_cast<int>(packed.size()), 2, "greedy packing fills to the budget");
  require_eq(packed[0].text(), "Alpha beta gamma delta. Epsilon zeta eta theta.",
             "packed sentences keep their original spacing");
}

void verify_hybrid_falls_back_to_words_then_hard_cuts() {
  docv1::Document words = new_document();
  add_paragraph(&words, "alpha beta gamma delta epsilon zeta");
  const auto word_chunks = chunk_hybrid(words, {}, hybrid_options(2), "d.pdf");
  require_eq(static_cast<int>(word_chunks.size()), 3,
             "a sentence over budget splits at word boundaries");
  require_eq(word_chunks[0].text(), "alpha beta", "words pack greedily");
  require_eq(word_chunks[2].text(), "epsilon zeta", "the tail keeps its words");

  docv1::Document glued = new_document();
  add_paragraph(&glued, "a,b,c,d,e,f,g");
  const auto cut = chunk_hybrid(glued, {}, hybrid_options(4), "d.pdf");
  require_eq(static_cast<int>(cut.size()), 4,
             "a single word over budget hard-cuts at the code point limit");
  require_eq(cut[0].text(), "a,b,", "the cut is by code point count");
  require_eq(cut[3].text(), "g", "the remainder is the last piece");
  for (const auto& chunk : cut) require(chunk.num_tokens() <= 4, "a hard cut respects the budget");
}

void verify_hybrid_headings_count_against_the_budget() {
  docv1::Document document = new_document();
  add_section(&document, "Heading words here", 1);
  add_paragraph(&document, "alpha beta gamma delta.");
  const auto chunks = chunk_hybrid(document, {}, hybrid_options(6), "d.pdf");
  require(chunks.size() >= 2, "the heading block eats into the budget");
  for (const auto& chunk : chunks) {
    require(headings_of(chunk) == std::vector<std::string>({"Heading words here"}),
            "split pieces keep the trail");
    require_eq(chunk.num_tokens(), count_tokens("Heading words here\n" + chunk.text()),
               "num_tokens is the contextualized count");
  }
}

void verify_split_pieces_narrow_offsets_only_when_exact() {
  docv1::Document document = new_document();
  const std::string ref = add_paragraph(&document, "Alpha beta. Gamma delta.", 1);
  OffsetTable offsets;
  offsets[ref] = OffsetEntry{100, 124, parsev1::TEXT_SOURCE_OCR};
  const auto chunks = chunk_hybrid(document, offsets, hybrid_options(3), "d.pdf");
  require_eq(static_cast<int>(chunks.size()), 2, "the paragraph splits in two");
  require_eq(static_cast<int>(chunks[0].start_offset()), 100, "the first piece starts at the item");
  require_eq(static_cast<int>(chunks[0].end_offset()), 111, "the first piece ends at its own text");
  require_eq(static_cast<int>(chunks[1].start_offset()), 112, "the second piece follows");
  require_eq(static_cast<int>(chunks[1].end_offset()), 124, "the second piece ends at the item");

  // A chunk whose text is not one entry's text verbatim cannot be narrowed.
  docv1::Document listed = new_document();
  std::vector<std::string> items;
  add_list(&listed, false, {"Alpha beta gamma.", "Delta epsilon zeta."}, 1, &items);
  OffsetTable list_offsets;
  list_offsets[items.at(0)] = OffsetEntry{0, 17, parsev1::TEXT_SOURCE_OCR};
  list_offsets[items.at(1)] = OffsetEntry{18, 37, parsev1::TEXT_SOURCE_OCR};
  const auto list_chunks = chunk_hybrid(listed, list_offsets, hybrid_options(4), "d.pdf");
  require(list_chunks.size() >= 2, "the list chunk splits");
  for (const auto& chunk : list_chunks) {
    require(!chunk.has_start_offset(),
            "a split piece of a serialized chunk reports no span");
  }
}

void verify_raw_text_mirrors_text_when_requested() {
  docv1::Document document = new_document();
  add_paragraph(&document, "alpha beta");
  const auto plain = chunk_hierarchical(document, {}, {}, "d.pdf");
  require(!plain.front().has_raw_text(), "raw_text stays unset by default");
  const auto raw = chunk_hierarchical(document, {}, ChunkOptions{false, true}, "d.pdf");
  require(raw.front().has_raw_text() && raw.front().raw_text() == raw.front().text(),
          "include_raw_text repeats the chunk text");
  require_eq(raw.front().filename(), "d.pdf", "every chunk names its source file");
}

void verify_hybrid_digest_reports_the_budget() {
  require_eq(hybrid_rules_digest(512, true),
             "grparse-hybrid/1;tok=wordish/1;sent=sentence/1;max_tokens=512;merge_peers=true",
             "the hybrid digest string is fixed");
  require_eq(hybrid_rules_digest(8, false),
             "grparse-hybrid/1;tok=wordish/1;sent=sentence/1;max_tokens=8;merge_peers=false",
             "the hybrid digest string is fixed");
}

void verify_sentence_rule_boundaries() {
  const auto spans = [](const std::string& text) {
    const auto points = grparse::chunking::decode_utf8(text);
    std::vector<std::string> parts;
    for (const auto& span : grparse::chunking::split_sentences(points)) {
      parts.push_back(grparse::chunking::encode_utf8(points.data() + span.begin,
                                                     points.data() + span.end));
    }
    return parts;
  };
  require(spans("One. Two.") == std::vector<std::string>({"One. ", "Two."}),
          "a terminator followed by space ends a sentence");
  require(spans("He said \"go.\" She left.") ==
              std::vector<std::string>({"He said \"go.\" ", "She left."}),
          "closing quotes ride with the terminator");
  require(spans("3.14 is pi.") == std::vector<std::string>({"3.14 is pi."}),
          "a terminator glued to more text is not a boundary");
  require(spans("Dr. Who") == std::vector<std::string>({"Dr. ", "Who"}),
          "abbreviations split by design");
  require(spans("no terminator here") == std::vector<std::string>({"no terminator here"}),
          "text with no terminator is one sentence");
}

struct Case {
  const char* name;
  void (*run)();
};

const Case kCases[] = {
    {"determinism across runs and threads", verify_chunking_is_byte_identical_across_runs_and_threads},
    {"heading trail shadowing", verify_heading_trail_shadows_and_pops},
    {"list group consumption", verify_list_group_consumes_its_items},
    {"table serialization", verify_table_serialization_and_its_degradations},
    {"picture captions", verify_picture_chunks_carry_captions_only},
    {"shared caption claiming", verify_a_shared_caption_is_claimed_once},
    {"pages, items and offsets", verify_pages_items_and_offsets_propagate},
    {"text source metadata", verify_mixed_text_source_is_reported},
    {"content layers", verify_non_body_layers_are_skipped},
    {"span contiguity", verify_chunk_spans_are_ordered_and_disjoint},
    {"wordish/1 counts", verify_wordish_token_counts},
    {"sentence/1 boundaries", verify_sentence_rule_boundaries},
    {"hybrid option validation", verify_hybrid_rejects_undecidable_options},
    {"hybrid peer merging", verify_hybrid_merges_only_equal_heading_trails},
    {"hybrid sentence splitting", verify_hybrid_splits_oversized_chunks},
    {"hybrid word and hard-cut fallback", verify_hybrid_falls_back_to_words_then_hard_cuts},
    {"hybrid heading budget", verify_hybrid_headings_count_against_the_budget},
    {"hybrid offset narrowing", verify_split_pieces_narrow_offsets_only_when_exact},
    {"raw text option", verify_raw_text_mirrors_text_when_requested},
    {"hybrid digest", verify_hybrid_digest_reports_the_budget},
};

}  // namespace

int main() {
  int failures = 0;
  for (const auto& [name, run] : kCases) {
    try {
      run();
      std::println("ok   {}", name);
    } catch (const std::exception& error) {
      std::println("FAIL {}: {}", name, error.what());
      ++failures;
    }
  }
  if (failures != 0) {
    std::println("{} chunking case(s) failed", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
