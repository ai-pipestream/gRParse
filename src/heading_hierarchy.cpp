#include "grparse/heading_hierarchy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "grparse/document_geometry.h"
#include "grparse/document_repair.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

// The share of the founding height below which a heading is one level
// deeper (the same ratio decides that a first-page heading block is a title
// and that an unnumbered heading is smaller than the smallest numbered
// cluster) is HeadingOptions::deeper_below(), 0.85 by default.
// Lines of one title block are within this share of each other's height.
constexpr double kTitleLineHeightShare = 0.7;
// The deepest level the wire allows; options.max_level clamps within it.
constexpr int kMaximumLevel = 6;

int level_ceiling(const HeadingOptions& options) {
  return std::clamp(options.max_level, 1, kMaximumLevel);
}
// The first numbering group of a heading; longer runs are years or ids.
constexpr size_t kMaximumNumberDigits = 3;
constexpr int kMaximumRomanValue = 40;
// A letter or roman enumerator only numbers a short heading; "F. Scott
// Fitzgerald (1925). Public domain text ..." is an initial, not "F.".
constexpr int kMaximumEnumeratedHeadingWords = 8;
constexpr size_t kMaximumSectionWords = 3;
constexpr size_t kMinimumSectionLetters = 4;
// A "heading" longer than this is a paragraph.
constexpr int kMaximumHeadingWords = 30;
constexpr std::string_view kSentenceEnd = ".!?,;";
constexpr std::string_view kOpeningQuotes[] = {"\"", "'", "\xE2\x80\x9C", "\xE2\x80\x98",
                                                "\xC2\xAB"};
constexpr std::string_view kClosingQuotes[] = {"\"", "'", "\xE2\x80\x9D", "\xE2\x80\x99",
                                                "\xC2\xBB"};

