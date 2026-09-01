#include "grparse/document_repair.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grparse/document_merge.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

constexpr std::string_view kTextsPrefix = "#/texts/";
constexpr std::string_view kFurnitureRef = "#/furniture";
constexpr std::string_view kSoftHyphen = "\xC2\xAD";
constexpr std::string_view kTerminalPunctuation = ".!?:;\"";
constexpr std::string_view kPageNumberPattern = "<page number>";

bool is_ascii_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_ascii_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ascii_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
char ascii_lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string_view trim_right(std::string_view text) {
  while (!text.empty() && is_ascii_space(text.back())) text.remove_suffix(1);
  return text;
}

std::string_view trim_left(std::string_view text) {
  while (!text.empty() && is_ascii_space(text.front())) text.remove_prefix(1);
  return text;
}

// The run of ASCII letters that ends `text`.
std::string_view trailing_word(std::string_view text) {
  size_t start = text.size();
  while (start > 0 && is_ascii_alpha(text[start - 1])) --start;
  return text.substr(start);
}

// The run of ASCII letters that starts `text`.
std::string_view leading_word(std::string_view text) {
  size_t end = 0;
  while (end < text.size() && is_ascii_alpha(text[end])) ++end;
  return text.substr(0, end);
}

// The arena index a "#/texts/N" reference names.
std::optional<int> text_index(std::string_view ref) {
  if (!ref.starts_with(kTextsPrefix)) return std::nullopt;
  const std::string_view digits = ref.substr(kTextsPrefix.size());
  if (digits.empty() || !std::ranges::all_of(digits, is_ascii_digit) || digits.size() > 9) {
    return std::nullopt;
  }
  return std::stoi(std::string(digits));
}

std::string texts_ref(int index) { return std::string(kTextsPrefix) + std::to_string(index); }

// The base of a text item that has one; CodeItem inlines its fields and
// never takes part in these repairs.
const docv1::TextItemBase* base_of(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading: return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

std::string self_ref_of(const docv1::BaseTextItem& item, int index) {
  if (item.item_case() == docv1::BaseTextItem::kCode) {
    return item.code().self_ref().empty() ? texts_ref(index) : item.code().self_ref();
  }
  const auto* base = base_of(item);
  if (base == nullptr || base->self_ref().empty()) return texts_ref(index);
  return base->self_ref();
}

// Plain prose: a TextItem carrying the TEXT or PARAGRAPH label. Captions,
// footnotes, references and the other TextItem labels are not prose, and
// neither are the other variants (headers, list items, formulas, code).
bool is_prose(const docv1::BaseTextItem& item) {
  if (item.item_case() != docv1::BaseTextItem::kText) return false;
  const auto label = item.text().base().label();
  return label == docv1::DOC_ITEM_LABEL_TEXT || label == docv1::DOC_ITEM_LABEL_PARAGRAPH;
}

// The prose base a body child names, or nullptr for anything else.
docv1::TextItemBase* prose_base(docv1::Document* document, const docv1::RefItem& child) {
  const auto index = text_index(child.ref());
  if (!index.has_value() || *index >= document->texts_size()) return nullptr;
  auto* item = document->mutable_texts(*index);
  if (!is_prose(*item)) return nullptr;
  return item->mutable_text()->mutable_base();
}

int first_page(const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) {
  int page = 0;
  for (const auto& entry : prov) {
    if (entry.page_no() > 0 && (page == 0 || entry.page_no() < page)) page = entry.page_no();
  }
  return page;
}

int last_page(const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) {
  int page = 0;
  for (const auto& entry : prov) page = std::max(page, entry.page_no());
  return page;
}

// ---------------------------------------------------------------------------
// Repair 1: running headers and footers.

enum class Band { kNone, kTop, kBottom };

// Page heights by page number: the page map's own size where it states
// one, otherwise the furthest box edge any item reaches on that page.
std::map<int, double> page_heights(const docv1::Document& document) {
  std::map<int, double> heights;
  for (const auto& [page_no, page] : document.pages()) {
    if (page.size().height() > 0) heights[page_no] = page.size().height();
  }
  std::map<int, double> extents;
  const auto note = [&extents](const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) {
    for (const auto& entry : prov) {
      if (entry.page_no() <= 0 || !entry.has_bbox()) continue;
      auto& extent = extents[entry.page_no()];
      extent = std::max({extent, entry.bbox().t(), entry.bbox().b()});
    }
  };
  for (const auto& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      note(item.code().prov());
    } else if (const auto* base = base_of(item); base != nullptr) {
      note(base->prov());
    }
  }
  for (const auto& table : document.tables()) note(table.prov());
  for (const auto& picture : document.pictures()) note(picture.prov());
  for (const auto& [page_no, extent] : extents) {
    if (!heights.contains(page_no) && extent > 0) heights[page_no] = extent;
  }
  return heights;
}

