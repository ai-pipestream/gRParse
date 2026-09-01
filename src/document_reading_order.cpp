#include "grparse/document_reading_order.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "grparse/document_geometry.h"
#include "grparse/reading_order.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

// A caption binds to the nearest float it visually labels: at least this
// share of its width overlapping the float horizontally, and a vertical gap
// of at most this many caption heights. The same thresholds the CV
// assembly applies when it attaches captions on a page.
constexpr double kCaptionOverlapShare = 0.3;
constexpr double kCaptionGapHeights = 1.5;

// Items are paragraphs, not lines: the space between two paragraphs in a
// column is often wider than the gutter, so a band gap must be twice the
// gutter to split rows first, and a gutter must run along at least half
// the block on both sides.
constexpr CutPolicy kItemCutPolicy{.band_over_gutter = 2.0, .gutter_side_share = 0.5};

// A page is re-cut only when at least this share of its body items have a
// usable box; a collector that drops boxes on most paragraphs leaves no
// geometry to order by, and its own order stands.
constexpr double kMinimumPlacedShare = 0.8;

// One run of body positions that moves as a unit: a placed item and the
// unplaced items that follow it.
struct Chain {
  std::optional<ItemPlacement> placement;
  docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_UNSPECIFIED;
  std::vector<int> positions;
};

bool is_furniture_label(docv1::DocItemLabel label) {
  return label == docv1::DOC_ITEM_LABEL_PAGE_HEADER || label == docv1::DOC_ITEM_LABEL_PAGE_FOOTER;
}

// Footnotes and furniture are placed apart from the main flow, so an
// unplaced item never rides with one of them: it belongs to the main text
// before it. A caption keeps its unplaced continuation lines.
bool carries_no_followers(docv1::DocItemLabel label) {
  return is_furniture_label(label) || label == docv1::DOC_ITEM_LABEL_FOOTNOTE;
}

std::vector<Chain> build_chains(const docv1::Document& document,
                                const std::map<int, double>& heights) {
  std::vector<Chain> chains;
  std::optional<size_t> last_main;
  const auto& children = document.body().children();
  for (int position = 0; position < children.size(); ++position) {
    const std::string& ref = children[position].ref();
    const auto placement = item_placement(document, ref, heights);
    const auto label = item_label(document, ref);
    if (!placement.has_value()) {
      if (last_main.has_value()) {
        chains[*last_main].positions.push_back(position);
      } else if (!chains.empty() && !chains.back().placement.has_value()) {
        chains.back().positions.push_back(position);
      } else {
        chains.push_back(Chain{std::nullopt, label, {position}});
      }
      continue;
    }
    chains.push_back(Chain{placement, label, {position}});
    if (!carries_no_followers(label)) last_main = chains.size() - 1;
  }
  return chains;
}

// Whether enough of a page's items are placed for the cut to mean anything.
bool enough_geometry(const std::vector<Chain>& chains, const std::vector<size_t>& members) {
  size_t items = 0;
  size_t placed = 0;
  for (const size_t index : members) {
    items += chains[index].positions.size();
    if (chains[index].placement.has_value()) ++placed;
  }
  return items > 0 && static_cast<double>(placed) >= kMinimumPlacedShare * static_cast<double>(items);
}

bool is_float_label(docv1::DocItemLabel label) {
  return label == docv1::DOC_ITEM_LABEL_TABLE || label == docv1::DOC_ITEM_LABEL_PICTURE;
}

OrderBox order_box(const Chain& chain) {
  const TopDownBox& box = chain.placement->box;
  return OrderBox{box.left, box.top, box.right, box.bottom};
}

// The chains of one page in XY-cut order.
std::vector<size_t> cut_order(const std::vector<Chain>& chains, const std::vector<size_t>& members) {
  std::vector<OrderBox> boxes;
  boxes.reserve(members.size());
  for (const size_t index : members) boxes.push_back(order_box(chains[index]));
  std::vector<size_t> ordered;
  ordered.reserve(members.size());
  for (const size_t cut : xy_cut_order(boxes, kItemCutPolicy)) ordered.push_back(members[cut]);
  return ordered;
}