int word_count(std::string_view text);

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_letter(char c) { return is_upper(c) || is_lower(c); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view trim_left(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  return text;
}

std::string_view trim_right(std::string_view text) {
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

bool has_letter(std::string_view text) { return std::ranges::any_of(text, is_letter); }

// The number of ".digits" groups following a first numbering group,
// advancing `text` past them.
int take_dotted_groups(std::string_view* text) {
  int groups = 0;
  while (text->size() >= 2 && text->front() == '.' && is_digit((*text)[1])) {
    size_t end = 1;
    while (end < text->size() && is_digit((*text)[end])) ++end;
    text->remove_prefix(end);
    ++groups;
  }
  return groups;
}

// A closing mark ("." or ")") after a numbering, when present.
void take_closer(std::string_view* text) {
  if (!text->empty() && (text->front() == '.' || text->front() == ')')) text->remove_prefix(1);
}

// The numbering must be followed by whitespace and then some text with a
// letter in it; "3" alone or "3 4" is not a heading number.
bool followed_by_words(std::string_view rest) {
  if (rest.empty() || !is_space(rest.front())) return false;
  return has_letter(trim_left(rest));
}

std::optional<int> roman_value(std::string_view token) {
  if (token.empty() || token.size() > 6) return std::nullopt;
  const auto value_of = [](char c) {
    switch (c) {
      case 'I': return 1;
      case 'V': return 5;
      case 'X': return 10;
      case 'L': return 50;
      default: return 0;
    }
  };
  int total = 0;
  for (size_t i = 0; i < token.size(); ++i) {
    const int value = value_of(token[i]);
    if (value == 0) return std::nullopt;
    const int next = i + 1 < token.size() ? value_of(token[i + 1]) : 0;
    total += value < next ? -value : value;
  }
  return total;
}

// "1", "1.2", "1.2.3", with an optional closing mark.
std::optional<int> decimal_depth(std::string_view text) {
  size_t digits = 0;
  while (digits < text.size() && is_digit(text[digits])) ++digits;
  if (digits == 0 || digits > kMaximumNumberDigits) return std::nullopt;
  std::string_view rest = text.substr(digits);
  const int depth = 1 + take_dotted_groups(&rest);
  take_closer(&rest);
  return followed_by_words(rest) ? std::optional<int>(depth) : std::nullopt;
}

// "A", "A.", "A)", "A.1": one uppercase letter, never a word.
std::optional<int> letter_depth(std::string_view text) {
  if (text.size() < 2 || !is_upper(text.front())) return std::nullopt;
  std::string_view rest = text.substr(1);
  if (is_letter(rest.front())) return std::nullopt;
  const int depth = 1 + take_dotted_groups(&rest);
  const bool closed = !rest.empty() && (rest.front() == '.' || rest.front() == ')');
  take_closer(&rest);
  // A bare letter and a space ("A short heading") reads as a word; the
  // closing mark or a sub-number is what makes it numbering.
  if (!closed && depth == 1) return std::nullopt;
  return followed_by_words(rest) ? std::optional<int>(depth) : std::nullopt;
}

// "IV", "IV.", "IV)" followed by a capitalized word.
std::optional<int> roman_depth(std::string_view text) {
  size_t end = 0;
  while (end < text.size() && is_upper(text[end])) ++end;
  const auto value = roman_value(text.substr(0, end));
  if (!value.has_value() || *value <= 0 || *value > kMaximumRomanValue) return std::nullopt;
  std::string_view rest = text.substr(end);
  const int depth = 1 + take_dotted_groups(&rest);
  const bool closed = !rest.empty() && (rest.front() == '.' || rest.front() == ')');
  take_closer(&rest);
  if (!followed_by_words(rest)) return std::nullopt;
  if (closed || depth > 1) return depth;
  // "I INTRODUCTION" numbers; "I think" does not.
  return is_upper(trim_left(rest).front()) ? std::optional<int>(depth) : std::nullopt;
}

bool starts_with_word(std::string_view text, std::string_view word) {
  if (text.size() <= word.size()) return false;
  for (size_t i = 0; i < word.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != word[i]) return false;
  }
  return is_space(text[word.size()]) || text[word.size()] == ':';
}

// "Appendix A", "APPENDIX B.2", "Appendix: proofs": depth 1 plus the
// sub-groups.
std::optional<int> appendix_depth(std::string_view text) {
  if (!starts_with_word(text, "appendix")) return std::nullopt;
  std::string_view rest = text.substr(std::string_view("appendix").size());
  if (rest.starts_with(':')) rest.remove_prefix(1);
  rest = trim_left(rest);
  if (rest.empty()) return std::nullopt;
  if (is_upper(rest.front()) && (rest.size() == 1 || !is_letter(rest[1]))) {
    rest.remove_prefix(1);
  } else if (is_digit(rest.front())) {
    size_t digits = 0;
    while (digits < rest.size() && is_digit(rest[digits])) ++digits;
    rest.remove_prefix(digits);
  } else {
    // "Appendix" followed by words is a heading, at depth 1, without a
    // label to sub-number.
    return has_letter(rest) ? std::optional<int>(1) : std::nullopt;
  }
  const int depth = 1 + take_dotted_groups(&rest);
  take_closer(&rest);
  return rest.empty() || followed_by_words(rest) ? std::optional<int>(depth) : std::nullopt;
}

// The heights headings compare by: declared font sizes when every heading
// has one, else the measured line heights.
std::vector<HeaderHeight> normalized(std::vector<HeaderHeight> headers) {
  const bool by_font = !headers.empty() &&
                       std::ranges::all_of(headers, [](const HeaderHeight& header) {
                         return header.font_size > 0;
                       });
  if (by_font) {
    for (auto& header : headers) header.height = header.font_size;
  }
  return headers;
}

double median_of(std::vector<double> values) {
  if (values.empty()) return 0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

// The legacy clustering: tallest first, a heading founds a deeper depth
// when visibly smaller than the current depth's founding height.
// Font-style rank, highest first: bold, then upright, then all-caps.
int style_rank(const HeaderHeight& header) {
  const bool has_letters = std::ranges::any_of(header.text, is_letter);
  const bool all_caps =
      has_letters && std::ranges::none_of(header.text, [](char c) { return is_lower(c); });
  return (header.bold ? 4 : 0) + (header.italic ? 0 : 2) + (all_caps ? 1 : 0);
}

// The legacy clustering: tallest first, a heading founds a deeper depth
// when visibly smaller than the current depth's founding height. With
// use_font_style, a heading of the same size but a lower style rank than
// the depth's founder founds the next depth too.
void cluster_by_height(const std::vector<const HeaderHeight*>& headers, double deeper_below,
                       bool use_font_style, std::map<std::string, int>* depths) {
  std::vector<const HeaderHeight*> sorted = headers;
  const auto ranked = [use_font_style](const HeaderHeight* header) {
    return use_font_style && header->has_font_style ? style_rank(*header) : -1;
  };
  std::ranges::stable_sort(sorted, [&](const HeaderHeight* a, const HeaderHeight* b) {
    if (a->height != b->height) return a->height > b->height;
    return ranked(a) > ranked(b);
  });
  int depth = 0;
  double founding = std::numeric_limits<double>::infinity();
  int founding_rank = -1;
  for (const HeaderHeight* header : sorted) {
    if (header->height <= 0) {
      (*depths)[header->self_ref] = 1;
      continue;
    }
    const int rank = ranked(header);
    if (header->height < deeper_below * founding) {
      ++depth;
      founding = header->height;
      founding_rank = rank;
    } else if (rank >= 0 && founding_rank >= 0 && rank < founding_rank) {
      ++depth;
      founding_rank = rank;
    }
    (*depths)[header->self_ref] = std::max(depth, 1);
  }
}

// The depth an unnumbered heading takes from the numbered clusters'
// median heights: the nearest one, one deeper than the deepest when it is
// visibly smaller than every cluster, depth 1 when visibly taller than all.
int depth_by_nearest_cluster(double height, const std::map<int, double>& medians,
                             double deeper_below) {
  if (height <= 0) return 1;
  const double largest = medians.begin()->second;
  const double smallest = medians.rbegin()->second;
  if (height > largest / deeper_below) return 1;
  if (height < smallest * deeper_below) return medians.rbegin()->first + 1;
  int best_depth = medians.begin()->first;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const auto& [depth, median] : medians) {
    const double distance = std::abs(median - height);
    if (distance < best_distance) {
      best_distance = distance;
      best_depth = depth;
    }
  }
  return best_depth;
}

// ---------------------------------------------------------------------------
// Document side.

struct Candidate {
  int arena_index = 0;
  HeaderHeight header;
};

bool from_geometry_collectors(const docv1::TextItemBase& base, const HeadingOptions& options) {
  if (base.source().empty()) return false;
  return std::ranges::all_of(base.source(), [&options](const docv1::SourceType& source) {
    return source.has_collector() &&
           std::ranges::find(options.geometry_collectors, source.collector().collector()) !=
               options.geometry_collectors.end();
  });
}

bool eligible(const docv1::SectionHeaderItem& item, const HeadingOptions& options) {
  return item.level() <= 0 || from_geometry_collectors(item.base(), options);
}

int word_count(std::string_view text) {
  int words = 0;
  bool in_word = false;
  for (const char c : text) {
    if (!is_space(c) && !in_word) ++words;
    in_word = !is_space(c);
  }
  return words;
}

bool has_title_item(const docv1::Document& document) {
  return std::ranges::any_of(document.texts(), [](const docv1::BaseTextItem& item) {
    return item.item_case() == docv1::BaseTextItem::kTitle;
  });
}

// A geometry collector's heading that reads as prose goes back to being a
// TEXT item, keeping everything its base carried.
int demote_prose_headings(docv1::Document* document, const HeadingOptions& options) {
  int demoted = 0;
  if (options.geometry_collectors.empty()) return 0;
  for (auto& item : *document->mutable_texts()) {
    if (item.item_case() != docv1::BaseTextItem::kSectionHeader) continue;
    const auto& header = item.section_header();
    if (!from_geometry_collectors(header.base(), options)) continue;
    if (!heading_reads_as_prose(header.base().text())) continue;
    docv1::TextItemBase base = std::move(*item.mutable_section_header()->mutable_base());
    item.clear_section_header();
    base.set_label(docv1::DOC_ITEM_LABEL_TEXT);
    *item.mutable_text()->mutable_base() = std::move(base);
    ++demoted;
  }
  return demoted;
}

// The title heading becomes the document's TitleItem.
void promote_to_title(docv1::BaseTextItem* item) {
  docv1::TextItemBase base = std::move(*item->mutable_section_header()->mutable_base());
  item->clear_section_header();
  base.set_label(docv1::DOC_ITEM_LABEL_TITLE);
  *item->mutable_title()->mutable_base() = std::move(base);
}

double median_line_height(const docv1::TextItemBase& base) {
  std::vector<double> heights;
  for (const auto& provenance : base.prov()) {
    const double height = std::abs(provenance.bbox().b() - provenance.bbox().t());
    if (height > 0) heights.push_back(height);
  }
  return median_of(std::move(heights));
}

double median_font_size(const docv1::TextItemBase& base) {
  std::vector<double> sizes;
  for (const auto& run : base.spans()) {
    if (run.has_font_size_pt() && run.font_size_pt() > 0) sizes.push_back(run.font_size_pt());
  }
  return median_of(std::move(sizes));
}

// A heading is bold (italic) when its item formatting says so or most of
// its styled runs do.
void fill_font_style(const docv1::TextItemBase& base, HeaderHeight* entry) {
  int styled = 0;
  int bold = 0;
  int italic = 0;
  for (const auto& span : base.spans()) {
    if (!span.has_formatting() && span.font_family().empty()) continue;
    ++styled;
    if (span.formatting().bold()) ++bold;
    if (span.formatting().italic()) ++italic;
  }
  if (base.has_formatting()) {
    entry->has_font_style = true;
    entry->bold = base.formatting().bold();
    entry->italic = base.formatting().italic();
  }
  if (styled > 0) {
    entry->has_font_style = true;
    entry->bold = entry->bold || bold * 2 > styled;
    entry->italic = entry->italic || italic * 2 > styled;
  }
}

std::vector<Candidate> candidates(const docv1::Document& document, const HeadingOptions& options) {
  const std::map<int, double> heights = document_page_heights(document);
  std::vector<Candidate> found;
  for (int index = 0; index < document.texts_size(); ++index) {
    const auto& item = document.texts(index);
    if (item.item_case() != docv1::BaseTextItem::kSectionHeader) continue;
    const auto& header = item.section_header();
    if (!eligible(header, options)) continue;
    HeaderHeight entry;
    entry.self_ref = header.base().self_ref().empty() ? "#/texts/" + std::to_string(index)
                                                      : header.base().self_ref();
    entry.height = median_line_height(header.base());
    entry.font_size = median_font_size(header.base());
    entry.text = header.base().text();
    fill_font_style(header.base(), &entry);
    if (const auto placement = provenance_placement(header.base().prov(), heights)) {
      entry.page = placement->page;
      entry.top = placement->box.top;
      entry.bottom = placement->box.bottom;
    } else {
      entry.page = first_page_of(header.base().prov());
    }
    found.push_back({index, std::move(entry)});
  }
  return found;
}

std::vector<HeaderHeight> headers_of(const std::vector<Candidate>& found) {
  std::vector<HeaderHeight> headers;
  headers.reserve(found.size());
  for (const auto& candidate : found) headers.push_back(candidate.header);
  return headers;
}

int64_t codepoint_count(std::string_view text) {
  int64_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

// The tail's text and provenance folded into the head, spans shifted past
// the head's text and the joining space, less the leading whitespace
// trimmed from the tail. Span ranges count code points.
void fold_title_line(docv1::TextItemBase* head, docv1::TextItemBase* tail) {
  const std::string head_text(trim_right(head->text()));
  const std::string tail_text(trim_left(tail->text()));
  const int64_t shift = codepoint_count(head_text) + 1 -
                        (codepoint_count(tail->text()) - codepoint_count(tail_text));
  head->set_text(head_text + " " + tail_text);
  if (!head->orig().empty() || !tail->orig().empty()) {
    const std::string head_orig(trim_right(head->orig().empty() ? head_text : head->orig()));
    const std::string tail_orig(trim_left(tail->orig().empty() ? tail_text : tail->orig()));
    head->set_orig(head_orig + " " + tail_orig);
  }
  for (auto& entry : *tail->mutable_prov()) *head->add_prov() = std::move(entry);
  for (auto& span : *tail->mutable_spans()) {
    auto* range = span.mutable_range();
    range->set_start(static_cast<int32_t>(range->start() + shift));
    range->set_end(static_cast<int32_t>(range->end() + shift));
    *head->add_spans() = std::move(span);
  }
}

// Merges the title lines that are consecutive direct body children into
// the first of them. Returns the number of lines folded away.
int merge_title_lines(docv1::Document* document, const std::vector<Candidate>& found,
                      const std::vector<std::string>& lines) {
  if (lines.size() < 2) return 0;
  std::map<std::string, int> body_position;
  for (int index = 0; index < document->body().children_size(); ++index) {
    body_position.emplace(document->body().children(index).ref(), index);
  }
  std::map<std::string, int> arena_index;
  for (const auto& candidate : found) arena_index.emplace(candidate.header.self_ref, candidate.arena_index);
  const auto head_position = body_position.find(lines.front());
  const auto head_index = arena_index.find(lines.front());
  if (head_position == body_position.end() || head_index == arena_index.end()) return 0;
  auto* head = document->mutable_texts(head_index->second)->mutable_section_header()->mutable_base();
  std::map<std::string, std::string> absorbed_by;
  int expected = head_position->second + 1;
  for (size_t line = 1; line < lines.size(); ++line) {
    const auto position = body_position.find(lines[line]);
    const auto index = arena_index.find(lines[line]);
    if (position == body_position.end() || index == arena_index.end() ||
        position->second != expected) {
      break;
    }
    fold_title_line(head, document->mutable_texts(index->second)->mutable_section_header()->mutable_base());
    absorbed_by[lines[line]] = lines.front();
    ++expected;
  }
  if (!absorbed_by.empty()) retire_text_items(document, absorbed_by);
  return static_cast<int>(absorbed_by.size());
}

}  // namespace

namespace {

bool is_roman_lower(char c) {
  return c == 'i' || c == 'v' || c == 'x' || c == 'l';
}

// A bracketed or bare enumerator token at the start of `text`: "(a)",
// "a.", "a)", "IV.", "12.3". On a match `token` is the enumerator itself
// and `rest` what follows its closing mark.
struct Enumerator {
  std::string_view token;
  std::string_view rest;
  bool closed = false;
  bool bracketed = false;
};

std::optional<Enumerator> leading_enumerator(std::string_view text) {
  Enumerator found;
  if (text.starts_with('(')) {
    const size_t close = text.find(')');
    if (close == std::string_view::npos || close < 2 || close > 8) return std::nullopt;
    found.token = text.substr(1, close - 1);
    found.rest = text.substr(close + 1);
    found.closed = true;
    found.bracketed = true;
    return found;
  }
  size_t end = 0;
  while (end < text.size() && (is_letter(text[end]) || is_digit(text[end]))) ++end;
  if (end == 0) return std::nullopt;
  found.token = text.substr(0, end);
  found.rest = text.substr(end);
  return found;
}

bool all_of_class(std::string_view token, bool (*predicate)(char)) {
  return !token.empty() && std::ranges::all_of(token, predicate);
}

bool roman_lower_token(std::string_view token) {
  if (!all_of_class(token, is_roman_lower)) return false;
  std::string upper(token);
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const auto value = roman_value(upper);
  return value.has_value() && *value > 0 && *value <= kMaximumRomanValue;
}

bool roman_upper_token(std::string_view token) {
  const auto value = roman_value(token);
  return value.has_value() && *value > 0 && *value <= kMaximumRomanValue;
}

int position_of(parsev1::HeadingNumberingScheme scheme,
                const std::vector<parsev1::HeadingNumberingScheme>& allowed) {
  const auto found = std::ranges::find(allowed, scheme);
  return found == allowed.end() ? -1 : static_cast<int>(found - allowed.begin());
}

// The keyword schemes: the word, then an arabic, roman or letter label.
std::optional<SchemeNumbering> keyword_numbering(std::string_view text) {
  struct Keyword {
    std::string_view word;
    parsev1::HeadingNumberingScheme scheme;
  };
  // SUBDIVISION before DIVISION: the longer word is matched whole.
  static constexpr Keyword kKeywords[] = {
      {"subdivision", parsev1::HEADING_NUMBERING_SCHEME_SUBDIVISION},
      {"division", parsev1::HEADING_NUMBERING_SCHEME_DIVISION},
      {"chapter", parsev1::HEADING_NUMBERING_SCHEME_CHAPTER},
      {"article", parsev1::HEADING_NUMBERING_SCHEME_ARTICLE},
      {"part", parsev1::HEADING_NUMBERING_SCHEME_PART},
  };
  for (const Keyword& keyword : kKeywords) {
    if (!starts_with_word(text, keyword.word)) continue;
    std::string_view rest = trim_left(text.substr(keyword.word.size()));
    size_t end = 0;
    while (end < rest.size() && (is_letter(rest[end]) || is_digit(rest[end]))) ++end;
    if (end == 0 || end > 6) return std::nullopt;
    const std::string_view label = rest.substr(0, end);
    const bool numeric = all_of_class(label, is_digit);
    const bool roman = roman_upper_token(label);
    const bool letter = label.size() == 1 && is_letter(label.front());
    if (!numeric && !roman && !letter) return std::nullopt;
    rest = rest.substr(end);
    SchemeNumbering numbering;
    numbering.scheme = keyword.scheme;
    numbering.sub_groups = take_dotted_groups(&rest);
    return numbering;
  }
  return std::nullopt;
}

}  // namespace

std::string_view heading_numbering_scheme_name(parsev1::HeadingNumberingScheme scheme) {
  switch (scheme) {
    case parsev1::HEADING_NUMBERING_SCHEME_PART: return "part";
    case parsev1::HEADING_NUMBERING_SCHEME_CHAPTER: return "chapter";
    case parsev1::HEADING_NUMBERING_SCHEME_DIVISION: return "division";
    case parsev1::HEADING_NUMBERING_SCHEME_SUBDIVISION: return "subdivision";
    case parsev1::HEADING_NUMBERING_SCHEME_ARTICLE: return "article";
    case parsev1::HEADING_NUMBERING_SCHEME_ROMAN_U: return "roman_u";
    case parsev1::HEADING_NUMBERING_SCHEME_ARABIC: return "arabic";
    case parsev1::HEADING_NUMBERING_SCHEME_ALPHA_U: return "alpha_u";
    case parsev1::HEADING_NUMBERING_SCHEME_ALPHA_L: return "alpha_l";
    case parsev1::HEADING_NUMBERING_SCHEME_ROMAN_L: return "roman_l";
    default: return "";
  }
}

std::optional<parsev1::HeadingNumberingScheme> heading_numbering_scheme_from_name(
    std::string_view name) {
  for (int value = parsev1::HeadingNumberingScheme_MIN + 1;
       value <= parsev1::HeadingNumberingScheme_MAX; ++value) {
    const auto scheme = static_cast<parsev1::HeadingNumberingScheme>(value);
    if (!parsev1::HeadingNumberingScheme_IsValid(value)) continue;
    if (heading_numbering_scheme_name(scheme) == name) return scheme;
  }
  return std::nullopt;
}

std::optional<SchemeNumbering> heading_scheme_numbering(
    std::string_view text, const std::vector<parsev1::HeadingNumberingScheme>& allowed) {
  const std::string_view trimmed = trim_left(text);
  if (trimmed.empty() || allowed.empty()) return std::nullopt;
  if (const auto keyword = keyword_numbering(trimmed)) {
    return position_of(keyword->scheme, allowed) >= 0 ? keyword : std::nullopt;
  }
  const auto enumerator = leading_enumerator(trimmed);
  if (!enumerator.has_value()) return std::nullopt;
  std::string_view rest = enumerator->rest;
  int sub_groups = 0;
  bool closed = enumerator->closed;
  if (!enumerator->bracketed) {
    sub_groups = take_dotted_groups(&rest);
    closed = !rest.empty() && (rest.front() == '.' || rest.front() == ')');
    take_closer(&rest);
  }
  if (!followed_by_words(rest)) return std::nullopt;
  const std::string_view token = enumerator->token;
  // The schemes this token can be, most specific reading first.
  std::vector<parsev1::HeadingNumberingScheme> readings;
  if (all_of_class(token, is_digit) && token.size() <= kMaximumNumberDigits) {
    readings.push_back(parsev1::HEADING_NUMBERING_SCHEME_ARABIC);
  }
  if (word_count(trimmed) <= kMaximumEnumeratedHeadingWords) {
    if (roman_upper_token(token) && (closed || sub_groups > 0 ||
                                     is_upper(trim_left(rest).front()))) {
      readings.push_back(parsev1::HEADING_NUMBERING_SCHEME_ROMAN_U);
    }
    if (token.size() == 1 && is_upper(token.front()) && (closed || sub_groups > 0)) {
      readings.push_back(parsev1::HEADING_NUMBERING_SCHEME_ALPHA_U);
    }
    if (roman_lower_token(token) && closed) {
      readings.push_back(parsev1::HEADING_NUMBERING_SCHEME_ROMAN_L);
    }
    if (token.size() == 1 && is_lower(token.front()) && closed) {
      readings.push_back(parsev1::HEADING_NUMBERING_SCHEME_ALPHA_L);
    }
  }
  int best = -1;
  for (const auto scheme : readings) {
    const int position = position_of(scheme, allowed);
    if (position >= 0 && (best < 0 || position < best)) best = position;
  }
  if (best < 0) return std::nullopt;
  SchemeNumbering numbering;
  numbering.scheme = allowed[best];
  numbering.sub_groups = sub_groups;
  return numbering;
}

std::optional<int> heading_scheme_depth(
    std::string_view text, const std::vector<parsev1::HeadingNumberingScheme>& schemes) {
  const auto numbering = heading_scheme_numbering(text, schemes);
  if (!numbering.has_value()) return std::nullopt;
  return position_of(numbering->scheme, schemes) + 1 + numbering->sub_groups;
}

std::optional<int> heading_numbering_depth(std::string_view text) {
  const std::string_view trimmed = trim_left(text);
  if (trimmed.empty()) return std::nullopt;
  if (const auto depth = appendix_depth(trimmed)) return depth;
  if (const auto depth = decimal_depth(trimmed)) return depth;
  if (word_count(trimmed) > kMaximumEnumeratedHeadingWords) return std::nullopt;
  if (const auto depth = roman_depth(trimmed)) return depth;
  if (const auto depth = letter_depth(trimmed)) return depth;
  return std::nullopt;
}

bool is_section_word_heading(std::string_view text) {
  const std::string_view trimmed = trim_right(trim_left(text));
  if (trimmed.empty() || heading_numbering_depth(trimmed).has_value()) return false;
  size_t words = 0;
  size_t letters = 0;
  bool in_word = false;
  for (const char c : trimmed) {
    if (is_space(c)) {
      in_word = false;
      continue;
    }
    if (!is_upper(c)) return false;
    ++letters;
    if (!in_word) ++words;
    in_word = true;
  }
  return words >= 1 && words <= kMaximumSectionWords && letters >= kMinimumSectionLetters;
}

std::vector<std::string> title_lines(const std::vector<HeaderHeight>& input,
                                     const HeadingOptions& options) {
  // A title is elected by size alone; without the style signal there is
  // no basis for one.
  if (!options.use_style) return {};
  const std::vector<HeaderHeight> headers = normalized(input);
  int first_page = 0;
  for (const auto& header : headers) {
    if (header.page > 0 && (first_page == 0 || header.page < first_page)) first_page = header.page;
  }
  if (first_page == 0) return {};
  std::vector<const HeaderHeight*> opening;
  for (const auto& header : headers) {
    if (header.page == first_page) opening.push_back(&header);
  }
  std::ranges::stable_sort(opening, [](const HeaderHeight* a, const HeaderHeight* b) {
    return a->top < b->top;
  });
  std::vector<const HeaderHeight*> block;
  for (const HeaderHeight* header : opening) {
    const bool numbered =
        options.numbering_schemes.empty()
            ? heading_numbering_depth(header->text).has_value()
            : heading_scheme_depth(header->text, options.numbering_schemes).has_value();
    if ((options.use_numbering && numbered) ||
        header->height <= 0) {
      break;
    }
    if (!block.empty()) {
      const HeaderHeight* previous = block.back();
      const double gap = header->top - previous->bottom;
      const double smaller = std::min(previous->height, header->height);
      const double larger = std::max(previous->height, header->height);
      if (gap > larger || smaller < kTitleLineHeightShare * larger) break;
    }
    block.push_back(header);
  }
  if (block.empty() || block.size() == headers.size()) return {};
  double tallest = 0;
  std::set<std::string> members;
  for (const HeaderHeight* header : block) {
    tallest = std::max(tallest, header->height);
    members.insert(header->self_ref);
  }
  bool others = false;
  for (const auto& header : headers) {
    if (members.contains(header.self_ref) || header.height <= 0) continue;
    others = true;
    if (header.height >= options.deeper_below() * tallest) return {};
  }
  if (!others) return {};
  std::vector<std::string> lines;
  for (const HeaderHeight* header : block) lines.push_back(header->self_ref);
  return lines;
}

std::map<std::string, int32_t> infer_heading_levels(std::vector<HeaderHeight> input,
                                                    const HeadingOptions& options) {
  std::map<std::string, int32_t> levels;
  if (input.empty()) return levels;
  const std::vector<HeaderHeight> headers = normalized(std::move(input));
  const std::vector<std::string> title = title_lines(headers, options);
  const std::set<std::string> title_set(title.begin(), title.end());
  const double deeper_below = options.deeper_below();

  std::map<std::string, int> depths;
  std::map<int, std::vector<double>> cluster_heights;
  std::vector<const HeaderHeight*> unnumbered;
  for (const auto& header : headers) {
    if (title_set.contains(header.self_ref)) continue;
    std::optional<int> depth;
    if (options.use_numbering && !options.numbering_schemes.empty()) {
      depth = heading_scheme_depth(header.text, options.numbering_schemes);
    } else if (options.use_numbering) {
      depth = heading_numbering_depth(header.text);
      if (!depth.has_value() && is_section_word_heading(header.text)) depth = 1;
    }
    if (!depth.has_value()) {
      unnumbered.push_back(&header);
      continue;
    }
    depths[header.self_ref] = *depth;
    if (header.height > 0) cluster_heights[*depth].push_back(header.height);
  }
  if (!options.use_style) {
    // Nothing decides an unnumbered heading's depth but its size; with the
    // style signal off it stays at the top.
    for (const HeaderHeight* header : unnumbered) depths[header->self_ref] = 1;
  } else if (cluster_heights.empty()) {
    cluster_by_height(unnumbered, deeper_below, options.use_font_style, &depths);
  } else {
    std::map<int, double> medians;
    for (const auto& [depth, heights] : cluster_heights) medians[depth] = median_of(heights);
    for (const HeaderHeight* header : unnumbered) {
      depths[header->self_ref] = depth_by_nearest_cluster(header->height, medians, deeper_below);
    }
  }
  const int ceiling = level_ceiling(options);
  for (const auto& header : headers) {
    if (title_set.contains(header.self_ref)) {
      levels[header.self_ref] = 1;
      continue;
    }
    const int depth = depths[header.self_ref];
    levels[header.self_ref] = std::min(ceiling, std::max(depth, 1));
  }
  return levels;
}

bool heading_reads_as_prose(std::string_view text) {
  const std::string_view body = trim_right(trim_left(text));
  if (body.empty()) return false;
  for (const std::string_view quote : kOpeningQuotes) {
    if (body.starts_with(quote)) return true;
  }
  for (const std::string_view quote : kClosingQuotes) {
    if (body.ends_with(quote)) return true;
  }
  if (kSentenceEnd.contains(body.back())) return true;
  return word_count(body) > kMaximumHeadingWords;
}

namespace {

std::string normalized_title(std::string_view text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || u >= 0x80) {
      if (space && !out.empty()) out.push_back(' ');
      space = false;
      out.push_back(static_cast<char>(std::tolower(u)));
    } else {
      space = true;
    }
  }
  return out;
}

}  // namespace

