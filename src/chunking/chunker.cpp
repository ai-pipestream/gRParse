#include "chunker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../render/canonical_json_writer.h"
#include "../render/renderer_base.h"
#include "sentence_rules.h"
#include "token_counter.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse::chunking {
namespace {

using grparse::render::ArenaRef;
using grparse::render::parse_ref;
using grparse::render::table_grid;
using grparse::render::text_base;
using grparse::render::trimmed;

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) out += separator;
    out += parts[index];
  }
  return out;
}

// One chunk while it is still being built. The proto is materialized only
// once every pass has run, so merging and splitting stay plain data folds.
struct WorkChunk {
  std::string text;
  std::vector<std::string> headings;
  std::vector<std::string> captions;
  std::vector<std::string> doc_items;
  std::set<int> pages;
  // The offset span is reported only when every text item the chunk consumed
  // had an entry (offsets_complete) and at least one did (offsets_known).
  bool offsets_known = false;
  bool offsets_complete = true;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  // True when the chunk is exactly one offset-table entry's text, which is
  // the only case where a split piece's own span can be derived.
  bool offsets_exact = false;
  std::optional<double> min_confidence;
  bool saw_digital = false;
  bool saw_ocr = false;
};

// The headings block a chunk is contextualized with: the trail joined with
// newlines, then a newline before the text. Empty trail, no prefix.
std::string contextualized(const WorkChunk& chunk) {
  std::string out;
  for (const auto& heading : chunk.headings) {
    out += heading;
    out.push_back('\n');
  }
  out += chunk.text;
  return out;
}

int heading_tokens(const WorkChunk& chunk) {
  std::string block;
  for (const auto& heading : chunk.headings) {
    block += heading;
    block.push_back('\n');
  }
  return count_tokens(block);
}

// The item fields the chunkers fold regardless of which arena an item lives
// in: where it came from on the page and which engine produced it.
struct ItemView {
  const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov = nullptr;
  const google::protobuf::RepeatedPtrField<docv1::SourceType>* source = nullptr;
  // True for items in the texts arena, the only ones the offset table keys.
  bool is_text = false;
};

std::string escaped_cell(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : trimmed(text)) {
    if (character == '|') {
      out += "\\|";
      continue;
    }
    // A newline inside a cell would break the row; a space keeps the row
    // shape and the reading order.
    out.push_back(character == '\n' || character == '\r' ? ' ' : character);
  }
  return out;
}

// -- table serialization ----------------------------------------------------

// Every cell text in row-major order, each spanned cell counted once. This is
// the degradation the flattening falls back to.
std::vector<std::string> flattened_cells(
    const std::vector<std::vector<const docv1::TableCell*>>& grid) {
  std::vector<std::string> texts;
  std::set<const docv1::TableCell*> seen;
  for (const auto& row : grid) {
    for (const auto* cell : row) {
      if (cell == nullptr || !seen.insert(cell).second) continue;
      const std::string text = trimmed(cell->text());
      if (!text.empty()) texts.push_back(text);
    }
  }
  return texts;
}