// The float chain a caption chain labels, when one qualifies; nearest
// vertical gap wins, the earlier float on a tie.
std::optional<size_t> caption_target(const std::vector<Chain>& chains, size_t caption,
                                     const std::vector<size_t>& floats) {
  const TopDownBox& box = chains[caption].placement->box;
  if (box.width() <= 0 || box.height() <= 0) return std::nullopt;
  std::optional<size_t> best;
  double best_gap = 0;
  for (const size_t index : floats) {
    const TopDownBox& target = chains[index].placement->box;
    const double overlap = std::min(box.right, target.right) - std::max(box.left, target.left);
    if (overlap < kCaptionOverlapShare * box.width()) continue;
    double gap = 0;
    if (box.top >= target.bottom) {
      gap = box.top - target.bottom;
    } else if (box.bottom <= target.top) {
      gap = target.top - box.bottom;
    }
    if (gap > kCaptionGapHeights * box.height()) continue;
    if (!best.has_value() || gap < best_gap) {
      best = index;
      best_gap = gap;
    }
  }
  return best;
}

// One page's chains in reading order: main text by the cut with each bound
// caption following its float, then footnotes, then furniture.
std::vector<size_t> order_page(const std::vector<Chain>& chains, const std::vector<size_t>& members) {
  std::vector<size_t> main;
  std::vector<size_t> floats;
  std::vector<size_t> captions;
  std::vector<size_t> footnotes;
  std::vector<size_t> furniture;
  for (const size_t index : members) {
    const auto label = chains[index].label;
    if (is_furniture_label(label)) {
      furniture.push_back(index);
    } else if (label == docv1::DOC_ITEM_LABEL_FOOTNOTE) {
      footnotes.push_back(index);
    } else if (label == docv1::DOC_ITEM_LABEL_CAPTION) {
      captions.push_back(index);
    } else {
      main.push_back(index);
      if (is_float_label(label)) floats.push_back(index);
    }
  }
  std::map<size_t, std::vector<size_t>> bound;
  for (const size_t caption : captions) {
    if (const auto target = caption_target(chains, caption, floats); target.has_value()) {
      bound[*target].push_back(caption);
    } else {
      main.push_back(caption);
    }
  }
  std::vector<size_t> ordered;
  ordered.reserve(members.size());
  for (const size_t index : cut_order(chains, main)) {
    ordered.push_back(index);
    if (const auto followers = bound.find(index); followers != bound.end()) {
      ordered.insert(ordered.end(), followers->second.begin(), followers->second.end());
    }
  }
  for (const size_t index : cut_order(chains, footnotes)) ordered.push_back(index);
  for (const size_t index : cut_order(chains, furniture)) ordered.push_back(index);
  return ordered;
}

struct Mover {
  std::string ref;
  docv1::RefItem child;
  std::optional<ItemPlacement> placement;
};

bool before(const Mover& a, const Mover& b) {
  const int page_a = a.placement ? a.placement->page : std::numeric_limits<int>::max();
  const int page_b = b.placement ? b.placement->page : std::numeric_limits<int>::max();
  if (page_a != page_b) return page_a < page_b;
  if (a.placement && b.placement) {
    if (a.placement->box.top != b.placement->box.top) {
      return a.placement->box.top < b.placement->box.top;
    }
    if (a.placement->box.left != b.placement->box.left) {
      return a.placement->box.left < b.placement->box.left;
    }
  }
  return a.ref < b.ref;
}

}  // namespace

std::vector<int> body_reading_order(const docv1::Document& document) {
  const std::map<int, double> heights = document_page_heights(document);
  const std::vector<Chain> chains = build_chains(document, heights);
  // Unplaced leading chains have no page and come first, as page 0.
  std::map<int, std::vector<size_t>> pages;
  for (size_t index = 0; index < chains.size(); ++index) {
    pages[chains[index].placement ? chains[index].placement->page : 0].push_back(index);
  }
  std::vector<int> order;
  order.reserve(static_cast<size_t>(document.body().children_size()));
  for (const auto& [page, members] : pages) {
    const bool cut = page != 0 && enough_geometry(chains, members);
    const std::vector<size_t> ordered = cut ? order_page(chains, members) : members;
    for (const size_t index : ordered) {
      order.insert(order.end(), chains[index].positions.begin(), chains[index].positions.end());
    }
  }
  return order;
}

