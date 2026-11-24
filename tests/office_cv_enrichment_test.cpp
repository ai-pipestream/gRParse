#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
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
using grparse_test::require_equal;

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
  int barcodes = 0;
  bool decoded = false;
  for (const auto& annotation : picture.annotations()) {
    require(!annotation.has_misc(), "no legacy misc-annotation Struct is written");
    if (annotation.has_classification()) {
      classified = annotation.classification().predicted_classes_size() == 2 &&
                   annotation.classification().predicted_classes(0).class_name() == "qr_code";
    }
    if (annotation.has_barcode()) {
      ++barcodes;
      decoded = annotation.barcode().value() == kQrPayload &&
                annotation.barcode().provenance() == "zxing-cpp" &&
                !annotation.barcode().format().empty();
    }
  }
  require(classified, "the classifier's distribution rides the picture");
  require(picture.annotations_size() == 2 && barcodes == 1,
          "the decoded QR is exactly one annotation beside the classification");
  require(decoded, "the class-triggered decode extracts the QR payload on the typed arm");
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
    require(!annotation.has_barcode() && !annotation.has_misc(),
            "no classifier, no class-triggered decode");
  }

  docv1::Document decode_all = office_document_with_qr_page(222, 2220);
  enrichment.barcode_mode = grparse::PageScheduler::BarcodeMode::kAll;
  grparse::enrich_office_document(enrichment, &decode_all);
  const auto& annotations = decode_all.pictures(0).annotations();
  require(annotations.size() == 1 && annotations.Get(0).has_barcode() &&
              annotations.Get(0).barcode().value() == kQrPayload,
          "decode-all mode needs no classifier: the QR is exactly one typed barcode annotation");
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
  // 1310..2210 twips. On page 1 it overlaps "p1 middle" (1200..1400), which
  // starts above it, so it follows that; on page 2 the paragraph it
  // overlaps, "p2 bottom" (2000..2200), starts well below the figure's top,
  // so the figure reads before it.
  const std::vector<std::string> expected = {
      "p2 top", "#/pictures/2@p2:100", "#/pictures/3@p2:1310", "p2 bottom",
      "p1 top", "#/pictures/0@p1:100", "p1 middle", "#/pictures/1@p1:1310",
      "p1 bottom",
  };
  std::string got;
  for (const auto& entry : a) got += entry + " | ";
  require(a == expected, "figures sit at their provenance positions; got " + got);
}

// A deck: each slide's items sit in that slide's group. A figure found on a
// slide joins that group, among the slide's own items by position, instead
// of trailing the whole slide in the body.
void verify_deck_figures_join_their_slide() {
  docv1::Document document = office_document_with_qr_page(222, 2220);
  docv1::PageItem& second = (*document.mutable_pages())[2];
  second = document.pages().at(1);
  second.set_page_no(2);
  for (int slide = 1; slide <= 2; ++slide) {
    const std::string group_ref = "#/groups/" + std::to_string(document.groups_size());
    docv1::GroupItem* group = document.add_groups();
    group->set_self_ref(group_ref);
    group->mutable_parent()->set_ref("#/body");
    group->set_label(docv1::GROUP_LABEL_SLIDE);
    document.mutable_body()->add_children()->set_ref(group_ref);
    for (const double top : {0.0, 2000.0}) {
      add_paragraph(&document, "s" + std::to_string(slide) + (top == 0.0 ? " title" : " footer"), slide, top);
      // add_paragraph links into the body; move the link into the slide.
      document.mutable_body()->mutable_children()->RemoveLast();
      auto* base = document.mutable_texts(document.texts_size() - 1)->mutable_text()->mutable_base();
      base->mutable_parent()->set_ref(group_ref);
      group->add_children()->set_ref(base->self_ref());
    }
  }
  AlternatingTwoFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;
  const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &document);
  require(report.pictures_added == 4 && report.pictures_anchored == 4, "four figures, all anchored");
  require(document.body().children_size() == 2, "the body still holds just the two slides");
  for (int slide = 0; slide < 2; ++slide) {
    const docv1::GroupItem& group = document.groups(slide);
    std::vector<std::string> sequence;
    for (const auto& child : group.children()) {
      if (child.ref().starts_with("#/texts/")) {
        sequence.push_back(document.texts(std::stoi(child.ref().substr(8))).text().base().text());
      } else {
        const auto& picture = document.pictures(std::stoi(child.ref().substr(11)));
        require(picture.parent().ref() == group.self_ref(), "a figure's parent is its slide");
        require(picture.prov(0).page_no() == slide + 1, "a figure joins the slide it was found on");
        sequence.push_back("figure@" + std::to_string(static_cast<int>(picture.prov(0).bbox().t())));
      }
    }
    const std::string title = "s" + std::to_string(slide + 1) + " title";
    const std::string footer = "s" + std::to_string(slide + 1) + " footer";
    const std::vector<std::string> expected = {title, "figure@100", "figure@1310", footer};
    std::string got;
    for (const auto& entry : sequence) got += entry + " | ";
    require(sequence == expected, "figures read inside their slide; got " + got);
  }
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

// A PNG whose header states `width` x `height` pixels and nothing after it:
// enough for the pixel cap to read, not enough to decode.
std::string png_header_only(uint32_t width, uint32_t height) {
  std::string png("\x89PNG\r\n\x1a\n", 8);
  png += std::string("\0\0\0\x0d", 4) + "IHDR";
  for (const uint32_t value : {width, height}) {
    for (int shift = 24; shift >= 0; shift -= 8) png.push_back(static_cast<char>(value >> shift));
  }
  png += std::string("\x08\x02\0\0\0", 5);
  return png;
}