std::string serialize_table_triplets(const docv1::TableData& data) {
  const auto grid = table_grid(data);
  if (grid.empty()) return std::string();
  std::size_t columns = 0;
  for (const auto& row : grid) columns = std::max(columns, row.size());

  bool any_header = false;
  for (const auto& row : grid) {
    for (const auto* cell : row) {
      if (cell != nullptr && (cell->column_header() || cell->row_header())) {
        any_header = true;
      }
    }
  }

  // A column's label is the first column-header cell standing over it.
  std::vector<std::string> column_labels(columns);
  for (std::size_t column = 0; column < columns; ++column) {
    for (const auto& row : grid) {
      if (column >= row.size()) continue;
      const auto* cell = row[column];
      if (cell == nullptr || !cell->column_header()) continue;
      const std::string label = trimmed(cell->text());
      if (label.empty()) continue;
      column_labels[column] = label;
      break;
    }
  }

  std::vector<std::string> entries;
  for (const auto& row : grid) {
    bool header_row = true;
    for (const auto* cell : row) {
      if (cell != nullptr && !cell->column_header()) header_row = false;
    }
    if (header_row) continue;
    std::string row_label;
    for (const auto* cell : row) {
      if (cell == nullptr || !cell->row_header()) continue;
      row_label = trimmed(cell->text());
      if (!row_label.empty()) break;
    }
    std::set<const docv1::TableCell*> seen;
    for (std::size_t column = 0; column < row.size(); ++column) {
      const auto* cell = row[column];
      if (cell == nullptr || !seen.insert(cell).second) continue;
      if (cell->column_header() || cell->row_header()) continue;
      const std::string value = trimmed(cell->text());
      if (value.empty()) continue;
      std::vector<std::string> labels;
      if (!row_label.empty()) labels.push_back(row_label);
      if (!column_labels[column].empty()) labels.push_back(column_labels[column]);
      entries.push_back(labels.empty() ? value
                                       : join(labels, ", ") + " = " + value);
    }
  }

  // No headers to name a value by, one column to name it in, or nothing but
  // headers: the triplets would be noise, so the cells speak for themselves.
  if (!any_header || columns <= 1 || entries.empty()) {
    return join(flattened_cells(grid), ". ");
  }
  return join(entries, ". ");
}

std::string serialize_table_markdown(const docv1::TableData& data) {
  const auto grid = table_grid(data);
  if (grid.empty()) return std::string();
  std::size_t columns = 0;
  for (const auto& row : grid) columns = std::max(columns, row.size());
  if (columns == 0) return std::string();

  const auto row_text = [&](const std::vector<const docv1::TableCell*>& row) {
    std::vector<std::string> cells(columns);
    for (std::size_t column = 0; column < row.size(); ++column) {
      if (row[column] != nullptr) cells[column] = escaped_cell(row[column]->text());
    }
    return "| " + join(cells, " | ") + " |";
  };

  bool leading_header = false;
  for (const auto* cell : grid.front()) {
    if (cell != nullptr && cell->column_header()) leading_header = true;
  }

  std::vector<std::string> lines;
  if (leading_header) {
    lines.push_back(row_text(grid.front()));
  } else {
    // Markdown has no header-less table; an empty header row keeps the shape
    // readable without inventing labels.
    lines.push_back("| " + join(std::vector<std::string>(columns), " | ") + " |");
  }
  std::vector<std::string> rule(columns, "---");
  lines.push_back("| " + join(rule, " | ") + " |");
  for (std::size_t index = leading_header ? 1 : 0; index < grid.size(); ++index) {
    lines.push_back(row_text(grid[index]));
  }
  return join(lines, "\n");
}

// -- the walk ---------------------------------------------------------------

class Chunker {
 public:
  Chunker(const docv1::Document& document, const OffsetTable& offsets,
          const ChunkOptions& options)
      : document_(document), offsets_(offsets), options_(options) {
    collect_caption_refs();
  }

  std::vector<WorkChunk> run() {
    walk("#/body");
    return std::move(chunks_);
  }

 private:
  const docv1::Document& document_;
  const OffsetTable& offsets_;
  const ChunkOptions& options_;
  std::set<std::string> visited_;
  std::set<std::string> caption_refs_;
  // The heading trail in force, outermost first, each with the level that
  // recorded it.
  std::vector<std::pair<int, std::string>> trail_;
  std::vector<WorkChunk> chunks_;

  void collect_caption_refs() {
    const auto claim = [this](const auto& item) {
      for (const auto& ref : item.captions()) caption_refs_.insert(ref.ref());
    };
    for (const auto& item : document_.tables()) claim(item);
    for (const auto& item : document_.pictures()) claim(item);
    for (const auto& item : document_.texts()) {
      if (item.item_case() == docv1::BaseTextItem::kCode) claim(item.code());
    }
  }

