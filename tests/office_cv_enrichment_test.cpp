#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/base64.h"
#include "grparse/office_cv_enrichment.h"

namespace docv1 = ai::pipestream::document::v1;
namespace fs = std::filesystem;

namespace {

// The committed fixture encodes this payload (see tests/data/qr_code.png).
constexpr const char* kQrPayload = "https://github.com/krickert/gRParse/e3";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// A deterministic detector so the hybrid leg proves out without any model:
// it reports one figure region covering the whole raster.
class WholePageFigureDetector final : public grparse::RegionDetector {
 public:
  std::vector<grparse::LayoutRegion> detect_regions(const cv::Mat& image) override {
    ++calls;
    grparse::LayoutRegion region;
    region.label = "figure";
    region.confidence = 0.9F;
    region.right = image.cols - 1;
    region.bottom = image.rows - 1;
    return {region};
  }
  int calls = 0;
};

class FixedClassifier final : public grparse::FigureClassifierBase {
 public:
  std::vector<grparse::FigureClass> classify(const cv::Mat&) override {
    return {{"qr_code", 0.99F}, {"bar_chart", 0.01F}};
  }
};

std::string qr_page_data_uri() {
  const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
  const fs::path image_path =
      fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "qr_code.png";
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "QR fixture must load: " + image_path.string());
  std::vector<unsigned char> png;
  require(cv::imencode(".png", image, png), "fixture re-encodes as PNG");
  return "data:image/png;base64," + grparse::encode_base64(png.data(), png.size());
}

// One office-mapped page: size in twips (the office coordinate space), the
// render as a PNG data URI, exactly what DoclingMapper::on_page_image stores.
docv1::Document office_document_with_qr_page(int width_px, int width_twips) {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  docv1::PageItem& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(width_twips);
  page.mutable_size()->set_height(width_twips);
  docv1::ImageRef* image = page.mutable_image();
  image->set_mimetype("image/png");
  image->mutable_size()->set_width(width_px);
  image->mutable_size()->set_height(width_px);
  image->set_uri(qr_page_data_uri());
  return document;
}

void verify_null_detector_is_a_noop() {
  docv1::Document document = office_document_with_qr_page(222, 2220);
  grparse::enrich_office_document({}, &document);
  require(document.pictures_size() == 0, "no detector, no enrichment");
}

void verify_detected_figure_lands_scaled_classified_and_decoded() {
  // The fixture is 222 px square; a 10x twips page proves the box scaling.
  docv1::Document document = office_document_with_qr_page(222, 2220);
  WholePageFigureDetector detector;
  FixedClassifier classifier;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;
  enrichment.classifier = &classifier;
  enrichment.barcode_mode = grparse::PageScheduler::BarcodeMode::kClassTriggered;
  grparse::enrich_office_document(enrichment, &document);

  require(detector.calls == 1, "the page render runs through the detector once");
  require(document.pictures_size() == 1, "the figure region becomes one picture");
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.self_ref() == "#/pictures/0", "pictures number from the document");
  require(document.body().children_size() == 1 &&
              document.body().children(0).ref() == picture.self_ref(),
          "the picture links into the body");
  require(picture.prov_size() == 1 && picture.prov(0).page_no() == 1,
          "provenance names the page");
  const docv1::BoundingBox& box = picture.prov(0).bbox();
  require(box.l() == 0 && box.r() == (222 - 1) * 10.0 && box.b() == (222 - 1) * 10.0,
          "the box scales from render pixels into the page's twips space");
  require(picture.source_size() == 1 &&
              picture.source(0).collector().collector() == "grparse" &&
              picture.source(0).collector().model() == "picodet-publaynet",
          "the CV leg tags its own source");

  bool classified = false;
  bool decoded = false;
  for (const auto& annotation : picture.annotations()) {
    if (annotation.has_classification()) {
      classified = annotation.classification().predicted_classes_size() == 2 &&
                   annotation.classification().predicted_classes(0).class_name() == "qr_code";
    }
    if (annotation.has_misc() && annotation.misc().kind() == "barcode") {
      const auto& fields = annotation.misc().content().fields();
      decoded = fields.count("value") > 0 &&
                fields.at("value").string_value() == kQrPayload;
    }
  }
  require(classified, "the classifier's distribution rides the picture");
  require(decoded, "the class-triggered decode extracts the QR payload");
}

void verify_class_gate_blocks_decode() {
  docv1::Document document = office_document_with_qr_page(222, 2220);
  WholePageFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;
  // No classifier: class-triggered mode has no class to trigger on.
  enrichment.barcode_mode = grparse::PageScheduler::BarcodeMode::kClassTriggered;
  grparse::enrich_office_document(enrichment, &document);
  require(document.pictures_size() == 1, "the figure still lands");
  for (const auto& annotation : document.pictures(0).annotations()) {
    require(!annotation.has_misc(), "no classifier, no class-triggered decode");
  }

  docv1::Document decode_all = office_document_with_qr_page(222, 2220);
  enrichment.barcode_mode = grparse::PageScheduler::BarcodeMode::kAll;
  grparse::enrich_office_document(enrichment, &decode_all);
  bool decoded = false;
  for (const auto& annotation : decode_all.pictures(0).annotations()) {
    if (annotation.has_misc() && annotation.misc().kind() == "barcode") decoded = true;
  }
  require(decoded, "decode-all mode needs no classifier");
}

void verify_pages_without_usable_images_are_skipped() {
  WholePageFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;

  docv1::Document no_image;
  (*no_image.mutable_pages())[1].set_page_no(1);
  grparse::enrich_office_document(enrichment, &no_image);
  require(no_image.pictures_size() == 0 && detector.calls == 0,
          "a page without an image is skipped");

  docv1::Document bad_uri = office_document_with_qr_page(222, 2220);
  (*bad_uri.mutable_pages())[1].mutable_image()->set_uri(
      "data:image/png;base64,not-base64!");
  grparse::enrich_office_document(enrichment, &bad_uri);
  require(bad_uri.pictures_size() == 0, "an undecodable image is skipped, not fatal");
}

}  // namespace

int main() {
  try {
    verify_null_detector_is_a_noop();
    verify_detected_figure_lands_scaled_classified_and_decoded();
    verify_class_gate_blocks_decode();
    verify_pages_without_usable_images_are_skipped();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "office-cv-enrichment-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