std::string png_data_uri(const std::string& bytes) {
  return "data:image/png;base64," + grparse::encode_base64(bytes.data(), bytes.size());
}

// A page with no render is skipped and says nothing: there was nothing to
// read. A page whose render is there but cannot be read is skipped too,
// and that is a loss the caller is told about, with the page it is about.
void verify_pages_without_usable_images_are_skipped() {
  WholePageFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;

  docv1::Document no_image;
  (*no_image.mutable_pages())[1].set_page_no(1);
  const grparse::OfficeCvReport nothing = grparse::enrich_office_document(enrichment, &no_image);
  require(no_image.pictures_size() == 0 && detector.calls == 0,
          "a page without an image is skipped");
  require(nothing.warnings.empty() && nothing.pages_unreadable == 0,
          "a page without a render is not a warning");

  struct Unreadable {
    const char* what;
    std::string uri;
  };
  const Unreadable cases[] = {
      {"a render that is not base64", "data:image/png;base64,not-base64!"},
      {"a render over the pixel cap", png_data_uri(png_header_only(100000, 100000))},
      {"a render that does not decode", png_data_uri(png_header_only(64, 64))},
      {"a render that is not an inline PNG", "https://example.test/slide-1.png"},
  };
  for (const Unreadable& one : cases) {
    docv1::Document document = office_document_with_qr_page(222, 2220);
    (*document.mutable_pages())[1].mutable_image()->set_uri(one.uri);
    const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &document);
    require(document.pictures_size() == 0,
            std::string(one.what) + " is skipped, not fatal");
    require_equal(report.pages_unreadable, 1,
                  std::string(one.what) + " counts as one unreadable page");
    require_equal(report.warnings.size(), size_t{1},
                  std::string(one.what) + " raises one warning");
    const grparse::Warning& warning = report.warnings[0];
    require(warning.code == docv1::WARNING_CODE_PAGE_RENDER_UNREADABLE &&
                warning.page_no == std::optional<int32_t>(1) && !warning.message.empty(),
            std::string(one.what) + " is typed PAGE_RENDER_UNREADABLE on page 1: " +
                warning.message);
  }

  // A render that decodes on a page that states no size: a box found on it
  // has no page coordinates to convert to, so the page is not searched. That
  // is the same loss as a render that does not decode, and is told the same
  // way.
  for (const bool sized_zero : {false, true}) {
    docv1::Document unsized = office_document_with_qr_page(222, 2220);
    docv1::PageItem& page = (*unsized.mutable_pages())[1];
    if (sized_zero) {
      page.mutable_size()->set_width(0);
    } else {
      page.clear_size();
    }
    const int calls_before = detector.calls;
    const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &unsized);
    require(unsized.pictures_size() == 0 && detector.calls == calls_before,
            "a render on a page with no size is not searched");
    require_equal(report.pages_unreadable, 1,
                  "a render on a page with no size counts as one page not read");
    require_equal(report.warnings.size(), size_t{1},
                  "a render on a page with no size raises one warning");
    require(report.warnings[0].code == docv1::WARNING_CODE_PAGE_RENDER_UNREADABLE &&
                report.warnings[0].page_no == std::optional<int32_t>(1),
            "and the warning is typed and names the page: " + report.warnings[0].message);
  }

  // Beside readable pages, only the unreadable one is named, and the others
  // are still read.
  docv1::Document mixed = office_document_with_qr_page(222, 2220);
  (*mixed.mutable_pages())[2] = mixed.pages().at(1);
  (*mixed.mutable_pages())[2].set_page_no(2);
  (*mixed.mutable_pages())[2].mutable_image()->set_uri(png_data_uri(png_header_only(64, 64)));
  (*mixed.mutable_pages())[3] = mixed.pages().at(1);
  (*mixed.mutable_pages())[3].set_page_no(3);
  const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &mixed);
  require(report.pictures_added == 2 && report.pages_unreadable == 1 &&
              report.warnings.size() == 1 &&
              report.warnings[0].page_no == std::optional<int32_t>(2),
          "the unreadable page among readable ones is the one named");
}

// A producer that holds the render's bytes puts them in ImageRef.data, not
// in a data: URI. The page is read all the same.
void verify_a_render_in_the_data_field_is_read() {
  WholePageFigureDetector detector;
  grparse::OfficeCvEnrichment enrichment;
  enrichment.detector = &detector;
  docv1::Document document = office_document_with_qr_page(222, 2220);
  docv1::ImageRef* image = (*document.mutable_pages())[1].mutable_image();
  const std::string prefix = "data:image/png;base64,";
  image->set_data(grparse::decode_base64(image->uri().substr(prefix.size())));
  image->clear_uri();
  const grparse::OfficeCvReport report = grparse::enrich_office_document(enrichment, &document);
  require(detector.calls == 1, "a render held as bytes reaches the detector");
  require(report.pictures_added == 1 && report.pages_unreadable == 0 && report.warnings.empty(),
          "and its figure is added with nothing reported unreadable");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("office-cv-enrichment-test", {
      verify_null_detector_is_a_noop,
      verify_detected_figure_lands_scaled_classified_and_decoded,
      verify_class_gate_blocks_decode,
      verify_figures_anchor_deterministically,
      verify_deck_figures_join_their_slide,
      verify_detections_dedupe_against_existing_pictures,
      verify_pages_without_usable_images_are_skipped,
      verify_a_render_in_the_data_field_is_read,
  });
}