  const docv1::GroupItem* group_at(const std::string& ref) const {
    if (ref == "#/body") return &document_.body();
    if (ref == "#/furniture") return &document_.furniture();
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kGroup && parsed.index < document_.groups_size()) {
      return &document_.groups(parsed.index);
    }
    return nullptr;
  }

  const docv1::BaseTextItem* text_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kText && parsed.index < document_.texts_size()) {
      return &document_.texts(parsed.index);
    }
    return nullptr;
  }

  const docv1::TableItem* table_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kTable && parsed.index < document_.tables_size()) {
      return &document_.tables(parsed.index);
    }
    return nullptr;
  }

  const docv1::PictureItem* picture_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kPicture && parsed.index < document_.pictures_size()) {
      return &document_.pictures(parsed.index);
    }
    return nullptr;
  }

  // Only the body layer chunks. An unspecified layer is the producer default
  // and counts as body; every other layer is one the producer chose
  // deliberately, so its subtree is left out entirely.
  static bool excluded(docv1::ContentLayer layer) {
    return layer != docv1::CONTENT_LAYER_BODY &&
           layer != docv1::CONTENT_LAYER_UNSPECIFIED;
  }

  docv1::ContentLayer layer_of(const std::string& ref) const {
    if (const auto* group = group_at(ref)) return group->content_layer();
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        return text->code().content_layer();
      }
      const auto* base = text_base(*text);
      return base == nullptr ? docv1::CONTENT_LAYER_UNSPECIFIED : base->content_layer();
    }
    if (const auto* table = table_at(ref)) return table->content_layer();
    if (const auto* picture = picture_at(ref)) return picture->content_layer();
    return docv1::CONTENT_LAYER_UNSPECIFIED;
  }

  std::optional<ItemView> view_of(const std::string& ref) const {
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        return ItemView{&text->code().prov(), &text->code().source(), true};
      }
      const auto* base = text_base(*text);
      if (base == nullptr) return std::nullopt;
      return ItemView{&base->prov(), &base->source(), true};
    }
    if (const auto* table = table_at(ref)) {
      return ItemView{&table->prov(), &table->source(), false};
    }
    if (const auto* picture = picture_at(ref)) {
      return ItemView{&picture->prov(), &picture->source(), false};
    }
    return std::nullopt;
  }

  static std::string text_of(const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) return item.code().text();
    const auto* base = text_base(item);
    return base == nullptr ? std::string() : base->text();
  }

  std::vector<std::string> trail_texts() const {
    std::vector<std::string> headings;
    headings.reserve(trail_.size());
    for (const auto& [level, text] : trail_) headings.push_back(text);
    return headings;
  }

  // A heading shadows every recorded heading at or below its own level.
  void record_heading(int level, const std::string& text) {
    while (!trail_.empty() && trail_.back().first >= level) trail_.pop_back();
    if (!trimmed(text).empty()) trail_.emplace_back(level, text);
  }

  static bool list_group(const docv1::GroupItem& group) {
    return group.label() == docv1::GROUP_LABEL_LIST ||
           group.label() == docv1::GROUP_LABEL_ORDERED_LIST;
  }

  // Records one consumed item on the chunk: its reference, the pages it
  // proves, the confidence its producer reported, and its offset entry.
  void absorb(WorkChunk* chunk, const std::string& ref) {
    visited_.insert(ref);
    chunk->doc_items.push_back(ref);
    const auto view = view_of(ref);
    if (!view.has_value()) return;
    for (const auto& provenance : *view->prov) {
      if (provenance.page_no() > 0) chunk->pages.insert(provenance.page_no());
    }
    for (const auto& source : *view->source) {
      if (!source.has_collector() || !source.collector().has_confidence()) continue;
      const double confidence = source.collector().confidence();
      if (!chunk->min_confidence.has_value() || confidence < *chunk->min_confidence) {
        chunk->min_confidence = confidence;
      }
    }
    if (!view->is_text) return;
    const auto entry = offsets_.find(ref);
    if (entry == offsets_.end()) {
      chunk->offsets_complete = false;
      return;
    }
    if (!chunk->offsets_known) {
      chunk->start = entry->second.start;
      chunk->end = entry->second.end;
      chunk->offsets_known = true;
    } else {
      chunk->start = std::min(chunk->start, entry->second.start);
      chunk->end = std::max(chunk->end, entry->second.end);
    }
    if (entry->second.source == parsev1::TEXT_SOURCE_DIGITAL_PDF) chunk->saw_digital = true;
    if (entry->second.source == parsev1::TEXT_SOURCE_OCR) chunk->saw_ocr = true;
  }

  // The caption texts a table or picture claims, in reference order. Each is
  // absorbed so the walk never emits it a second time.
  std::vector<std::string> caption_texts(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions,
      WorkChunk* chunk) {
    std::vector<std::string> texts;
    for (const auto& ref : captions) {
      const auto* text = text_at(ref.ref());
      if (text == nullptr) continue;
      absorb(chunk, ref.ref());
      const std::string body = text_of(*text);
      if (!trimmed(body).empty()) texts.push_back(body);
    }
    return texts;
  }

  void mark_exact(WorkChunk* chunk) {
    chunk->offsets_exact =
        chunk->offsets_known && chunk->offsets_complete &&
        chunk->doc_items.size() == 1 &&
        chunk->end >= chunk->start &&
        chunk->end - chunk->start == codepoint_length(chunk->text);
  }

  void walk(const std::string& ref) {
    if (!visited_.insert(ref).second) return;
    if (excluded(layer_of(ref))) return;
    if (const auto* group = group_at(ref)) {
      if (list_group(*group)) {
        emit_list(ref, *group);
        return;
      }
      // Every other group is structure, not content: it emits nothing of its
      // own and its children chunk in place.
      for (const auto& child : group->children()) walk(child.ref());
      return;
    }
    if (const auto* text = text_at(ref)) {
      emit_text(ref, *text);
      const auto* base = text->item_case() == docv1::BaseTextItem::kCode
                             ? nullptr
                             : text_base(*text);
      if (base != nullptr) {
        for (const auto& child : base->children()) walk(child.ref());
      }
      return;
    }
    if (const auto* table = table_at(ref)) {
      emit_table(ref, *table);
      return;
    }
    if (const auto* picture = picture_at(ref)) {
      emit_picture(ref, *picture);
      return;
    }
    // Key-value and form areas have no text/1 serialization; they contribute
    // no chunk rather than a guessed one.
  }

  void emit_text(const std::string& ref, const docv1::BaseTextItem& item) {
    // A caption chunks through the table or picture that claims it.
    if (caption_refs_.contains(ref)) return;
    if (item.item_case() == docv1::BaseTextItem::kTitle) {
      record_heading(0, item.title().base().text());
      return;
    }
    if (item.item_case() == docv1::BaseTextItem::kSectionHeader) {
      record_heading(item.section_header().level(), item.section_header().base().text());
      return;
    }
    WorkChunk chunk;
    chunk.headings = trail_texts();
    absorb(&chunk, ref);
    chunk.text = text_of(item);
    if (trimmed(chunk.text).empty()) return;
    mark_exact(&chunk);
    chunks_.push_back(std::move(chunk));
  }

  void collect_list_entries(const docv1::GroupItem& group, WorkChunk* chunk,
                            std::vector<std::string>* lines) {
    const bool ordered = group.label() == docv1::GROUP_LABEL_ORDERED_LIST;
    int position = 0;
    for (const auto& child : group.children()) {
      const std::string& ref = child.ref();
      if (visited_.contains(ref) || excluded(layer_of(ref))) continue;
      if (const auto* nested = group_at(ref)) {
        visited_.insert(ref);
        chunk->doc_items.push_back(ref);
        // A nested list flattens into the same chunk; its own numbering
        // restarts, matching how it reads on the page.
        collect_list_entries(*nested, chunk, lines);
        continue;
      }
      const auto* text = text_at(ref);
      if (text == nullptr) continue;
      absorb(chunk, ref);
      const std::string body = text_of(*text);
      if (trimmed(body).empty()) continue;
      ++position;
      lines->push_back((ordered ? std::to_string(position) + ". " : std::string("- ")) +
                       body);
    }
  }

  void emit_list(const std::string& ref, const docv1::GroupItem& group) {
    WorkChunk chunk;
    chunk.headings = trail_texts();
    chunk.doc_items.push_back(ref);
    std::vector<std::string> lines;
    collect_list_entries(group, &chunk, &lines);
    chunk.text = join(lines, "\n");
    if (trimmed(chunk.text).empty()) return;
    chunks_.push_back(std::move(chunk));
  }

  void emit_table(const std::string& ref, const docv1::TableItem& table) {
    WorkChunk chunk;
    chunk.headings = trail_texts();
    absorb(&chunk, ref);
    chunk.captions = caption_texts(table.captions(), &chunk);
    std::vector<std::string> lines = chunk.captions;
    const std::string body = options_.use_markdown_tables
                                 ? serialize_table_markdown(table.data())
                                 : serialize_table_triplets(table.data());
    if (!body.empty()) lines.push_back(body);
    chunk.text = join(lines, "\n");
    if (trimmed(chunk.text).empty()) return;
    chunks_.push_back(std::move(chunk));
  }

  void emit_picture(const std::string& ref, const docv1::PictureItem& picture) {
    WorkChunk chunk;
    chunk.headings = trail_texts();
    absorb(&chunk, ref);
    chunk.captions = caption_texts(picture.captions(), &chunk);
    chunk.text = join(chunk.captions, "\n");
    if (trimmed(chunk.text).empty()) return;
    chunks_.push_back(std::move(chunk));
  }
};