BodyOrderReport order_body_by_geometry(docv1::Document* document, const BodyOrderOptions& options) {
  BodyOrderReport report;
  if (document == nullptr || !produced_only_by(*document, options.geometry_collectors)) {
    return report;
  }
  const std::vector<int> order = body_reading_order(*document);
  const auto& children = document->body().children();
  if (static_cast<int>(order.size()) != children.size()) return report;
  std::set<int> pages_changed;
  const std::map<int, double> heights = document_page_heights(*document);
  google::protobuf::RepeatedPtrField<docv1::RefItem> reordered;
  reordered.Reserve(children.size());
  for (size_t index = 0; index < order.size(); ++index) {
    const int position = order[index];
    if (position != static_cast<int>(index)) {
      ++report.items_moved;
      if (const auto placement = item_placement(*document, children[position].ref(), heights)) {
        pages_changed.insert(placement->page);
      }
    }
    *reordered.Add() = children[position];
  }
  report.pages_reordered = static_cast<int>(pages_changed.size());
  if (report.items_moved > 0) document->mutable_body()->mutable_children()->Swap(&reordered);
  return report;
}

PictureAnchorReport anchor_pictures_by_provenance(docv1::Document* document,
                                                  const std::vector<std::string>& picture_refs) {
  PictureAnchorReport report;
  if (document == nullptr || picture_refs.empty()) return report;
  const std::set<std::string> moving(picture_refs.begin(), picture_refs.end());
  const std::map<int, double> heights = document_page_heights(*document);

  // The body without the movers, each kept item with its placement.
  struct Kept {
    docv1::RefItem child;
    std::optional<ItemPlacement> placement;
  };
  std::vector<Kept> kept;
  std::vector<Mover> movers;
  for (const auto& child : document->body().children()) {
    if (moving.contains(child.ref())) {
      movers.push_back({child.ref(), child, item_placement(*document, child.ref(), heights)});
    } else {
      kept.push_back({child, item_placement(*document, child.ref(), heights)});
    }
  }
  if (movers.empty()) return report;
  std::ranges::sort(movers, before);

  // Insertion slots are positions in `kept` decided against the original
  // body alone, so two pictures anchored before the same item keep their
  // sorted order and neither sees the other.
  std::map<size_t, std::vector<const Mover*>> slots;
  std::vector<const Mover*> trailing;
  for (const Mover& mover : movers) {
    if (!mover.placement.has_value()) {
      trailing.push_back(&mover);
      ++report.appended;
      continue;
    }
    const int page = mover.placement->page;
    std::optional<size_t> slot;
    std::optional<size_t> last_on_page;
    std::optional<size_t> first_after_page;
    std::optional<size_t> beside;
    for (size_t index = 0; index < kept.size(); ++index) {
      const auto& placement = kept[index].placement;
      if (!placement.has_value()) continue;
      if (placement->page == page) {
        last_on_page = index;
        const TopDownBox& box = placement->box;
        const TopDownBox& mine = mover.placement->box;
        if (!beside.has_value() && std::min(box.bottom, mine.bottom) > std::max(box.top, mine.top)) {
          beside = index;
        }
        if (!slot.has_value() && box.top >= mine.top) slot = index;
      } else if (placement->page > page && !first_after_page.has_value()) {
        first_after_page = index;
      }
    }
    if (beside.has_value()) slot = *beside + 1;
    if (!slot.has_value()) {
      if (last_on_page.has_value()) {
        slot = *last_on_page + 1;
      } else if (first_after_page.has_value()) {
        slot = *first_after_page;
      } else {
        slot = kept.size();
      }
    }
    slots[*slot].push_back(&mover);
    ++report.anchored;
  }

  google::protobuf::RepeatedPtrField<docv1::RefItem> rebuilt;
  rebuilt.Reserve(document->body().children_size());
  const auto emit_slot = [&](size_t index) {
    if (const auto found = slots.find(index); found != slots.end()) {
      for (const Mover* mover : found->second) *rebuilt.Add() = mover->child;
    }
  };
  for (size_t index = 0; index < kept.size(); ++index) {
    emit_slot(index);
    *rebuilt.Add() = kept[index].child;
  }
  emit_slot(kept.size());
  for (const Mover* mover : trailing) *rebuilt.Add() = mover->child;
  document->mutable_body()->mutable_children()->Swap(&rebuilt);
  return report;
}

}  // namespace grparse