// A box's vertical extent measured downward from the page's top edge,
// whichever origin the box states; a bottom-left box measures its edges
// upward from the page's bottom.
struct VerticalSpan {
  double top = 0;
  double bottom = 0;
};

VerticalSpan vertical_span(const docv1::BoundingBox& box, double height) {
  if (box.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) {
    return {height - std::max(box.t(), box.b()), height - std::min(box.t(), box.b())};
  }
  return {std::min(box.t(), box.b()), std::max(box.t(), box.b())};
}

// The band a box sits in on a page of `height`.
Band band_of(const docv1::BoundingBox& box, double height, double fraction) {
  if (height <= 0) return Band::kNone;
  const VerticalSpan span = vertical_span(box, height);
  const double center = (span.top + span.bottom) / 2.0;
  if (center < 0 || center > height) return Band::kNone;
  if (center <= height * fraction) return Band::kTop;
  if (center >= height * (1.0 - fraction)) return Band::kBottom;
  return Band::kNone;
}

int page_count(const docv1::Document& document) {
  if (!document.pages().empty()) return static_cast<int>(document.pages().size());
  std::set<int> pages;
  for (const auto& item : document.texts()) {
    if (const auto* base = base_of(item); base != nullptr) {
      for (const auto& entry : base->prov()) {
        if (entry.page_no() > 0) pages.insert(entry.page_no());
      }
    }
  }
  return static_cast<int>(pages.size());
}

// Strict roman numeral grammar (thousands, hundreds, tens, units), all
// lowercase because the text was normalized first.
bool is_roman_numeral(std::string_view token) {
  if (token.empty()) return false;
  size_t i = 0;
  const auto take = [&](std::string_view literal) {
    if (token.substr(i).starts_with(literal)) {
      i += literal.size();
      return true;
    }
    return false;
  };
  // One decimal place: the subtractive pairs, else the five with up to
  // three ones.
  const auto place = [&](std::string_view nine, std::string_view four, std::string_view five,
                         std::string_view one) {
    if (take(nine) || take(four)) return;
    take(five);
    for (int count = 0; count < 3 && take(one); ++count) {
    }
  };
  for (int count = 0; count < 4 && take("m"); ++count) {
  }
  place("cm", "cd", "d", "c");
  place("xc", "xl", "l", "x");
  place("ix", "iv", "v", "i");
  return i == token.size();
}

constexpr std::string_view kTokenDecoration = "-|[](){}*_~.,:";

// The dressings a page number comes in, with '#' standing for the number
// (digits or a roman numeral).
constexpr std::array<std::string_view, 12> kPageNumberShapes = {
    "#",        "# #",      "# of #",    "page #",  "page # of #", "page # / #",
    "p #",      "pg #",     "seite #",   "pagina #", "# / #",      "# of # pages",
};