// -- materialization --------------------------------------------------------

parsev1::Chunk to_proto(const WorkChunk& work, int index, std::string_view filename,
                        const ChunkOptions& options, const std::string& digest) {
  parsev1::Chunk chunk;
  chunk.set_filename(std::string(filename));
  chunk.set_chunk_index(index);
  chunk.set_text(work.text);
  if (options.include_raw_text) chunk.set_raw_text(work.text);
  chunk.set_num_tokens(count_tokens(contextualized(work)));
  for (const auto& heading : work.headings) chunk.add_headings(heading);
  for (const auto& caption : work.captions) chunk.add_captions(caption);
  for (const auto& item : work.doc_items) chunk.add_doc_items(item);
  for (const int page : work.pages) chunk.add_page_numbers(page);
  if (work.min_confidence.has_value()) {
    (*chunk.mutable_metadata())["min_confidence"] =
        render::canonical_double(*work.min_confidence);
  }
  if (work.saw_digital || work.saw_ocr) {
    (*chunk.mutable_metadata())["text_source"] =
        work.saw_digital && work.saw_ocr ? "mixed" : (work.saw_digital ? "digital" : "ocr");
  }
  if (work.offsets_known && work.offsets_complete) {
    chunk.set_start_offset(static_cast<std::int64_t>(work.start));
    chunk.set_end_offset(static_cast<std::int64_t>(work.end));
  }
  chunk.set_rules_digest(digest);
  return chunk;
}

