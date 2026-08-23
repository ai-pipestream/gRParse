#include "grparse/office_cv_enrichment.h"

#include <optional>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/barcode_decoder.h"
#include "grparse/base64.h"
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

// One detected figure as a PictureItem, box scaled from render pixels into
// the page's own coordinate space and linked into the body like every other
// mapped item.
void append_figure(const LayoutRegion& region, int page_number, double scale,
                   docv1::Document* document) {
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
  add_collector_source("picodet-publaynet", region.confidence, picture->mutable_source());

  if (!region.figure_classes.empty()) {
    auto* classification = picture->add_annotations()->mutable_classification();
    classification->set_kind("classification");
    classification->set_provenance("DocumentFigureClassifier");
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
}

}  // namespace

void enrich_office_document(const OfficeCvEnrichment& enrichment,
                            docv1::Document* document) {
  if (enrichment.detector == nullptr || document == nullptr) return;
  for (auto& [page_number, page] : *document->mutable_pages()) {
    const cv::Mat raster = decode_page_image(page);
    if (raster.empty()) continue;
    // The page's coordinate space (twips for office documents) and the
    // render's pixel space share only their aspect ratio; every typed item's
    // provenance is in the former, so detected boxes convert on the way in.
    if (page.size().width() <= 0 || raster.cols <= 0) continue;
    const double scale = page.size().width() / static_cast<double>(raster.cols);

    std::vector<LayoutRegion> regions = enrichment.detector->detect_regions(raster);
    for (auto& region : regions) {
      if (region.label != "figure") continue;
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
      append_figure(region, page_number, scale, document);
    }
  }
}

}  // namespace grparse