std::vector<std::string> page_number_tokens(std::string_view normalized) {
  std::vector<std::string> tokens;
  std::string token;
  const auto flush = [&] {
    std::string_view view(token);
    while (!view.empty() && kTokenDecoration.contains(view.front())) view.remove_prefix(1);
    while (!view.empty() && kTokenDecoration.contains(view.back())) view.remove_suffix(1);
    if (!view.empty()) tokens.emplace_back(is_roman_numeral(view) ? "#" : std::string(view));
    token.clear();
  };
  for (const char c : normalized) {
    if (c == ' ') {
      flush();
    } else if (c == '/') {
      flush();
      tokens.emplace_back("/");
    } else {
      token.push_back(c);
    }
  }
  flush();
  return tokens;
}

struct FurnitureCandidate {
  int body_index = 0;
  int arena_index = 0;
  int page = 0;
  Band band = Band::kNone;
  std::string normalized;
};

}  // namespace

std::string normalize_running_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const char c : text) {
    if (is_ascii_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    pending_space = false;
    if (is_ascii_digit(c)) {
      if (out.empty() || out.back() != '#') out.push_back('#');
      continue;
    }
    out.push_back(ascii_lower(c));
  }
  return out;
}

bool is_page_number_shape(std::string_view normalized) {
  const std::vector<std::string> tokens = page_number_tokens(normalized);
  if (tokens.empty()) return false;
  std::string joined;
  for (const auto& token : tokens) {
    if (!joined.empty()) joined.push_back(' ');
    joined += token;
  }
  return std::ranges::find(kPageNumberShapes, joined) != kPageNumberShapes.end();
}

int demote_running_furniture(docv1::Document* document, const RepairOptions& options,
                             std::vector<std::string>* patterns) {
  const int pages = page_count(*document);
  if (pages < 2) return 0;
  const std::map<int, double> heights = page_heights(*document);
  const auto* body = &document->body();

  // Decide first: every direct body child that is prose with a page and a
  // box in a band is a candidate; nothing inside a group ever is.
  std::vector<FurnitureCandidate> candidates;
  for (int index = 0; index < body->children_size(); ++index) {
    const auto* base = prose_base(document, body->children(index));
    if (base == nullptr || base->children_size() > 0) continue;
    const int page = first_page(base->prov());
    if (page <= 0) continue;
    const auto height = heights.find(page);
    if (height == heights.end()) continue;
    Band band = Band::kNone;
    for (const auto& entry : base->prov()) {
      if (entry.page_no() != page || !entry.has_bbox()) continue;
      band = band_of(entry.bbox(), height->second, options.band_fraction);
      break;
    }
    if (band == Band::kNone) continue;
    std::string normalized = normalize_running_text(base->text());
    if (normalized.empty()) continue;
    candidates.push_back({index, *text_index(body->children(index).ref()), page, band,
                          std::move(normalized)});
  }
  if (candidates.empty()) return 0;

  std::map<std::string, std::set<int>> pages_by_pattern;
  for (const auto& candidate : candidates) {
    pages_by_pattern[candidate.normalized].insert(candidate.page);
  }
  const int threshold = std::max(
      options.minimum_repeat_pages,
      static_cast<int>(std::ceil(options.minimum_repeat_share * static_cast<double>(pages))));

  std::vector<const FurnitureCandidate*> demoted;
  std::set<std::string> matched;
  bool page_numbers = false;
  for (const auto& candidate : candidates) {
    const bool recurring =
        static_cast<int>(pages_by_pattern[candidate.normalized].size()) >= threshold;
    const bool page_number = is_page_number_shape(candidate.normalized);
    if (!recurring && !page_number) continue;
    demoted.push_back(&candidate);
    if (recurring) {
      matched.insert(candidate.normalized);
    } else {
      page_numbers = true;
    }
  }
  if (demoted.empty()) return 0;

  // Then move: relabel, re-parent, and hand the references to the
  // furniture tree in page order (body order within a page).
  std::ranges::stable_sort(demoted, [](const auto* left, const auto* right) {
    return std::pair(left->page, left->body_index) < std::pair(right->page, right->body_index);
  });
  std::set<int> retired_positions;
  for (const auto* candidate : demoted) {
    auto* base = document->mutable_texts(candidate->arena_index)->mutable_text()->mutable_base();
    base->set_label(candidate->band == Band::kTop ? docv1::DOC_ITEM_LABEL_PAGE_HEADER
                                                  : docv1::DOC_ITEM_LABEL_PAGE_FOOTER);
    base->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
    base->mutable_parent()->set_ref(std::string(kFurnitureRef));
    *document->mutable_furniture()->add_children() = body->children(candidate->body_index);
    retired_positions.insert(candidate->body_index);
  }
  google::protobuf::RepeatedPtrField<docv1::RefItem> kept;
  for (int index = 0; index < body->children_size(); ++index) {
    if (!retired_positions.contains(index)) *kept.Add() = body->children(index);
  }
  document->mutable_body()->mutable_children()->Swap(&kept);

  if (patterns != nullptr) {
    for (const auto& pattern : matched) patterns->push_back(pattern);
    if (page_numbers) patterns->emplace_back(kPageNumberPattern);
  }
  return static_cast<int>(demoted.size());
}