std::vector<parsev1::Chunk> materialize(const std::vector<WorkChunk>& work,
                                        std::string_view filename,
                                        const ChunkOptions& options,
                                        const std::string& digest) {
  std::vector<parsev1::Chunk> chunks;
  chunks.reserve(work.size());
  for (std::size_t index = 0; index < work.size(); ++index) {
    chunks.push_back(to_proto(work[index], static_cast<int>(index), filename, options, digest));
  }
  return chunks;
}

// -- hybrid passes ----------------------------------------------------------

void fold_peer(WorkChunk* into, WorkChunk&& next, std::string&& merged_text) {
  into->text = std::move(merged_text);
  for (auto& caption : next.captions) into->captions.push_back(std::move(caption));
  for (auto& item : next.doc_items) into->doc_items.push_back(std::move(item));
  into->pages.insert(next.pages.begin(), next.pages.end());
  into->offsets_complete = into->offsets_complete && next.offsets_complete;
  if (into->offsets_known && next.offsets_known) {
    into->start = std::min(into->start, next.start);
    into->end = std::max(into->end, next.end);
  } else {
    into->offsets_known = false;
  }
  into->offsets_exact = false;
  if (next.min_confidence.has_value() &&
      (!into->min_confidence.has_value() || *next.min_confidence < *into->min_confidence)) {
    into->min_confidence = next.min_confidence;
  }
  into->saw_digital = into->saw_digital || next.saw_digital;
  into->saw_ocr = into->saw_ocr || next.saw_ocr;
}

