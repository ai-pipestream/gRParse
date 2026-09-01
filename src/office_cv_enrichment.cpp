#include "grparse/office_cv_enrichment.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/barcode_decoder.h"
#include "grparse/base64.h"
#include "grparse/document_geometry.h"
#include "grparse/document_reading_order.h"
#include "grparse/region_geometry.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

constexpr const char kPngDataUriPrefix[] = "data:image/png;base64,";

// The mapped page image back as pixels.  Empty when the page carries no
// data-URI image or the bytes do not decode.
cv::Mat decode_page_image(const docv1::PageItem& page) {
  const std::string& uri = page.image().uri();
  const size_t prefix = sizeof(kPngDataUriPrefix) - 1;
  if (!uri.starts_with(kPngDataUriPrefix)) return {};
  std::string bytes;
  try {
    bytes = decode_base64(uri.substr(prefix));
  } catch (const std::invalid_argument&) {
    return {};
  }
  const cv::Mat buffer(1, static_cast<int>(bytes.size()), CV_8UC1,
                       const_cast<char*>(bytes.data()));
  return cv::imdecode(buffer, cv::IMREAD_COLOR);
}

bool barcode_class(const LayoutRegion& region) {
  if (region.figure_classes.empty()) return false;
  const std::string& top = region.figure_classes.front().label;
  return top == "bar_code" || top == "qr_code";
}

void add_collector_source(const std::string& model, std::optional<float> confidence,
                          google::protobuf::RepeatedPtrField<docv1::SourceType>* source) {
  auto* collector = source->Add()->mutable_collector();
  collector->set_collector("grparse");
  collector->set_model(model);
  if (confidence.has_value()) collector->set_confidence(*confidence);
}

struct OfficeCvCounters {
  std::atomic<uint64_t> pictures_added{0};
  std::atomic<uint64_t> pictures_anchored{0};
};

OfficeCvCounters& counters() {
  static OfficeCvCounters instance;
  return instance;
}

// Two picture boxes cover the same place when their overlap is at least
// half of their union or either holds the other's center.
constexpr double kDuplicateIou = 0.5;

bool same_place(const TopDownBox& a, const TopDownBox& b) {
  const double left = std::max(a.left, b.left);
  const double top = std::max(a.top, b.top);
  const double right = std::min(a.right, b.right);
  const double bottom = std::min(a.bottom, b.bottom);
  const double intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
  const auto contains_center = [](const TopDownBox& outer, const TopDownBox& inner) {
    const double x = (inner.left + inner.right) / 2;
    const double y = (inner.top + inner.bottom) / 2;
    return x >= outer.left && x <= outer.right && y >= outer.top && y <= outer.bottom;
  };
  if (intersection > 0 && (contains_center(a, b) || contains_center(b, a))) return true;
  const double union_area = a.width() * a.height() + b.width() * b.height() - intersection;
  return union_area > 0 && intersection / union_area >= kDuplicateIou;
}

// The boxes of the pictures the document already places on `page`, in the
// page's own top-down space.
std::vector<TopDownBox> existing_picture_boxes(const docv1::Document& document, int page,
                                               double page_height) {
  std::vector<TopDownBox> boxes;
  for (const auto& picture : document.pictures()) {
    for (const auto& entry : picture.prov()) {
      if (entry.page_no() != page || !entry.has_bbox()) continue;
      boxes.push_back(top_down_box(entry.bbox(), page_height));
    }
  }
  return boxes;
}

// Detections in page order: top edge, then left, then the far edges, so
// two detectors reporting the same boxes in a different order add the same
// pictures arena.
bool region_before(const LayoutRegion& a, const LayoutRegion& b) {
  if (a.top != b.top) return a.top < b.top;
  if (a.left != b.left) return a.left < b.left;
  if (a.bottom != b.bottom) return a.bottom < b.bottom;
  return a.right < b.right;
}