// ---------------------------------------------------------------------------
// Repair 2: hyphenation.

namespace {

// The prefixes that stay hyphenated when a line break falls on their
// hyphen. Some only when a vowel follows: "re-enter" and "co-operate" keep
// the hyphen, "re-\nmain" and "co-\nlumn" do not.
struct HyphenatedPrefix {
  std::string_view prefix;
  bool only_before_vowel;
};

constexpr std::array<HyphenatedPrefix, 7> kHyphenatedPrefixes = {{
    {"self", false},
    {"well", false},
    {"non", false},
    {"pre", true},
    {"post", true},
    {"co", true},
    {"re", true},
}};

bool starts_with_vowel(std::string_view word) {
  return !word.empty() && std::string_view("aeiou").contains(ascii_lower(word.front()));
}

bool is_known_compound(std::string_view head, std::string_view tail) {
  std::string folded(head);
  std::ranges::transform(folded, folded.begin(), ascii_lower);
  return std::ranges::any_of(kHyphenatedPrefixes, [&](const HyphenatedPrefix& entry) {
    return folded == entry.prefix && (!entry.only_before_vowel || starts_with_vowel(tail));
  });
}

// The line break a hyphen may be followed by: a newline (either flavour)
// or the single space a line join left behind. Zero when none.
size_t break_after(std::string_view text, size_t position) {
  if (position >= text.size()) return 0;
  if (text[position] == '\n' || text[position] == ' ') return 1;
  if (text[position] == '\r' && position + 1 < text.size() && text[position + 1] == '\n') return 2;
  return 0;
}

}  // namespace

std::string join_hyphenated_fragments(std::string_view head, std::string_view tail) {
  std::string joined(head);
  if (is_known_compound(head, tail)) joined.push_back('-');
  joined += tail;
  return joined;
}

std::string rejoin_hyphenated_words(std::string_view text, HyphenationCounts* counts) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const bool soft = text.substr(i).starts_with(kSoftHyphen);
    const bool hard = text[i] == '-';
    if (!soft && !hard) {
      out.push_back(text[i++]);
      continue;
    }
    const size_t after = i + (soft ? kSoftHyphen.size() : 1);
    const size_t gap = break_after(text, after);
    const std::string_view tail = leading_word(text.substr(after + gap));
    if (soft) {
      // A discretionary hyphen is never text: it goes, and the break it
      // sat on goes with it when a word continues past it.
      if (counts != nullptr) ++counts->soft_hyphens_removed;
      i = tail.empty() ? after : after + gap;
      continue;
    }
    const std::string_view head = trailing_word(out);
    const bool joinable = gap > 0 && !head.empty() && !tail.empty() &&
                          is_ascii_lower(head.back()) && is_ascii_lower(tail.front());
    if (!joinable) {
      out.push_back('-');
      ++i;
      continue;
    }
    const std::string head_word(head);
    out.erase(out.size() - head_word.size());
    out += join_hyphenated_fragments(head_word, tail);
    i = after + gap + tail.size();
    if (counts != nullptr) ++counts->rejoined;
  }
  return out;
}