// Pass 2: consecutive chunks under the same heading trail join while their
// contextualized text still fits the budget. A different trail always
// breaks the run, whatever the budget allows.
std::vector<WorkChunk> merge_peers(std::vector<WorkChunk>&& work, int max_tokens) {
  std::vector<WorkChunk> merged;
  for (auto& chunk : work) {
    if (!merged.empty() && merged.back().headings == chunk.headings) {
      std::string candidate = merged.back().text + "\n" + chunk.text;
      WorkChunk probe;
      probe.headings = chunk.headings;
      probe.text = candidate;
      if (count_tokens(contextualized(probe)) <= max_tokens) {
        fold_peer(&merged.back(), std::move(chunk), std::move(candidate));
        continue;
      }
    }
    merged.push_back(std::move(chunk));
  }
  return merged;
}

// Greedy packing of `points` into pieces of at most `budget` tokens each:
// sentences first, then words inside a sentence that does not fit alone,
// then a hard cut inside a word that does not fit alone. The hard cut is by
// code point count, which bounds the token count because no token spans
// fewer than one code point.
std::vector<Span> pack_spans(const std::vector<char32_t>& points, int budget) {
  const char32_t* data = points.data();
  const auto tokens_in = [data](const Span& span) {
    return count_tokens(data + span.begin, data + span.end);
  };
  const auto trim = [&points](Span span) {
    while (span.begin < span.end && is_wordish_whitespace(points[span.begin])) ++span.begin;
    while (span.end > span.begin && is_wordish_whitespace(points[span.end - 1])) --span.end;
    return span;
  };

  std::vector<Span> pieces;
  const auto flush = [&pieces, &trim](std::optional<Span>& pending) {
    if (!pending.has_value()) return;
    const Span span = trim(*pending);
    if (!span.empty()) pieces.push_back(span);
    pending.reset();
  };

  std::optional<Span> pending;
  for (const Span& sentence : split_sentences(points)) {
    if (tokens_in(sentence) <= budget) {
      if (!pending.has_value()) {
        pending = sentence;
        continue;
      }
      const Span candidate{pending->begin, sentence.end};
      if (tokens_in(candidate) <= budget) {
        pending = candidate;
        continue;
      }
      flush(pending);
      pending = sentence;
      continue;
    }
    // The sentence alone is over budget: fall to words, and to a hard cut
    // for a word that is still over budget on its own.
    flush(pending);
    std::size_t index = sentence.begin;
    while (index < sentence.end) {
      while (index < sentence.end && is_wordish_whitespace(points[index])) ++index;
      if (index >= sentence.end) break;
      std::size_t word_end = index;
      while (word_end < sentence.end && !is_wordish_whitespace(points[word_end])) ++word_end;
      const Span word{index, word_end};
      index = word_end;
      if (tokens_in(word) > budget) {
        flush(pending);
        for (std::size_t cut = word.begin; cut < word.end;
             cut += static_cast<std::size_t>(budget)) {
          pieces.push_back({cut, std::min(word.end, cut + static_cast<std::size_t>(budget))});
        }
        continue;
      }
      if (!pending.has_value()) {
        pending = word;
        continue;
      }
      const Span candidate{pending->begin, word.end};
      if (tokens_in(candidate) <= budget) {
        pending = candidate;
        continue;
      }
      flush(pending);
      pending = word;
    }
  }
  flush(pending);
  return pieces;
}