double heading_title_similarity(std::string_view a, std::string_view b) {
  const std::string x = normalized_title(a);
  const std::string y = normalized_title(b);
  if (x.empty() && y.empty()) return 1.0;
  if (x.empty() || y.empty()) return 0.0;
  std::vector<size_t> row(y.size() + 1);
  for (size_t j = 0; j <= y.size(); ++j) row[j] = j;
  for (size_t i = 1; i <= x.size(); ++i) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= y.size(); ++j) {
      const size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (x[i - 1] == y[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  const double longer = static_cast<double>(std::max(x.size(), y.size()));
  return 1.0 - static_cast<double>(row[y.size()]) / longer;
}

std::map<std::string, int32_t> bookmark_levels(
    const std::vector<HeaderHeight>& headers,
    const google::protobuf::RepeatedPtrField<docv1::OutlineEntry>& outline,
    const HeadingOptions& options) {
  std::map<std::string, int32_t> levels;
  if (!options.use_bookmarks || outline.empty()) return levels;
  const int ceiling = level_ceiling(options);
  for (const HeaderHeight& header : headers) {
    double best = -1.0;
    int32_t best_level = 0;
    for (const docv1::OutlineEntry& entry : outline) {
      if (entry.level() <= 0) continue;
      if (entry.has_page_no() && entry.page_no() > 0 && header.page > 0 &&
          entry.page_no() != header.page) {
        continue;
      }
      const double similarity = heading_title_similarity(header.text, entry.title());
      if (similarity > best) {
        best = similarity;
        best_level = entry.level();
      }
    }
    if (best >= options.bookmark_match_threshold && best_level > 0) {
      levels[header.self_ref] = std::min(ceiling, best_level);
    }
  }
  return levels;
}

HeadingReport infer_heading_hierarchy(docv1::Document* document, const HeadingOptions& options) {
  HeadingReport report;
  if (document == nullptr) return report;
  report.headings_demoted = demote_prose_headings(document, options);
  std::vector<Candidate> found = candidates(*document, options);
  if (found.empty()) return report;
  // A document that already has a title (a structural producer's, or this
  // pass's on an earlier run) elects no second one.
  if (!has_title_item(*document)) {
    std::vector<std::string> title = title_lines(headers_of(found), options);
    if (options.merge_title_lines && title.size() > 1) {
      report.titles_merged = merge_title_lines(document, found, title);
      if (report.titles_merged > 0) {
        found = candidates(*document, options);
        title = title_lines(headers_of(found), options);
      }
    }
    if (title.size() == 1) {
      for (const auto& candidate : found) {
        if (candidate.header.self_ref != title.front()) continue;
        promote_to_title(document->mutable_texts(candidate.arena_index));
        ++report.titles_promoted;
      }
      found = candidates(*document, options);
    }
  }
  auto levels = infer_heading_levels(headers_of(found), options);
  for (const auto& [self_ref, level] :
       bookmark_levels(headers_of(found), document->outline(), options)) {
    levels[self_ref] = level;
    ++report.levels_from_bookmarks;
  }
  for (const auto& candidate : found) {
    const auto level = levels.find(candidate.header.self_ref);
    if (level == levels.end()) continue;
    auto* header = document->mutable_texts(candidate.arena_index)->mutable_section_header();
    if (header->level() == level->second) continue;
    header->set_level(level->second);
    ++report.levels_assigned;
  }
  return report;
}

}  // namespace grparse
