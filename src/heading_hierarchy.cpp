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

namespace {

// A heading below this share of the founding height of the current level
// is one level deeper; the same ratio decides that a first-page heading
// block is a title (every other heading below it) and that an unnumbered
// heading is smaller than the smallest numbered cluster.
constexpr double kDeeperBelow = 0.85;
// Lines of one title block are within this share of each other's height.
constexpr double kTitleLineHeightShare = 0.7;
constexpr int kMaximumLevel = 6;
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
void cluster_by_height(const std::vector<const HeaderHeight*>& headers,
                       std::map<std::string, int>* depths) {
  std::vector<const HeaderHeight*> sorted = headers;
  std::ranges::stable_sort(sorted, [](const HeaderHeight* a, const HeaderHeight* b) {
    return a->height > b->height;
  });
  int depth = 0;
  double founding = std::numeric_limits<double>::infinity();
  for (const HeaderHeight* header : sorted) {
    if (header->height <= 0) {
      (*depths)[header->self_ref] = 1;
      continue;
    }
    if (header->height < kDeeperBelow * founding) {
      ++depth;
      founding = header->height;
    }
    (*depths)[header->self_ref] = std::max(depth, 1);
  }
}

// The depth an unnumbered heading takes from the numbered clusters'
// median heights: the nearest one, one deeper than the deepest when it is
// visibly smaller than every cluster, depth 1 when visibly taller than all.
int depth_by_nearest_cluster(double height, const std::map<int, double>& medians) {
  if (height <= 0) return 1;
  const double largest = medians.begin()->second;
  const double smallest = medians.rbegin()->second;
  if (height > largest / kDeeperBelow) return 1;
  if (height < smallest * kDeeperBelow) return medians.rbegin()->first + 1;
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

// The tail's text and provenance folded into the head, spans shifted past
// the head's text and the joining space.
void fold_title_line(docv1::TextItemBase* head, docv1::TextItemBase* tail) {
  const std::string head_text(trim_right(head->text()));
  const std::string tail_text(trim_left(tail->text()));
  const int64_t shift = static_cast<int64_t>(head_text.size()) + 1;
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

std::vector<std::string> title_lines(const std::vector<HeaderHeight>& input) {
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
    if (heading_numbering_depth(header->text).has_value() || header->height <= 0) break;
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
    if (header.height >= kDeeperBelow * tallest) return {};
  }
  if (!others) return {};
  std::vector<std::string> lines;
  for (const HeaderHeight* header : block) lines.push_back(header->self_ref);
  return lines;
}

std::map<std::string, int32_t> infer_heading_levels(std::vector<HeaderHeight> input) {
  std::map<std::string, int32_t> levels;
  if (input.empty()) return levels;
  const std::vector<HeaderHeight> headers = normalized(std::move(input));
  const std::vector<std::string> title = title_lines(headers);
  const std::set<std::string> title_set(title.begin(), title.end());

  std::map<std::string, int> depths;
  std::map<int, std::vector<double>> cluster_heights;
  std::vector<const HeaderHeight*> unnumbered;
  for (const auto& header : headers) {
    if (title_set.contains(header.self_ref)) continue;
    std::optional<int> depth = heading_numbering_depth(header.text);
    if (!depth.has_value() && is_section_word_heading(header.text)) depth = 1;
    if (!depth.has_value()) {
      unnumbered.push_back(&header);
      continue;
    }
    depths[header.self_ref] = *depth;
    if (header.height > 0) cluster_heights[*depth].push_back(header.height);
  }
  if (cluster_heights.empty()) {
    cluster_by_height(unnumbered, &depths);
  } else {
    std::map<int, double> medians;
    for (const auto& [depth, heights] : cluster_heights) medians[depth] = median_of(heights);
    for (const HeaderHeight* header : unnumbered) {
      depths[header->self_ref] = depth_by_nearest_cluster(header->height, medians);
    }
  }
  for (const auto& header : headers) {
    if (title_set.contains(header.self_ref)) {
      levels[header.self_ref] = 1;
      continue;
    }
    const int depth = depths[header.self_ref];
    levels[header.self_ref] = std::min(kMaximumLevel, std::max(depth, 1));
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

HeadingReport infer_heading_hierarchy(docv1::Document* document, const HeadingOptions& options) {
  HeadingReport report;
  if (document == nullptr) return report;
  report.headings_demoted = demote_prose_headings(document, options);
  std::vector<Candidate> found = candidates(*document, options);
  if (found.empty()) return report;
  // A document that already has a title (a structural producer's, or this
  // pass's on an earlier run) elects no second one.
  if (!has_title_item(*document)) {
    std::vector<std::string> title = title_lines(headers_of(found));
    if (options.merge_title_lines && title.size() > 1) {
      report.titles_merged = merge_title_lines(document, found, title);
      if (report.titles_merged > 0) {
        found = candidates(*document, options);
        title = title_lines(headers_of(found));
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
  const auto levels = infer_heading_levels(headers_of(found));
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