// Pass 3: split what is still over budget. The pieces keep the headings and
// the doc items of the chunk they came from; their offsets narrow only when
// the chunk was exactly one offset-table entry's text.
std::vector<WorkChunk> split_oversized(std::vector<WorkChunk>&& work, int max_tokens) {
  std::vector<WorkChunk> split;
  for (auto& chunk : work) {
    if (count_tokens(contextualized(chunk)) <= max_tokens) {
      split.push_back(std::move(chunk));
      continue;
    }
    const int budget = std::max(1, max_tokens - heading_tokens(chunk));
    const std::vector<char32_t> points = decode_utf8(chunk.text);
    const std::vector<Span> pieces = pack_spans(points, budget);
    if (pieces.size() <= 1) {
      split.push_back(std::move(chunk));
      continue;
    }
    for (const Span& piece : pieces) {
      WorkChunk part = chunk;
      part.text = encode_utf8(points.data() + piece.begin, points.data() + piece.end);
      if (chunk.offsets_exact) {
        part.start = chunk.start + piece.begin;
        part.end = chunk.start + piece.end;
      } else {
        part.offsets_known = false;
      }
      split.push_back(std::move(part));
    }
  }
  return split;
}

}  // namespace

void add_offsets(const google::protobuf::RepeatedPtrField<parsev1::TextOffset>& rows,
                 OffsetTable* table) {
  if (table == nullptr) return;
  for (const auto& row : rows) {
    table->emplace(row.self_ref(), OffsetEntry{row.utf_start(), row.utf_end(), row.source()});
  }
}

std::string hybrid_rules_digest(int max_tokens, bool merge_peers) {
  return "grparse-hybrid/1;tok=" + std::string(kTokenizerRules) +
         ";sent=" + std::string(kSentenceRules) +
         ";max_tokens=" + std::to_string(max_tokens) +
         ";merge_peers=" + (merge_peers ? "true" : "false");
}

std::vector<parsev1::Chunk> chunk_hierarchical(const docv1::Document& document,
                                               const OffsetTable& offsets,
                                               const ChunkOptions& options,
                                               std::string_view filename) {
  Chunker chunker(document, offsets, options);
  return materialize(chunker.run(), filename, options, std::string(kHierarchicalRules));
}

grpc::Status validate_hybrid_options(const parsev1::HybridChunkerOptions& options) {
  if (!options.has_max_tokens()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "ChunkHybridSource requires chunking option 'max_tokens'");
  }
  if (options.max_tokens() <= 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "chunking option 'max_tokens' must be positive");
  }
  if (options.has_tokenizer() && options.tokenizer() != kTokenizerRules) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "chunking option 'tokenizer' must be one of: " +
                            std::string(kTokenizerRules));
  }
  return grpc::Status::OK;
}

std::vector<parsev1::Chunk> chunk_hybrid(const docv1::Document& document,
                                         const OffsetTable& offsets,
                                         const parsev1::HybridChunkerOptions& options,
                                         std::string_view filename) {
  const ChunkOptions serialization{options.use_markdown_tables(), options.include_raw_text()};
  const int max_tokens = options.max_tokens();
  // The wire default of an unset merge_peers is on: peers merge unless the
  // caller says otherwise.
  const bool peers = !options.has_merge_peers() || options.merge_peers();
  Chunker chunker(document, offsets, serialization);
  std::vector<WorkChunk> work = chunker.run();
  if (peers) work = merge_peers(std::move(work), max_tokens);
  work = split_oversized(std::move(work), max_tokens);
  return materialize(work, filename, serialization, hybrid_rules_digest(max_tokens, peers));
}

}  // namespace grparse::chunking
