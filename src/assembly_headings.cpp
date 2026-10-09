// Section header depths for the CV path: the per-page height collection the
// streaming surface ships with its terminal event, and the whole-document
// pass the unary fold runs once every page is in (heading_hierarchy.h).
#include "grparse/document_assembly.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "grparse/heading_hierarchy.h"

namespace pipestream = ai::pipestream;

namespace grparse {

namespace {

double median_of(std::vector<double> values) {
  if (values.empty()) return 0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

// The median prov box height of one heading; one clipped or merged line
// must not drag a heading into another cluster. Zero means unusable.
double median_header_height(const pipestream::document::v1::SectionHeaderItem& header) {
  std::vector<double> heights;
  for (const auto& provenance : header.base().prov()) {
    const auto& box = provenance.bbox();
    const double height = std::abs(box.b() - box.t());
    if (height > 0) heights.push_back(height);
  }
  return median_of(std::move(heights));
}

// The median declared font size of a heading's runs, in points; zero when
// the text layer declared none.
double median_header_font(const pipestream::document::v1::SectionHeaderItem& header) {
  std::vector<double> sizes;
  for (const auto& run : header.base().spans()) {
    if (run.has_font_size_pt() && run.font_size_pt() > 0) sizes.push_back(run.font_size_pt());
  }
  return median_of(std::move(sizes));
}

}  // namespace

// The extent of a heading on its first page in the page's own top-down
// pixels: the CV path emits every box top-left, so no origin flip applies.
void place_header(const pipestream::document::v1::SectionHeaderItem& header,
                  HeaderHeight* entry) {
  int page = 0;
  for (const auto& provenance : header.base().prov()) {
    if (provenance.page_no() > 0 && (page == 0 || provenance.page_no() < page)) {
      page = provenance.page_no();
    }
  }
  entry->page = page;
  bool any = false;
  for (const auto& provenance : header.base().prov()) {
    if (provenance.page_no() != page || !provenance.has_bbox()) continue;
    const auto& box = provenance.bbox();
    const double top = std::min(box.t(), box.b());
    const double bottom = std::max(box.t(), box.b());
    entry->top = any ? std::min(entry->top, top) : top;
    entry->bottom = any ? std::max(entry->bottom, bottom) : bottom;
    any = true;
  }
}

HeaderHeight header_entry(const pipestream::document::v1::SectionHeaderItem& header) {
  HeaderHeight entry;
  entry.self_ref = header.base().self_ref();
  entry.height = median_header_height(header);
  entry.font_size = median_header_font(header);
  entry.text = header.base().text();
  place_header(header, &entry);
  return entry;
}

std::map<std::string, int32_t> section_header_levels(std::vector<HeaderHeight> headers) {
  return infer_heading_levels(std::move(headers));
}

void collect_header_heights(const pipestream::parse::v1::PageData& page,
                            std::vector<HeaderHeight>* into) {
  if (into == nullptr) throw std::invalid_argument("Header height output is required");
  for (const auto& text : page.texts()) {
    if (text.item_case() != pipestream::document::v1::BaseTextItem::kSectionHeader) continue;
    const auto& header = text.section_header();
    if (header.level() > 0) continue;  // the producer already chose
    into->push_back(header_entry(header));
  }
}

void assign_section_header_levels(pipestream::document::v1::Document* document) {
  assign_section_header_levels(document, HeadingOptions{});
}

void assign_section_header_levels(pipestream::document::v1::Document* document,
                                  const HeadingOptions& requested) {
  if (document == nullptr) throw std::invalid_argument("Document is required");
  // Only headers without a level are eligible here: the CV path's own
  // output, before any collector's levels are in play.
  HeadingOptions options = requested;
  options.geometry_collectors.clear();
  if (!options.enabled) {
    for (auto& text : *document->mutable_texts()) {
      if (!text.has_section_header() || text.section_header().level() != 0) continue;
      text.mutable_section_header()->set_level(1);
    }
    return;
  }
  infer_heading_hierarchy(document, options);
}


}  // namespace grparse