// One detected picture as a PictureItem, box scaled from render pixels into
// the page's own coordinate space and linked into the body like every other
// mapped item. Returns its reference.
std::string append_figure(const LayoutRegion& region, int page_number, double scale,
                          const std::string& layout_model, docv1::Document* document) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  docv1::PictureItem* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref("#/body");
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  document->mutable_body()->add_children()->set_ref(ref);

  docv1::ProvenanceItem* provenance = picture->add_prov();
  provenance->set_page_no(page_number);
  docv1::BoundingBox* box = provenance->mutable_bbox();
  box->set_l(region.left * scale);
  box->set_t(region.top * scale);
  box->set_r(region.right * scale);
  box->set_b(region.bottom * scale);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  add_collector_source(layout_model, region.confidence, picture->mutable_source());

  if (!region.figure_classes.empty()) {
    auto* classification = picture->add_annotations()->mutable_classification();
    classification->set_kind("classification");
    classification->set_provenance("figure-classifier");
    for (const auto& figure_class : region.figure_classes) {
      auto* predicted = classification->add_predicted_classes();
      predicted->set_class_name(figure_class.label);
      predicted->set_confidence(figure_class.confidence);
    }
  }
  for (const auto& barcode : region.barcodes) {
    auto* misc = picture->add_annotations()->mutable_misc();
    misc->set_kind("barcode");
    auto& fields = *misc->mutable_content()->mutable_fields();
    fields["format"].set_string_value(barcode.format);
    fields["value"].set_string_value(barcode.text);
    fields["provenance"].set_string_value("zxing-cpp");
  }
  return ref;
}

}  // namespace

OfficeCvReport enrich_office_document(const OfficeCvEnrichment& enrichment,
                                      docv1::Document* document) {
  OfficeCvReport report;
  if (enrichment.detector == nullptr || document == nullptr) return report;
  // The page map is a hash map; walking it directly numbers the pictures
  // arena in whatever order the map happens to hold, which differs run to
  // run. Page order is the only order a document has.
  std::vector<int> page_numbers;
  page_numbers.reserve(document->pages().size());
  for (const auto& [page_number, _] : document->pages()) page_numbers.push_back(page_number);
  std::ranges::sort(page_numbers);

  std::vector<std::string> added;
  for (const int page_number : page_numbers) {
    const docv1::PageItem& page = document->pages().at(page_number);
    const cv::Mat raster = decode_page_image(page);
    if (raster.empty()) continue;
    // The page's coordinate space (twips for office documents) and the
    // render's pixel space share only their aspect ratio; every typed item's
    // provenance is in the former, so detected boxes convert on the way in.
    if (page.size().width() <= 0 || raster.cols <= 0) continue;
    const double scale = page.size().width() / static_cast<double>(raster.cols);

    std::vector<LayoutRegion> regions = enrichment.detector->detect_regions(raster);
    std::erase_if(regions, [](const LayoutRegion& region) { return region.label != "picture"; });
    std::ranges::stable_sort(regions, region_before);
    const std::string layout_model = enrichment.detector->model_name();
    std::vector<TopDownBox> taken = existing_picture_boxes(*document, page_number, page.size().height());
    for (auto& region : regions) {
      const TopDownBox box{region.left * scale, region.top * scale, region.right * scale,
                           region.bottom * scale};
      if (std::ranges::any_of(taken, [&box](const TopDownBox& have) { return same_place(have, box); })) {
        ++report.pictures_deduplicated;
        continue;
      }
      taken.push_back(box);
      const cv::Mat crop = crop_region(raster, region);
      if (crop.empty()) continue;
      if (enrichment.classifier != nullptr) {
        region.figure_classes = enrichment.classifier->classify(crop);
      }
      const bool decode =
          enrichment.barcode_mode == PageScheduler::BarcodeMode::kAll ||
          (enrichment.barcode_mode == PageScheduler::BarcodeMode::kClassTriggered &&
           barcode_class(region));
      if (decode) region.barcodes = decode_barcodes(crop);
      added.push_back(append_figure(region, page_number, scale, layout_model, document));
    }
  }
  report.pictures_added = static_cast<int>(added.size());
  report.pictures_anchored = anchor_pictures_by_provenance(document, added).anchored;
  counters().pictures_added.fetch_add(static_cast<uint64_t>(report.pictures_added),
                                      std::memory_order_relaxed);
  counters().pictures_anchored.fetch_add(static_cast<uint64_t>(report.pictures_anchored),
                                         std::memory_order_relaxed);
  return report;
}

OfficeCvTotals office_cv_totals() {
  return OfficeCvTotals{
      .pictures_added = counters().pictures_added.load(std::memory_order_relaxed),
      .pictures_anchored = counters().pictures_anchored.load(std::memory_order_relaxed),
  };
}

}  // namespace grparse