HyphenationCounts rejoin_hyphenation(docv1::Document* document) {
  HyphenationCounts counts;
  for (auto& item : *document->mutable_texts()) {
    if (!is_prose(item)) continue;
    auto* base = item.mutable_text()->mutable_base();
    if (base->text().find('-') == std::string::npos &&
        base->text().find(kSoftHyphen) == std::string::npos) {
      continue;
    }
    HyphenationCounts before = counts;
    std::string repaired = rejoin_hyphenated_words(base->text(), &counts);
    if (counts.rejoined != before.rejoined ||
        counts.soft_hyphens_removed != before.soft_hyphens_removed) {
      base->set_text(std::move(repaired));
    }
  }
  return counts;
}

// ---------------------------------------------------------------------------
// Repair 3: paragraph continuation.

namespace {

bool ends_open(std::string_view text) {
  const std::string_view trimmed = trim_right(text);
  if (trimmed.empty()) return false;
  return !kTerminalPunctuation.contains(trimmed.back());
}

bool starts_lowercase(std::string_view text) {
  const std::string_view trimmed = trim_left(text);
  return !trimmed.empty() && is_ascii_lower(trimmed.front());
}

// Where an item's text stops: its lowest box on its last page, in
// top-down page space. Absent when the item has no page, no box, or a page
// of unknown height.
struct Placement {
  int page = 0;
  double top = 0;
  double bottom = 0;
  double left = 0;
  double height = 0;
};

std::optional<Placement> placement_on(const docv1::TextItemBase& item, int page,
                                      const std::map<int, double>& heights, bool lowest) {
  const auto height = heights.find(page);
  if (page <= 0 || height == heights.end()) return std::nullopt;
  std::optional<Placement> found;
  for (const auto& entry : item.prov()) {
    if (entry.page_no() != page || !entry.has_bbox()) continue;
    const VerticalSpan span = vertical_span(entry.bbox(), height->second);
    const bool better = !found.has_value() ||
                        (lowest ? span.bottom > found->bottom : span.top < found->top);
    if (better) found = Placement{page, span.top, span.bottom, entry.bbox().l(), height->second};
  }
  return found;
}

// The fraction of a page a paragraph must have reached before a page break
// is a plausible reason for it to stop mid-sentence.
constexpr double kPageBreakDepth = 0.5;

// Whether `tail` continues `head`: an open ending, a lowercase start, and
// a layout that explains the split. Across a page break the head must have
// run into the lower half of its page; across a column break on one page
// the tail must start higher up and further right than the head stopped.
// Items with no page or box never merge: the rule exists for page and
// column breaks and has nothing to say elsewhere.
bool continues(const docv1::TextItemBase& head, const docv1::TextItemBase& tail,
               const std::map<int, double>& heights) {
  if (head.children_size() > 0 || tail.children_size() > 0) return false;
  if (!ends_open(head.text()) || !starts_lowercase(tail.text())) return false;
  const auto head_end = placement_on(head, last_page(head.prov()), heights, /*lowest=*/true);
  const auto tail_start = placement_on(tail, first_page(tail.prov()), heights, /*lowest=*/false);
  if (!head_end.has_value() || !tail_start.has_value()) return false;
  if (tail_start->page == head_end->page + 1) {
    return head_end->bottom >= head_end->height * kPageBreakDepth;
  }
  if (tail_start->page == head_end->page) {
    return tail_start->top < head_end->top && tail_start->left > head_end->left;
  }
  return false;
}

// The two texts as one: by the hyphen rule when the head ends on a
// hyphenated word, with one space otherwise.
std::string joined_text(std::string_view head, std::string_view tail) {
  const std::string_view head_trimmed = trim_right(head);
  const std::string_view tail_trimmed = trim_left(tail);
  if (head_trimmed.ends_with('-')) {
    const std::string_view head_word = trailing_word(head_trimmed.substr(0, head_trimmed.size() - 1));
    const std::string_view tail_word = leading_word(tail_trimmed);
    if (!head_word.empty() && !tail_word.empty()) {
      std::string joined(head_trimmed.substr(0, head_trimmed.size() - 1 - head_word.size()));
      joined += join_hyphenated_fragments(head_word, tail_word);
      joined += tail_trimmed.substr(tail_word.size());
      return joined;
    }
  }
  std::string joined(head_trimmed);
  joined.push_back(' ');
  joined += tail_trimmed;
  return joined;
}

void absorb(docv1::TextItemBase* head, docv1::TextItemBase* tail) {
  const std::string head_text = head->text();
  const std::string joined = joined_text(head_text, tail->text());
  // Spans measure from the item's own start; the tail's move by however
  // much text now precedes what was its first character.
  const int64_t shift = static_cast<int64_t>(joined.size()) -
                        static_cast<int64_t>(trim_left(tail->text()).size());
  head->set_text(joined);
  if (!head->orig().empty() || !tail->orig().empty()) {
    head->set_orig(joined_text(head->orig().empty() ? head_text : head->orig(),
                               tail->orig().empty() ? tail->text() : tail->orig()));
  }
  for (auto& entry : *tail->mutable_prov()) *head->add_prov() = std::move(entry);
  for (auto& span : *tail->mutable_spans()) {
    auto* range = span.mutable_range();
    range->set_start(static_cast<int32_t>(range->start() + shift));
    range->set_end(static_cast<int32_t>(range->end() + shift));
    *head->add_spans() = std::move(span);
  }
  for (auto& comment : *tail->mutable_comments()) *head->add_comments() = std::move(comment);
  for (const auto& source : tail->source()) {
    const bool known = std::ranges::any_of(head->source(), [&source](const auto& have) {
      return have.SerializeAsString() == source.SerializeAsString();
    });
    if (!known) *head->add_source() = source;
  }
  if (tail->has_meta()) {
    auto& fields = *head->mutable_meta()->mutable_custom_fields();
    for (const auto& [key, value] : tail->meta().custom_fields()) {
      if (!fields.contains(key)) fields[key] = value;
    }
  }
}

void prune_children(docv1::GroupItem* group, const std::set<std::string>& retired) {
  auto* children = group->mutable_children();
  children->erase(std::remove_if(children->begin(), children->end(),
                                 [&retired](const docv1::RefItem& child) {
                                   return retired.contains(child.ref());
                                 }),
                  children->end());
}

// Removes the retired items from the texts arena, renumbers what remains,
// and points every reference at its new name; a reference into a retired
// item follows it to the item that absorbed it.
void retire_texts(docv1::Document* document, const std::map<std::string, std::string>& absorbed_by) {
  std::set<std::string> retired;
  for (const auto& [ref, _] : absorbed_by) retired.insert(ref);
  std::map<std::string, std::string> renumbering;
  google::protobuf::RepeatedPtrField<docv1::BaseTextItem> kept;
  int next = 0;
  for (int index = 0; index < document->texts_size(); ++index) {
    auto* item = document->mutable_texts(index);
    const std::string old_ref = self_ref_of(*item, index);
    if (retired.contains(old_ref)) continue;
    const std::string new_ref = texts_ref(next++);
    if (old_ref != new_ref) renumbering[old_ref] = new_ref;
    *kept.Add() = std::move(*item);
  }
  document->mutable_texts()->Swap(&kept);
  for (const auto& [ref, survivor] : absorbed_by) {
    const auto renamed = renumbering.find(survivor);
    renumbering[ref] = renamed == renumbering.end() ? survivor : renamed->second;
  }
  prune_children(document->mutable_body(), retired);
  prune_children(document->mutable_furniture(), retired);
  for (auto& group : *document->mutable_groups()) prune_children(&group, retired);
  if (!renumbering.empty()) rewrite_references(renumbering, document);
}

}  // namespace

