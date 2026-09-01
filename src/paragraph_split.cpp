#include "grparse/paragraph_split.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "grparse/document_geometry.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

constexpr std::string_view kCheckboxEmpty = "\xE2\x98\x90";    // U+2610
constexpr std::string_view kCheckboxChecked = "\xE2\x98\x92";  // U+2612
constexpr size_t kMinimumCapsWords = 2;
constexpr size_t kMinimumParagraphWords = 3;
constexpr size_t kMinimumBlankRun = 3;

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

struct Token {
  size_t start = 0;
  size_t end = 0;
};

std::vector<Token> tokens_of(std::string_view text) {
  std::vector<Token> tokens;
  size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) ++index;
    if (index >= text.size()) break;
    const size_t start = index;
    while (index < text.size() && !is_space(text[index])) ++index;
    tokens.push_back({start, index});
  }
  return tokens;
}

// "3", "3.1", "4.2.1", with an optional closing period.
bool is_decimal_numbering(std::string_view token) {
  if (token.empty() || !is_digit(token.front())) return false;
  bool digit_expected = true;
  for (size_t i = 0; i < token.size(); ++i) {
    const char c = token[i];
    if (is_digit(c)) {
      digit_expected = false;
    } else if (c == '.' && !digit_expected && i + 1 < token.size()) {
      digit_expected = true;
    } else if (c == '.' && i + 1 == token.size()) {
      return true;
    } else {
      return false;
    }
  }
  return !digit_expected;
}

// Letters only, all uppercase, optionally hyphenated ("PRE-TRAINED").
bool is_caps_word(std::string_view token) {
  size_t letters = 0;
  for (const char c : token) {
    if (is_upper(c)) {
      ++letters;
    } else if (c != '-') {
      return false;
    }
  }
  return letters > 0;
}

bool is_sentence_case_word(std::string_view token) {
  return token.size() >= 2 && is_upper(token.front()) && is_lower(token[1]);
}

bool is_label_with_colon(std::string_view token) {
  if (token.size() < 3 || token.back() != ':' || !is_upper(token.front())) return false;
  return std::ranges::all_of(token.substr(1, token.size() - 2),
                             [](char c) { return is_lower(c) || is_upper(c); });
}

bool is_blank_run(std::string_view token) {
  return token.size() >= kMinimumBlankRun &&
         std::ranges::all_of(token, [](char c) { return c == '_'; });
}

bool starts_with_checkbox(std::string_view text) {
  return text.starts_with(kCheckboxEmpty) || text.starts_with(kCheckboxChecked);
}

