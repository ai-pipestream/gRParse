#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/base64.h"
#include "grparse/office_cv_enrichment.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace fs = std::filesystem;

namespace {

// The committed fixture encodes this payload (see tests/data/qr_code.png).
constexpr const char* kQrPayload = "https://github.com/krickert/gRParse/e3";

using grparse_test::require;

// A detector that reports two figures per page in a different order on
// every call, the way a real detector's output order is not stable.
class AlternatingTwoFigureDetector final : public grparse::RegionDetector {
 public:
  std::vector<grparse::LayoutRegion> detect_regions(const cv::Mat& image) override {
    grparse::LayoutRegion upper;
    upper.label = "picture";
    upper.confidence = 0.9F;
    upper.left = 10;
    upper.top = 10;
    upper.right = image.cols / 2;
    upper.bottom = image.rows / 2;
    grparse::LayoutRegion lower;
    lower.label = "picture";
    lower.confidence = 0.8F;
    lower.left = 10;
    lower.top = image.rows / 2 + 20;
    lower.right = image.cols - 1;
    lower.bottom = image.rows - 1;
    grparse::LayoutRegion text;
    text.label = "text";
    text.left = 0;
    text.top = 0;
    text.right = 5;
    text.bottom = 5;
    ++calls;
    if (calls % 2 == 0) return {lower, text, upper};
    return {upper, lower, text};
  }
  int calls = 0;
};

// A deterministic detector so the hybrid leg proves out without any model:
// it reports one figure region covering the whole raster.
class WholePageFigureDetector final : public grparse::RegionDetector {
 public:
  std::vector<grparse::LayoutRegion> detect_regions(const cv::Mat& image) override {
    ++calls;
    grparse::LayoutRegion region;
    region.label = "picture";
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
              picture.source(0).collector().model() == "layout",
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

// One office paragraph on a page, in twips, top-left origin.
void add_paragraph(docv1::Document* document, const std::string& text, int page, double top) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  auto* prov = base->add_prov();
  prov->set_page_no(page);
  auto* box = prov->mutable_bbox();
  box->set_l(0);
  box->set_t(top);
  box->set_r(2220);
  box->set_b(top + 200);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  base->add_source()->mutable_collector()->set_collector("libreoffice");
  document->mutable_body()->add_children()->set_ref(ref);
}

// Two pages with two figures each, paragraphs above, between and below
// them: the pictures arena and the body come out identical however the
// detector orders its output, and each figure sits at its provenance
// position instead of at the end of the body.
void verify_figures_anchor_deterministically() {
  const auto convert = [](grparse::RegionDetector* detector) {
    docv1::Document document = office_document_with_qr_page(222, 2220);
    docv1::PageItem& second = (*document.mutable_pages())[2];
    second = document.pages().at(1);
    second.set_page_no(2);
    // Page 2's paragraphs first in the body to prove page order is by
    // page number, not by map iteration or body order.
    add_paragraph(&document, "p2 top", 2, 0);
    add_paragraph(&document, "p2 bottom", 2, 2000);
    add_paragraph(&document, "p1 top", 1, 0);
    add_paragraph(&document, "p1 middle", 1, 1200);
    add_paragraph(&document, "p1 bottom", 1, 2000);
    grparse::OfficeCvEnrichment enrichment;
    enrichment.detector = detector;
    const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &document);
    require(report.pictures_added == 4 && report.pictures_anchored == 4,
            "four figures, all anchored");
    std::vector<std::string> sequence;
    for (const auto& child : document.body().children()) {
      if (child.ref().starts_with("#/texts/")) {
        sequence.push_back(document.texts(std::stoi(child.ref().substr(8))).text().base().text());
      } else {
        const auto& picture = document.pictures(std::stoi(child.ref().substr(11)));
        sequence.push_back(child.ref() + "@p" + std::to_string(picture.prov(0).page_no()) + ":" +
                           std::to_string(static_cast<int>(picture.prov(0).bbox().t())));
      }
    }
    return sequence;
  };
  AlternatingTwoFigureDetector first;
  AlternatingTwoFigureDetector second;
  second.calls = 1;  // starts on the other order
  const auto a = convert(&first);
  const auto b = convert(&second);
  require(a == b, "the detector's output order does not change the document");
  // Upper figure: 10..111 px -> 100..1110 twips, overlapping the page's
  // first paragraph (0..200) so it follows it; lower figure: 131..221 px ->
  // 1310..2210 twips, overlapping "p1 middle" (1200..1400) and "p2 bottom"
  // (2000..2200) respectively, so it follows those.
  const std::vector<std::string> expected = {
      "p2 top", "#/pictures/2@p2:100", "p2 bottom", "#/pictures/3@p2:1310",
      "p1 top", "#/pictures/0@p1:100", "p1 middle", "#/pictures/1@p1:1310",
      "p1 bottom",
  };
  std::string got;
  for (const auto& entry : a) got += entry + " | ";
  require(a == expected, "figures sit at their provenance positions; got " + got);
}

// A detection where the office core already placed a picture is dropped,
// and so is a second detection of the same place on one page.
void verify_detections_dedupe_against_existing_pictures() {
  docv1::Document document = office_document_with_qr_page(222, 2220);
  add_paragraph(&document, "before", 1, 0);
  // The office core's own picture where the upper detection will land:
  // 10..111 px is 100..1110 twips.
  auto* own = document.add_pictures();
  own->set_self_ref("#/pictures/0");
  own->mutable_parent()->set_ref("#/body");
  own->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  auto* prov = own->add_prov();
  prov->set_page_no(1);
  auto* box = prov->mutable_bbox();
  box->set_l(120);
  box->set_t(120);
  box->set_r(1100);
  box->set_b(1100);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  own->add_source()->mutable_collector()->set_collector("libreoffice");
  document.mutable_body()->add_children()->set_ref("#/pictures/0");

  AlternatingTwoFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;
  const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &document);
  require(report.pictures_deduplicated == 1 && report.pictures_added == 1 && report.pictures_anchored == 1,
          "the upper detection is a duplicate of the office picture, the lower one is new");
  require(document.pictures_size() == 2 && document.pictures(1).prov(0).bbox().t() == 1310,
          "only the lower figure was added");
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
  return grparse_test::run_test_main("office-cv-enrichment-test", {
      verify_null_detector_is_a_noop,
      verify_detected_figure_lands_scaled_classified_and_decoded,
      verify_class_gate_blocks_decode,
      verify_figures_anchor_deterministically,
      verify_detections_dedupe_against_existing_pictures,
      verify_pages_without_usable_images_are_skipped,
  });
}
