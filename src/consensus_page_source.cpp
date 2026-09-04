#include "grparse/consensus_page_source.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "grparse/document_assembly.h"
#include "grparse/remote_page_source.h"

namespace grparse {
namespace {

// Folds a word the way the validated prototype normalizes: ASCII lowered,
// curly quotes and long dashes straightened, soft hyphens and the
// noncharacters some engines use as hyphenation marks removed. Without
// this, ligature and hyphen artifacts depress bigram agreement.
std::string fold_word(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    const unsigned char byte = static_cast<unsigned char>(word[i]);
    uint32_t code = byte;
    size_t length = 1;
    if ((byte & 0xE0) == 0xC0 && i + 1 < word.size()) {
      code = ((byte & 0x1F) << 6) | (word[i + 1] & 0x3F);
      length = 2;
    } else if ((byte & 0xF0) == 0xE0 && i + 2 < word.size()) {
      code = ((byte & 0x0F) << 12) | ((word[i + 1] & 0x3F) << 6) |
             (word[i + 2] & 0x3F);
      length = 3;
    } else if ((byte & 0xF8) == 0xF0 && i + 3 < word.size()) {
      length = 4;
    }
    i += length;
    switch (code) {
      case 0x00AD:  // soft hyphen
      case 0xFFFE:  // noncharacter hyphenation mark
      case 0xFFFF:
        continue;
      case 0x2018:
      case 0x2019:
        out.push_back('\'');
        continue;
      case 0x201C:
      case 0x201D:
        out.push_back('"');
        continue;
      case 0x2013:
      case 0x2014:
        out.push_back('-');
        continue;
      default:
        break;
    }
    if (length == 1) {
      out.push_back(static_cast<char>(std::tolower(byte)));
    } else {
      out.append(word, i - length, length);
    }
  }
  return out;
}

// Word stream of a page in emission order, folded for voting.
std::vector<std::string> page_words(const OcrPage& page) {
  std::vector<std::string> words;
  for (const auto& line : page.lines) {
    std::istringstream stream(line.text);
    std::string word;
    while (stream >> word) {
      words.push_back(fold_word(word));
    }
  }
  return words;
}

using BigramCounts = std::unordered_map<std::string, int>;

BigramCounts bigrams(const std::vector<std::string>& words) {
  BigramCounts counts;
  for (size_t i = 0; i + 1 < words.size(); ++i) {
    counts[words[i] + '\x1f' + words[i + 1]] += 1;
  }
  return counts;
}

// Share of the candidate's adjacent pairs that another leg also emits
// adjacently, averaged over the other legs.
double agreement(const BigramCounts& candidate,
                 const std::vector<const BigramCounts*>& others) {
  int total = 0;
  for (const auto& [pair, count] : candidate) total += count;
  if (total == 0 || others.empty()) return 0.0;
  double sum = 0.0;
  for (const BigramCounts* other : others) {
    int shared = 0;
    for (const auto& [pair, count] : candidate) {
      auto it = other->find(pair);
      if (it != other->end()) shared += std::min(count, it->second);
    }
    sum += static_cast<double>(shared) / total;
  }
  return sum / static_cast<double>(others.size());
}

// Adjacent pairs that read like running text: mid-sentence lowercase
// continuation, or a sentence end followed by a capital. Contributes a
// fifth of the vote throughout; with exactly two candidates the agreement
// score is symmetric, so this is what decides.
double continuity(const std::vector<std::string>& raw_words) {
  if (raw_words.size() < 2) return 0.0;
  int smooth = 0;
  for (size_t i = 0; i + 1 < raw_words.size(); ++i) {
    const std::string& prev = raw_words[i];
    const std::string& next = raw_words[i + 1];
    if (prev.empty() || next.empty()) continue;
    const char last = prev.back();
    const bool ends_sentence = last == '.' || last == '!' || last == '?';
    const bool starts_upper = std::isupper(static_cast<unsigned char>(next.front())) != 0;
    if ((!ends_sentence && !starts_upper) || (ends_sentence && starts_upper)) {
      ++smooth;
    }
  }
  return static_cast<double>(smooth) / static_cast<double>(raw_words.size() - 1);
}

std::vector<std::string> raw_words(const OcrPage& page) {
  std::vector<std::string> words;
  for (const auto& line : page.lines) {
    std::istringstream stream(line.text);
    std::string word;
    while (stream >> word) words.push_back(std::move(word));
  }
  return words;
}

// One word entry of a page stream: the line's text, an anchor point, and
// the word's codepoint offset within the stream (words joined by single
// spaces), the coordinate system the wire links use.
struct WordEntry {
  const OcrLine* line = nullptr;
  std::string folded;
  uint64_t utf_start = 0;
  uint32_t index = 0;
};

// The page's stream split into words (so a source with line-granularity
// cells still aligns word by word), each with its codepoint offset in the
// stream and the owning line as the geometric anchor.
std::vector<WordEntry> word_entries(const OcrPage& page) {
  std::vector<WordEntry> entries;
  entries.reserve(page.lines.size());
  uint64_t offset = 0;
  for (const auto& line : page.lines) {
    std::istringstream stream(line.text);
    std::string word;
    while (stream >> word) {
      WordEntry entry;
      entry.line = &line;
      entry.folded = fold_word(word);
      entry.utf_start = offset;
      entry.index = static_cast<uint32_t>(entries.size());
      offset += utf8_codepoint_count(word) + 1;
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

double anchor_distance(const OcrLine& a, const OcrLine& b) {
  if (a.polygon.empty() || b.polygon.empty()) return 1e18;
  const double dx = a.polygon.front().x - b.polygon.front().x;
  const double dy = a.polygon.front().y - b.polygon.front().y;
  return dx * dx + dy * dy;
}

// Aligns one non-winning candidate's word stream against the winner's:
// each winner word links to the candidate word with the same text at the
// nearest anchor, or to a different word at the same place (a text
// deviation, the correction sites), or to nothing (missing).
SourceReconciliation reconcile(const OcrPage& winner, const OcrPage& candidate,
                               std::string source, double weight,
                               double tolerance_px) {
  SourceReconciliation out;
  out.source = std::move(source);
  out.weight = weight;
  const auto winner_words = word_entries(winner);
  const auto candidate_words = word_entries(candidate);
  std::unordered_map<std::string, std::vector<const WordEntry*>> by_text;
  for (const auto& entry : candidate_words) {
    by_text[entry.folded].push_back(&entry);
  }
  std::vector<bool> used(candidate_words.size(), false);
  const double tolerance_sq = tolerance_px * tolerance_px;
  std::optional<uint32_t> previous;
  out.links.reserve(winner_words.size());
  for (const auto& word : winner_words) {
    ReconciliationLink link;
    link.consensus_index = word.index;
    link.consensus_utf_start = word.utf_start;
    const WordEntry* match = nullptr;
    auto candidates = by_text.find(word.folded);
    if (candidates != by_text.end()) {
      double best = 1e18;
      for (const WordEntry* entry : candidates->second) {
        if (used[entry->index]) continue;
        const double distance = anchor_distance(*word.line, *entry->line);
        if (distance < best) {
          best = distance;
          match = entry;
        }
      }
    }
    if (match == nullptr) {
      // Same place, different characters: the nearest unused word within
      // the tolerance radius.
      double best = tolerance_sq;
      for (const auto& entry : candidate_words) {
        if (used[entry.index]) continue;
        const double distance = anchor_distance(*word.line, *entry.line);
        if (distance <= best) {
          best = distance;
          match = &entry;
        }
      }
      if (match != nullptr) {
        link.text_deviation = true;
        ++out.text_deviations;
      }
    }
    if (match == nullptr) {
      ++out.missing;
    } else {
      used[match->index] = true;
      link.source_index = match->index;
      link.source_utf_start = match->utf_start;
      ++out.matched;
      // A break is a regression in the source's order. A forward jump is
      // an insertion on the source side, not a reordering.
      if (previous.has_value() && match->index <= *previous) {
        link.order_break = true;
        ++out.order_breaks;
      }
      previous = match->index;
    }
    out.links.push_back(std::move(link));
  }
  return out;
}

// Trust thresholds for the vote's emission order. When every losing leg
// reconciles against the winner at least this completely and this
// monotonically, the winner's line order is the page's reading order and
// the fold keeps it (OcrPage::source_order_trusted) instead of re-deriving
// one geometrically. Numbers from eval/consensus/RESULTS-2026-09-04.md:
// pdf-two-column reconciles 0.998 matched with 1.6% order breaks and its
// voted order is truth-perfect (passes); pdf-form reconciles 91/91 matched
// with ~11% breaks (fails, so it falls back to geometry, which already
// scores 1.000 there).
constexpr double kTrustedMatchRateFloor = 0.95;
constexpr double kTrustedOrderBreakRateCeiling = 0.05;

bool reconciliation_supports_trust(const std::vector<SourceReconciliation>& legs) {
  return !legs.empty() && std::ranges::all_of(legs, [](const SourceReconciliation& rec) {
    const uint32_t seen = rec.matched + rec.missing;
    const double match_rate =
        seen == 0 ? 0.0 : static_cast<double>(rec.matched) / static_cast<double>(seen);
    const double break_rate = rec.matched == 0
                                  ? 1.0
                                  : static_cast<double>(rec.order_breaks) / static_cast<double>(rec.matched);
    return match_rate >= kTrustedMatchRateFloor && break_rate <= kTrustedOrderBreakRateCeiling;
  });
}

class ConsensusPdfPageSource final : public PageSource {
 public:
  ConsensusPdfPageSource(std::shared_ptr<const std::string> bytes,
                         const std::vector<std::string>& targets,
                         double render_dpi)
      : render_dpi_(render_dpi) {
    std::string last_error = "no backend targets";
    for (const std::string& target : targets) {
      try {
        sources_.push_back(
            {target, open_remote_pdf_document(bytes, target, render_dpi)});
      } catch (const InvalidDocument& error) {
        // A backend that cannot load this document leaves the vote; the
        // others still read it.
        last_error = error.what();
        std::cerr << "consensus: dropping backend " << target << ": "
                  << error.what() << std::endl;
      }
    }
    if (sources_.empty()) throw InvalidDocument(last_error);
    pages_ = sources_.front().source->page_count();
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    std::vector<OcrPage> candidates;
    std::vector<std::string> names;
    std::vector<std::string> engines;
    for (const auto& entry : sources_) {
      try {
        auto page = entry.source->extract_digital_page(page_number);
        if (page.has_value() && !page->lines.empty()) {
          candidates.push_back(std::move(*page));
          names.push_back(entry.target);
          engines.push_back(entry.source->backend_name());
        }
      } catch (const InvalidDocument& error) {
        // A backend that fails mid-document leaves this page's vote; the
        // healthy backends still read it. Consensus must never be less
        // dependable than the best configured backend.
        std::cerr << "consensus: page " << page_number << " skipped on "
                  << entry.target << ": " << error.what() << std::endl;
      }
    }
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() == 1) return std::move(candidates.front());

    std::vector<std::vector<std::string>> raw;
    std::vector<BigramCounts> grams;
    std::vector<double> scores(candidates.size(), 0.0);
    raw.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      raw.push_back(raw_words(candidate));
      grams.push_back(bigrams(page_words(candidate)));
    }
    size_t winner = 0;
    double best = -1.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
      std::vector<const BigramCounts*> others;
      for (size_t j = 0; j < grams.size(); ++j) {
        if (j != i) others.push_back(&grams[j]);
      }
      scores[i] = 0.8 * agreement(grams[i], others) + 0.2 * continuity(raw[i]);
      // Ties keep the earlier target: the configured order is the priority.
      if (scores[i] > best + 1e-9) {
        best = scores[i];
        winner = i;
      }
    }

    // The vote's correlations ride along instead of being dropped: each
    // non-winning backend's stream aligns against the winner word by word,
    // in both directions, with deviations marked. Wire slot:
    // PageData.reconciliation.
    OcrPage page = std::move(candidates[winner]);
    const double tolerance_px = 3.0 * render_dpi_ / 72.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i == winner) continue;
      page.reconciliation.push_back(reconcile(page, candidates[i], names[i],
                                              scores[i], tolerance_px));
    }
    // The winning half of the vote stays too: the winner's identity and
    // score, plus every leg that voted, so the assembled document can claim
    // the vote (the "protomolt" CollectorClaim) instead of discarding it.
    ConsensusVote vote;
    vote.winner = ConsensusVote::Leg{names[winner], engines[winner]};
    vote.winner_score = scores[winner];
    for (size_t i = 0; i < candidates.size(); ++i) {
      vote.legs.push_back(ConsensusVote::Leg{names[i], engines[i]});
    }
    page.vote = std::move(vote);
    // A clean vote (every leg nearly complete, nearly monotone) means the
    // winner's emission order is the reading order; the fold may skip its
    // geometric re-order. The early returns above never set this.
    page.source_order_trusted = reconciliation_supports_trust(page.reconciliation);
    return page;
  }

  cv::Mat render_page(int page_number) const override {
    // Raster priority is the configured order, with the same failure
    // isolation as the text path: a dead first target must not fail a page
    // another backend can render.
    std::string last_error = "no backend rendered the page";
    for (const auto& entry : sources_) {
      try {
        return entry.source->render_page(page_number);
      } catch (const InvalidDocument& error) {
        last_error = error.what();
        std::cerr << "consensus: render of page " << page_number
                  << " skipped on " << entry.target << ": " << error.what()
                  << std::endl;
      }
    }
    throw InvalidDocument(last_error);
  }

 private:
  struct Entry {
    std::string target;
    std::shared_ptr<PageSource> source;
  };
  std::vector<Entry> sources_;
  const double render_dpi_;
  int pages_ = 0;
};

}  // namespace

std::vector<std::string> split_backend_targets(const std::string& value) {
  std::vector<std::string> targets;
  std::istringstream stream(value);
  std::string target;
  while (std::getline(stream, target, ',')) {
    const auto begin = target.find_first_not_of(" \t");
    if (begin == std::string::npos) continue;
    const auto end = target.find_last_not_of(" \t");
    targets.push_back(target.substr(begin, end - begin + 1));
  }
  return targets;
}

std::shared_ptr<PageSource> open_consensus_pdf_document(
    std::shared_ptr<const std::string> bytes,
    const std::vector<std::string>& targets, double render_dpi) {
  return std::make_shared<ConsensusPdfPageSource>(std::move(bytes), targets,
                                                  render_dpi);
}

}  // namespace grparse