size_t codepoints(std::string_view text) {
  size_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

bool from_collectors(const docv1::TextItemBase& base, const std::vector<std::string>& collectors) {
  if (base.source().empty()) return false;
  return std::ranges::all_of(base.source(), [&collectors](const docv1::SourceType& source) {
    return source.has_collector() &&
           std::ranges::find(collectors, source.collector().collector()) != collectors.end();
  });
}

bool is_body_prose(const docv1::BaseTextItem& item) {
  if (item.item_case() != docv1::BaseTextItem::kText) return false;
  const auto& base = item.text().base();
  if (base.content_layer() != docv1::CONTENT_LAYER_BODY || base.children_size() > 0) return false;
  return base.label() == docv1::DOC_ITEM_LABEL_TEXT || base.label() == docv1::DOC_ITEM_LABEL_PARAGRAPH;
}

// The strip of `box` covering [start_share, end_share) of its height from
// the top, in the box's own coordinate origin.
docv1::BoundingBox strip_of(const docv1::BoundingBox& box, double page_height, double start_share,
                            double end_share) {
  docv1::BoundingBox strip = box;
  if (page_height <= 0 && box.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) return strip;
  const TopDownBox flipped = top_down_box(box, page_height);
  const double top = flipped.top + flipped.height() * start_share;
  const double bottom = flipped.top + flipped.height() * end_share;
  if (box.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) {
    strip.set_t(page_height - top);
    strip.set_b(page_height - bottom);
  } else {
    strip.set_t(top);
    strip.set_b(bottom);
  }
  return strip;
}

// The children list that names `ref`, and its position there: the body,
// the furniture, or a group.
google::protobuf::RepeatedPtrField<docv1::RefItem>* children_holding(docv1::Document* document,
                                                                    const std::string& ref,
                                                                    int* position) {
  const auto find = [&](google::protobuf::RepeatedPtrField<docv1::RefItem>* children) {
    for (int index = 0; index < children->size(); ++index) {
      if (children->Get(index).ref() == ref) {
        *position = index;
        return true;
      }
    }
    return false;
  };
  if (find(document->mutable_body()->mutable_children())) {
    return document->mutable_body()->mutable_children();
  }
  if (find(document->mutable_furniture()->mutable_children())) {
    return document->mutable_furniture()->mutable_children();
  }
  for (auto& group : *document->mutable_groups()) {
    if (find(group.mutable_children())) return group.mutable_children();
  }
  return nullptr;
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

}  // namespace

std::optional<size_t> run_in_heading_end(std::string_view text) {
  const std::vector<Token> tokens = tokens_of(text);
  if (tokens.size() < 2 + kMinimumCapsWords + kMinimumParagraphWords) return std::nullopt;
  if (!is_decimal_numbering(text.substr(tokens[0].start, tokens[0].end - tokens[0].start))) {
    return std::nullopt;
  }
  size_t index = 1;
  while (index < tokens.size() &&
         is_caps_word(text.substr(tokens[index].start, tokens[index].end - tokens[index].start))) {
    ++index;
  }
  if (index - 1 < kMinimumCapsWords || index >= tokens.size()) return std::nullopt;
  const std::string_view next = text.substr(tokens[index].start, tokens[index].end - tokens[index].start);
  if (!is_sentence_case_word(next)) return std::nullopt;
  if (tokens.size() - index < kMinimumParagraphWords) return std::nullopt;
  return tokens[index].start;
}

std::vector<size_t> form_row_starts(std::string_view text) {
  std::vector<size_t> starts;
  const std::vector<Token> tokens = tokens_of(text);
  for (size_t index = 1; index < tokens.size(); ++index) {
    const std::string_view token = text.substr(tokens[index].start, tokens[index].end - tokens[index].start);
    if (starts_with_checkbox(token)) {
      starts.push_back(tokens[index].start);
      continue;
    }
    if (is_label_with_colon(token) && index + 1 < tokens.size()) {
      const auto& blank = tokens[index + 1];
      if (is_blank_run(text.substr(blank.start, blank.end - blank.start))) {
        starts.push_back(tokens[index].start);
      }
    }
  }
  return starts;
}

std::vector<std::string> split_text_item(docv1::Document* document, int arena_index,
                                         const std::vector<size_t>& offsets) {
  std::vector<std::string> created;
  if (document == nullptr || arena_index < 0 || arena_index >= document->texts_size()) return created;
  auto* original = mutable_text_base_of(document->mutable_texts(arena_index));
  if (original == nullptr) return created;
  const std::string text = original->text();
  std::vector<size_t> cuts;
  for (const size_t offset : offsets) {
    if (offset > 0 && offset < text.size() && (cuts.empty() || offset > cuts.back())) {
      cuts.push_back(offset);
    }
  }
  if (cuts.empty()) return created;
  cuts.insert(cuts.begin(), 0);
  cuts.push_back(text.size());
  const double total = static_cast<double>(std::max<size_t>(text.size(), 1));
  const std::map<int, double> heights = document_page_heights(*document);

  const std::string self_ref = original->self_ref().empty() ? "#/texts/" + std::to_string(arena_index)
                                                            : original->self_ref();
  const docv1::TextItemBase pristine = *original;
  std::vector<std::string> new_refs;
  for (size_t piece = 0; piece + 1 < cuts.size(); ++piece) {
    const std::string_view slice = trimmed(std::string_view(text).substr(cuts[piece], cuts[piece + 1] - cuts[piece]));
    const double start_share = static_cast<double>(cuts[piece]) / total;
    const double end_share = static_cast<double>(cuts[piece + 1]) / total;
    docv1::TextItemBase* target = nullptr;
    if (piece == 0) {
      target = mutable_text_base_of(document->mutable_texts(arena_index));
      target->clear_prov();
      target->clear_spans();
    } else {
      const std::string ref = "#/texts/" + std::to_string(document->texts_size());
      target = document->add_texts()->mutable_text()->mutable_base();
      target->set_self_ref(ref);
      *target->mutable_parent() = pristine.parent();
      target->set_content_layer(pristine.content_layer());
      target->set_label(pristine.label());
      for (const auto& source : pristine.source()) *target->add_source() = source;
      new_refs.push_back(ref);
    }
    target->set_text(std::string(slice));
    if (!pristine.orig().empty()) target->set_orig(std::string(slice));
    for (const auto& entry : pristine.prov()) {
      auto* strip = target->add_prov();
      strip->set_page_no(entry.page_no());
      if (entry.has_bbox()) {
        const auto height = heights.find(entry.page_no());
        *strip->mutable_bbox() =
            strip_of(entry.bbox(), height == heights.end() ? 0 : height->second, start_share, end_share);
      }
      if (entry.has_charspan()) {
        strip->mutable_charspan()->set_start(0);
        strip->mutable_charspan()->set_end(static_cast<int32_t>(codepoints(slice)));
      }
    }
    for (const auto& span : pristine.spans()) {
      const size_t start = static_cast<size_t>(std::max(0, span.range().start()));
      if (start < cuts[piece] || start >= cuts[piece + 1]) continue;
      auto* moved = target->add_spans();
      *moved = span;
      const int64_t shift = static_cast<int64_t>(cuts[piece]);
      moved->mutable_range()->set_start(static_cast<int32_t>(span.range().start() - shift));
      moved->mutable_range()->set_end(static_cast<int32_t>(
          std::min<int64_t>(span.range().end() - shift, static_cast<int64_t>(slice.size()))));
    }
  }
  int position = 0;
  if (auto* children = children_holding(document, self_ref, &position); children != nullptr) {
    for (size_t index = 0; index < new_refs.size(); ++index) {
      auto* added = children->Add();
      added->set_ref(new_refs[index]);
      // Rotate the appended entry into place right after the original and
      // the pieces already inserted.
      for (int slot = children->size() - 1; slot > position + 1 + static_cast<int>(index); --slot) {
        children->SwapElements(slot, slot - 1);
      }
    }
  }
  return new_refs;
}

int split_run_in_headings(docv1::Document* document, const std::vector<std::string>& collectors) {
  if (document == nullptr || collectors.empty()) return 0;
  int split = 0;
  const int original_count = document->texts_size();
  for (int index = 0; index < original_count; ++index) {
    auto* item = document->mutable_texts(index);
    if (!is_body_prose(*item) || !from_collectors(item->text().base(), collectors)) continue;
    const auto end = run_in_heading_end(item->text().base().text());
    if (!end.has_value()) continue;
    if (split_text_item(document, index, {*end}).empty()) continue;
    // The first piece is the heading: it changes arm, keeping everything
    // the base carried.
    docv1::TextItemBase base = std::move(*item->mutable_text()->mutable_base());
    item->clear_text();
    base.set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
    *item->mutable_section_header()->mutable_base() = std::move(base);
    ++split;
  }
  return split;
}

int split_form_rows(docv1::Document* document, const std::vector<std::string>& collectors) {
  if (document == nullptr || collectors.empty()) return 0;
  int rows = 0;
  const int original_count = document->texts_size();
  for (int index = 0; index < original_count; ++index) {
    const auto& item = document->texts(index);
    if (!is_body_prose(item) || !from_collectors(item.text().base(), collectors)) continue;
    const std::vector<size_t> starts = form_row_starts(item.text().base().text());
    if (starts.empty()) continue;
    rows += static_cast<int>(split_text_item(document, index, starts).size());
  }
  return rows;
}

}  // namespace grparse