int merge_continuations(docv1::Document* document, const RepairOptions& options) {
  auto* body = document->mutable_body();
  const std::map<int, double> heights = page_heights(*document);
  std::map<std::string, std::string> absorbed_by;
  docv1::TextItemBase* anchor = nullptr;
  int merges = 0;
  for (int index = 0; index < body->children_size(); ++index) {
    auto* base = prose_base(document, body->children(index));
    if (base == nullptr) {
      anchor = nullptr;
      continue;
    }
    if (anchor != nullptr && merges < options.maximum_continuation_merges &&
        continues(*anchor, *base, heights)) {
      absorb(anchor, base);
      absorbed_by[base->self_ref().empty() ? body->children(index).ref() : base->self_ref()] =
          anchor->self_ref();
      ++merges;
      continue;
    }
    anchor = base;
  }
  if (merges > 0) retire_texts(document, absorbed_by);
  return merges;
}

// ---------------------------------------------------------------------------

RepairReport repair_document(docv1::Document* document, const RepairOptions& options) {
  RepairReport report;
  if (options.demote_running_furniture) {
    report.furniture_demoted =
        demote_running_furniture(document, options, &report.furniture_patterns);
  }
  if (options.merge_continuations) {
    report.paragraphs_merged = merge_continuations(document, options);
  }
  if (options.rejoin_hyphenation) {
    const HyphenationCounts counts = rejoin_hyphenation(document);
    report.hyphens_rejoined = counts.rejoined;
    report.soft_hyphens_removed = counts.soft_hyphens_removed;
  }
  return report;
}

