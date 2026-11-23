#include "grparse/assembly_floats.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "grparse/region_geometry.h"
#include "grparse/text_geometry.h"

namespace grparse {
namespace {

constexpr double kCaptionPageReach = 0.04;
constexpr double kCaptionExpectedReach = 0.06;

double float_area(const LayoutRegion& region) {
  return static_cast<double>(std::max(0, region.right - region.left)) *
         std::max(0, region.bottom - region.top);
}

}  // namespace

const LayoutRegion* panel_label_owner(const OcrPage& page, const TextBlock& block,
                                      const LayoutRegion* region, const std::string& merged) {
  const LayoutRegion* owner = region;
  if (region != nullptr && region->label == "picture" && opens_panel_caption(merged, true)) {
    AxisAlignedBox box = bounding_box(page.lines[block.lines.front()]);
    for (const size_t line_index : block.lines) {
      const AxisAlignedBox line_box = bounding_box(page.lines[line_index]);
      box.left = std::min(box.left, line_box.left);
      box.top = std::min(box.top, line_box.top);
      box.right = std::max(box.right, line_box.right);
      box.bottom = std::max(box.bottom, line_box.bottom);
    }
    const int64_t line_height = bounding_box(page.lines[block.lines.front()]).height();
    for (const auto& panel : page.regions) {
      if (&panel == region || panel.label != "picture" || !region_nested_in(panel, *region)) continue;
      const int64_t overlap = static_cast<int64_t>(std::min(box.right, panel.right)) -
                              std::max(box.left, panel.left);
      const int64_t gap = static_cast<int64_t>(box.top) - panel.bottom;
      if (overlap * 2 < box.width() || gap < 0 || gap * 2 > line_height * 3) continue;
      if (owner == region || panel.bottom > owner->bottom) owner = &panel;
    }
  }
  return owner;
}

void link_floats(std::vector<EmittedFloat>& floats, const std::vector<EmittedCaption>& captions,
                 const std::vector<PendingPanelCaption>& pending_panel_captions, int page_height,
                 std::vector<std::string>* body_order) {
  for (const auto& pending : pending_panel_captions) {
    EmittedFloat& figure = floats[pending.figure];
    bool moved = false;
    for (auto& panel : floats) {
      if (panel.region != pending.panel) continue;
      auto* children = figure.children;
      for (int index = 0; index < children->size(); ++index) {
        if (children->Get(index).ref() == pending.self_ref) {
          children->DeleteSubrange(index, 1);
          break;
        }
      }
      pending.base->mutable_parent()->set_ref(panel.self_ref);
      panel.children->Add()->set_ref(pending.self_ref);
      panel.captions->Add()->set_ref(pending.self_ref);
      moved = true;
      break;
    }
    if (!moved) figure.captions->Add()->set_ref(pending.self_ref);
  }

  // A figure made of panels keeps them as its children: each picture inside
  // another hangs off the smallest picture around it and leaves body order.
  std::unordered_set<const LayoutRegion*> figures_with_panels;
  std::unordered_map<const LayoutRegion*, EmittedFloat*> figure_of;
  for (const auto& panel : floats) {
    if (panel.region->label != "picture") continue;
    const EmittedFloat* figure = nullptr;
    for (const auto& outer : floats) {
      if (&outer == &panel || outer.region->label != "picture") continue;
      if (!region_nested_in(*panel.region, *outer.region) ||
          float_area(*outer.region) <= float_area(*panel.region)) {
        continue;
      }
      if (figure == nullptr || float_area(*outer.region) < float_area(*figure->region)) {
        figure = &outer;
      }
    }
    if (figure == nullptr) continue;
    figures_with_panels.insert(figure->region);
    figure_of[panel.region] = const_cast<EmittedFloat*>(figure);
    panel.parent->set_ref(figure->self_ref);
    figure->children->Add()->set_ref(panel.self_ref);
    std::erase(*body_order, panel.self_ref);
  }

  // A caption binds to the nearest table or picture it visually labels: at
  // least 30% of the caption's width overlapping horizontally and a vertical
  // gap of at most 1.5 caption heights, nearest gap wins. The claimed
  // caption re-parents under the float and leaves body order; renderers then
  // emit it with its float instead of as free prose. The link is written on
  // both sides, as docling-core's add_text(parent=float) does: the caption's
  // parent names the float, and the float lists the caption among its
  // children as well as its captions. A parent that does not list its child
  // is the one-sided link the integrity check reports and the opt-in
  // repair_referenced_orphans pass exists to mend in foreign documents; a
  // document this assembler produces never needs it.
  // A figure caption sits under its figure and a table caption over its
  // table, as papers set them. Those captions bind first, each to a float
  // on its expected side; only then may one still free take a float on the
  // other side, and not one that already holds a caption of its own, so
  // "Figure 3" over a row of panels never takes the figure below that
  // "Figure 4" labels. A "Figure N" caption that lands on a panel labels
  // the panel's figure.
  struct CaptionKind {
    bool figure = false;
    bool table = false;
  };
  const auto kind_of = [](const EmittedCaption& caption) {
    const std::string_view text = caption.base->text();
    return CaptionKind{text.starts_with("Fig") || text.starts_with("FIG"),
                       text.starts_with("Table") || text.starts_with("TABLE")};
  };
  std::unordered_set<const EmittedFloat*> titled;
  const auto choose = [&](const EmittedCaption& caption, CaptionKind kind,
                          bool expected_only) -> EmittedFloat* {
    const double width = caption.region->right - caption.region->left;
    const double height = caption.region->bottom - caption.region->top;
    if (width <= 0 || height <= 0) return nullptr;
    EmittedFloat* best = nullptr;
    double best_gap = 0;
    for (auto& target : floats) {
      // A panel's own caption sits inside its figure: it labels the panel
      // above it, not the figure around it.
      const bool inside_figure = figures_with_panels.contains(target.region) &&
                                 region_nested_in(*caption.region, *target.region);
      if (inside_figure && !kind.figure) continue;
      if (!expected_only && (kind.figure || kind.table) && titled.contains(&target)) continue;
      const double overlap = std::min(caption.region->right, target.region->right) -
                             std::max(caption.region->left, target.region->left);
      if (overlap < 0.3 * width) continue;
      double gap = 0;
      const bool below_target = caption.region->top >= target.region->bottom;
      const bool above_target = caption.region->bottom <= target.region->top;
      if (below_target) {
        gap = caption.region->top - target.region->bottom;
      } else if (above_target) {
        gap = target.region->top - caption.region->bottom;
      }
      const bool picture = target.region->label == "picture";
      // "Figure 2: ..." set inside the figure's own box is that figure's.
      const bool expected = (kind.figure && picture && (below_target || inside_figure)) ||
                            (kind.table && !picture && above_target);
      if (expected_only && !expected) continue;
      // A one-line caption allows little by its own height (1.5 x 27 px at
      // 200 dpi), and papers set captions a body line or two from their
      // float: a real float also reaches 4% of the page height, 6% on the
      // side the caption labels. An icon or a logo (a licence badge) is no
      // figure and keeps the tight reach.
      const bool badge = !target.region->figure_classes.empty() &&
                         (target.region->figure_classes.front().label == "icon" ||
                          target.region->figure_classes.front().label == "logo");
      const double page_reach = expected ? kCaptionExpectedReach : kCaptionPageReach;
      const double reach = badge ? 1.5 * height : std::max(1.5 * height, page_reach * page_height);
      if (gap > reach) continue;
      // On a tie (the figure caption under a figure and its bottom panels)
      // the larger float, the whole figure, takes it.
      if (best == nullptr || gap < best_gap ||
          (gap == best_gap && float_area(*target.region) > float_area(*best->region))) {
        best = &target;
        best_gap = gap;
      }
    }
    if (best != nullptr && kind.figure) {
      for (auto figure = figure_of.find(best->region); figure != figure_of.end();
           figure = figure_of.find(best->region)) {
        best = figure->second;
      }
    }
    return best;
  };
  const auto bind = [&](const EmittedCaption& caption, EmittedFloat* target) {
    caption.base->mutable_parent()->set_ref(target->self_ref);
    target->captions->Add()->set_ref(caption.self_ref);
    target->children->Add()->set_ref(caption.self_ref);
    std::erase(*body_order, caption.self_ref);
  };
  std::vector<bool> bound(captions.size(), false);
  for (size_t index = 0; index < captions.size(); ++index) {
    const CaptionKind kind = kind_of(captions[index]);
    if (!kind.figure && !kind.table) continue;
    if (EmittedFloat* target = choose(captions[index], kind, true); target != nullptr) {
      bind(captions[index], target);
      titled.insert(target);
      bound[index] = true;
    }
  }
  for (size_t index = 0; index < captions.size(); ++index) {
    if (bound[index]) continue;
    const CaptionKind kind = kind_of(captions[index]);
    if (EmittedFloat* target = choose(captions[index], kind, false); target != nullptr) {
      bind(captions[index], target);
      if (kind.figure || kind.table) titled.insert(target);
    }
  }
}

}  // namespace grparse
