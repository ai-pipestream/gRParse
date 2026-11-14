#include "grparse/code_formula_crops.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <utility>

#include <opencv2/imgcodecs.hpp>

#include "grparse/in_memory_document.h"
#include "grparse/page_previews.h"
#include "grparse/page_source_watch.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

std::string text_self_ref(const std::string& self_ref, int index) {
  return self_ref.empty() ? "#/texts/" + std::to_string(index) : self_ref;
}

// One item still to crop: the name the enrich service selects it under and
// its box.
struct PendingCrop {
  std::string self_ref;
  CodeFormulaBox box;
};

}  // namespace

std::optional<CodeFormulaBox> code_formula_box(
    const docv1::Document& document,
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov, double expansion) {
  if (prov.empty()) return std::nullopt;
  const int page_no = prov.Get(0).page_no();
  const auto page = document.pages().find(page_no);
  if (page == document.pages().end() || !page->second.has_size()) return std::nullopt;
  const double height = page->second.size().height();
  double left = std::numeric_limits<double>::max();
  double top = left;
  double right = std::numeric_limits<double>::lowest();
  double bottom = right;
  for (const docv1::ProvenanceItem& one : prov) {
    if (one.page_no() != page_no || !one.has_bbox()) continue;
    const docv1::BoundingBox& box = one.bbox();
    const bool bottom_left = box.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT;
    const double box_top = bottom_left ? height - box.t() : box.t();
    const double box_bottom = bottom_left ? height - box.b() : box.b();
    left = std::min(left, box.l());
    right = std::max(right, box.r());
    top = std::min(top, std::min(box_top, box_bottom));
    bottom = std::max(bottom, std::max(box_top, box_bottom));
  }
  if (left >= right || top >= bottom) return std::nullopt;
  const double grow_x = (right - left) * expansion;
  const double grow_y = (bottom - top) * expansion;
  return CodeFormulaBox{page_no, left - grow_x, top - grow_y, right + grow_x, bottom + grow_y};
}

cv::Rect code_formula_pixels(const CodeFormulaBox& box, double page_width, double page_height,
                             cv::Size raster) {
  if (page_width <= 0.0 || page_height <= 0.0 || raster.width <= 0 || raster.height <= 0) {
    return {};
  }
  const double scale_x = raster.width / page_width;
  const double scale_y = raster.height / page_height;
  const int left = std::clamp(static_cast<int>(std::lround(box.left * scale_x)), 0, raster.width);
  const int top = std::clamp(static_cast<int>(std::lround(box.top * scale_y)), 0, raster.height);
  const int right = std::clamp(static_cast<int>(std::lround(box.right * scale_x)), 0, raster.width);
  const int bottom =
      std::clamp(static_cast<int>(std::lround(box.bottom * scale_y)), 0, raster.height);
  if (right <= left || bottom <= top) return {};
  return {left, top, right - left, bottom - top};
}

std::vector<ChartCandidate> code_formula_crops(const docv1::Document& document, bool code,
                                               bool formula, double expansion,
                                               const CodeFormulaPageRenderer& render) {
  std::vector<ChartCandidate> crops;
  if ((!code && !formula) || !render) return crops;
  std::map<int, std::vector<PendingCrop>> by_page;
  for (int index = 0; index < document.texts_size(); ++index) {
    const docv1::BaseTextItem& item = document.texts(index);
    std::optional<CodeFormulaBox> box;
    std::string self_ref;
    if (code && item.has_code() && !item.code().has_image()) {
      box = code_formula_box(document, item.code().prov(), expansion);
      self_ref = text_self_ref(item.code().self_ref(), index);
    } else if (formula && item.has_formula()) {
      box = code_formula_box(document, item.formula().base().prov(), expansion);
      self_ref = text_self_ref(item.formula().base().self_ref(), index);
    }
    if (box.has_value()) by_page[box->page_no].push_back({std::move(self_ref), *box});
  }
  for (const auto& [page_no, pending] : by_page) {
    const cv::Mat raster = render(page_no);
    if (raster.empty()) continue;
    const docv1::Size& size = document.pages().at(page_no).size();
    for (const PendingCrop& one : pending) {
      const cv::Rect pixels = code_formula_pixels(one.box, size.width(), size.height(), raster.size());
      if (pixels.empty()) continue;
      std::vector<unsigned char> png;
      if (!cv::imencode(".png", raster(pixels), png, kPngEncodeParams)) continue;
      ChartCandidate crop;
      crop.picture_index = -1;
      crop.self_ref = one.self_ref;
      crop.mimetype = "image/png";
      crop.bytes.assign(png.begin(), png.end());
      crops.push_back(std::move(crop));
    }
  }
  return crops;
}

std::vector<ChartCandidate> render_code_formula_crops(
    const std::shared_ptr<const std::string>& bytes, const docv1::Document& document, bool code,
    bool formula, double expansion, const std::function<bool()>& stop,
    std::chrono::system_clock::time_point deadline) {
  if ((!code && !formula) || bytes == nullptr || !bytes->starts_with("%PDF-")) return {};
  std::shared_ptr<PageSource> source;
  try {
    source = open_in_memory_document(bytes, /*pdf=*/true, kCodeFormulaRenderDpi,
                                     SourceOpening::kOnFirstUse);
  } catch (const std::exception&) {
    return {};
  }
  if (source == nullptr) return {};
  const PageSourceWatch watch(source, deadline, stop);
  return code_formula_crops(document, code, formula, expansion, [&](int page_no) {
    if (stop && stop()) return cv::Mat();
    try {
      return source->render_page(page_no);
    } catch (const std::exception&) {
      return cv::Mat();
    }
  });
}

std::vector<ChartCandidate> enrich_item_crops(const ChartDerenderOptions& options,
                                              const std::shared_ptr<const std::string>& bytes,
                                              const docv1::Document& document,
                                              const std::function<bool()>& stop,
                                              std::chrono::system_clock::time_point deadline) {
  // Code and formula items go to the VLM as crops of the rendered page, the
  // way Docling hands them to its code/formula model; without one the enrich
  // service only has the item's text layer to transcribe.
  if (!options.enabled() || (!options.do_code_enrichment && !options.do_formula_enrichment)) {
    return {};
  }
  return render_code_formula_crops(bytes, document, options.do_code_enrichment,
                                   options.do_formula_enrichment,
                                   options.code_formula_expansion_factor.value_or(
                                       kCodeFormulaExpansion),
                                   stop, deadline);
}

}  // namespace grparse