namespace {

struct RepairCounters {
  std::atomic<uint64_t> furniture_demoted{0};
  std::atomic<uint64_t> hyphens_rejoined{0};
  std::atomic<uint64_t> paragraphs_merged{0};
};

RepairCounters& counters() {
  static RepairCounters instance;
  return instance;
}

}  // namespace

RepairReport run_repair_pass(docv1::Document* document, const RepairOptions& options) {
  RepairReport report = repair_document(document, options);
  auto& totals = counters();
  totals.furniture_demoted.fetch_add(static_cast<uint64_t>(report.furniture_demoted),
                                     std::memory_order_relaxed);
  totals.hyphens_rejoined.fetch_add(
      static_cast<uint64_t>(report.hyphens_rejoined + report.soft_hyphens_removed),
      std::memory_order_relaxed);
  totals.paragraphs_merged.fetch_add(static_cast<uint64_t>(report.paragraphs_merged),
                                     std::memory_order_relaxed);
  if (options.log_report && report.changed_anything()) {
    std::string patterns;
    for (const auto& pattern : report.furniture_patterns) {
      if (!patterns.empty()) patterns += ", ";
      patterns += '"' + pattern + '"';
    }
    std::println("gRParse repair: {} furniture demoted [{}], {} hyphens rejoined, {} soft hyphens "
                 "removed, {} paragraphs merged ({})",
                 report.furniture_demoted, patterns, report.hyphens_rejoined,
                 report.soft_hyphens_removed, report.paragraphs_merged, document->name());
  }
  return report;
}

RepairTotals repair_totals() {
  const auto& totals = counters();
  return RepairTotals{
      .furniture_demoted = totals.furniture_demoted.load(std::memory_order_relaxed),
      .hyphens_rejoined = totals.hyphens_rejoined.load(std::memory_order_relaxed),
      .paragraphs_merged = totals.paragraphs_merged.load(std::memory_order_relaxed),
  };
}

}  // namespace grparse
