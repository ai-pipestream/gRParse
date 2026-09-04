#include "grparse/consensus_page_source.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "grparse/remote_page_source.h"

namespace grparse {
namespace {

// Word stream of a page in emission order, lowercased for voting.
std::vector<std::string> page_words(const OcrPage& page) {
  std::vector<std::string> words;
  for (const auto& line : page.lines) {
    std::istringstream stream(line.text);
    std::string word;
    while (stream >> word) {
      std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      words.push_back(std::move(word));
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
// continuation, or a sentence end followed by a capital. With two
// candidates the agreement score is symmetric, so this decides.
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

class ConsensusPdfPageSource final : public PageSource {
 public:
  ConsensusPdfPageSource(std::shared_ptr<const std::string> bytes,
                         const std::vector<std::string>& targets,
                         double render_dpi) {
    std::string last_error = "no backend targets";
    for (const std::string& target : targets) {
      try {
        sources_.push_back(open_remote_pdf_document(bytes, target, render_dpi));
      } catch (const InvalidDocument& error) {
        // A backend that cannot load this document leaves the vote; the
        // others still read it.
        last_error = error.what();
        std::cerr << "consensus: dropping backend " << target << ": "
                  << error.what() << std::endl;
      }
    }
    if (sources_.empty()) throw InvalidDocument(last_error);
    pages_ = sources_.front()->page_count();
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    std::vector<OcrPage> candidates;
    for (const auto& source : sources_) {
      auto page = source->extract_digital_page(page_number);
      if (page.has_value() && !page->lines.empty()) {
        candidates.push_back(std::move(*page));
      }
    }
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() == 1) return std::move(candidates.front());

    std::vector<std::vector<std::string>> raw;
    std::vector<BigramCounts> grams;
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
      const double score =
          0.8 * agreement(grams[i], others) + 0.2 * continuity(raw[i]);
      // Ties keep the earlier target: the configured order is the priority.
      if (score > best + 1e-9) {
        best = score;
        winner = i;
      }
    }
    return std::move(candidates[winner]);
  }

  cv::Mat render_page(int page_number) const override {
    return sources_.front()->render_page(page_number);
  }

 private:
  std::vector<std::shared_ptr<PageSource>> sources_;
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
