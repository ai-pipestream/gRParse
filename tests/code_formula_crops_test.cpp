// Proves the code/formula crop recipe: Docling's box (the item's boxes on
// its first page, grown by 0.18 of the union's size on each side), both
// coordinate origins, the page-to-raster mapping and its clamp, which items
// get a crop and under which self_ref, one render per page, the non-PDF
// path that renders nothing, and that every item left without a crop is
// named in a typed warning instead of being dropped.

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "grparse/code_formula_crops.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

void add_box(google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov, int page_no, double l,
             double t, double r, double b,
             docv1::CoordOrigin origin = docv1::COORD_ORIGIN_TOPLEFT) {
  docv1::ProvenanceItem* one = prov->Add();
  one->set_page_no(page_no);
  docv1::BoundingBox* box = one->mutable_bbox();
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(origin);
}

docv1::Document two_page_document() {
  docv1::Document document;
  for (int page_no : {1, 2}) {
    docv1::PageItem& page = (*document.mutable_pages())[page_no];
    page.set_page_no(page_no);
    page.mutable_size()->set_width(1000);
    page.mutable_size()->set_height(2000);
  }
  return document;
}

void verify_box_is_the_first_page_union_grown_by_the_factor() {
  const docv1::Document document = two_page_document();
  google::protobuf::RepeatedPtrField<docv1::ProvenanceItem> prov;
  add_box(&prov, 1, 100, 200, 150, 240);
  add_box(&prov, 1, 160, 190, 300, 230);
  add_box(&prov, 2, 0, 0, 900, 900);  // a later page never widens the crop
  const auto box = grparse::code_formula_box(document, prov);
  require(box.has_value() && box->page_no == 1, "the crop sits on the item's first page");
  // Union 100..300 x 190..240 = 200 x 50, grown 36 and 9 on each side.
  require(near(box->left, 64) && near(box->right, 336) && near(box->top, 181) &&
              near(box->bottom, 249),
          "the union grows by 0.18 of its width and height on every side");
  const auto tight = grparse::code_formula_box(document, prov, 0.0);
  require(tight.has_value() && near(tight->left, 100) && near(tight->bottom, 240),
          "expansion 0 is the plain union");
}

void verify_bottom_left_boxes_flip_to_top_left() {
  const docv1::Document document = two_page_document();
  google::protobuf::RepeatedPtrField<docv1::ProvenanceItem> prov;
  add_box(&prov, 1, 100, 1800, 200, 1700, docv1::COORD_ORIGIN_BOTTOMLEFT);
  const auto box = grparse::code_formula_box(document, prov, 0.0);
  require(box.has_value() && near(box->top, 200) && near(box->bottom, 300),
          "a bottom-left box measures from the page top once flipped");
}

void verify_no_box_without_a_sized_page() {
  const docv1::Document document = two_page_document();
  google::protobuf::RepeatedPtrField<docv1::ProvenanceItem> prov;
  require(!grparse::code_formula_box(document, prov).has_value(), "no provenance, no crop");
  add_box(&prov, 7, 0, 0, 10, 10);
  require(!grparse::code_formula_box(document, prov).has_value(), "a page the document lacks");
}

void verify_pixels_scale_to_the_raster_and_clamp() {
  const grparse::CodeFormulaBox box{1, -50, 100, 500, 2200};
  const cv::Rect pixels = grparse::code_formula_pixels(box, 1000, 2000, cv::Size(500, 1000));
  require(pixels == cv::Rect(0, 50, 250, 950), "half-scale raster, clamped at the left and bottom");
  const grparse::CodeFormulaBox off{1, 1200, 0, 1300, 10};
  require(grparse::code_formula_pixels(off, 1000, 2000, cv::Size(500, 1000)).empty(),
          "a box off the raster crops nothing");
}

void verify_crops_select_items_and_render_each_page_once() {
  docv1::Document document = two_page_document();
  auto* formula = document.add_texts()->mutable_formula();  // #/texts/0, no self_ref
  add_box(formula->mutable_base()->mutable_prov(), 1, 100, 100, 300, 140);
  auto* named = document.add_texts()->mutable_formula();
  named->mutable_base()->set_self_ref("#/texts/1");
  add_box(named->mutable_base()->mutable_prov(), 1, 100, 400, 300, 440);
  auto* code = document.add_texts()->mutable_code();
  code->set_self_ref("#/texts/2");
  add_box(code->mutable_prov(), 2, 100, 100, 600, 300);
  auto* imaged = document.add_texts()->mutable_code();
  imaged->set_self_ref("#/texts/3");
  imaged->mutable_image()->set_uri("data:image/png;base64,AAAA");
  add_box(imaged->mutable_prov(), 2, 100, 400, 600, 600);
  auto* body = document.add_texts()->mutable_text();
  body->mutable_base()->set_self_ref("#/texts/4");
  add_box(body->mutable_base()->mutable_prov(), 1, 0, 0, 10, 10);

  std::map<int, int> renders;
  const grparse::CodeFormulaPageRenderer render = [&renders](int page_no) {
    ++renders[page_no];
    return cv::Mat(1000, 500, CV_8UC3, cv::Scalar(255, 255, 255));
  };
  const grparse::CodeFormulaCrops cropped =
      grparse::code_formula_crops(document, true, true, 0.18, render);
  require(cropped.warnings.empty(), "every item got its crop, so nothing is reported");
  const std::vector<grparse::ChartCandidate>& both = cropped.crops;
  std::vector<std::string> refs;
  for (const auto& crop : both) refs.push_back(crop.self_ref);
  require(refs == std::vector<std::string>{"#/texts/0", "#/texts/1", "#/texts/2"},
          "formulas and image-less code, by self_ref (the arena name when unset)");
  require(renders[1] == 1 && renders[2] == 1, "each page renders once");
  const cv::Mat decoded = cv::imdecode(
      std::vector<unsigned char>(both[0].bytes.begin(), both[0].bytes.end()), cv::IMREAD_COLOR);
  // 200 x 40 grown to 64..336 x 92.8..147.2; at half scale 32..168 x 46..74.
  require(both[0].mimetype == "image/png" && decoded.cols == 136 && decoded.rows == 28,
          "the crop is a PNG of the grown box at the raster's scale: " +
              std::to_string(decoded.cols) + "x" + std::to_string(decoded.rows));

  renders.clear();
  const auto formulas = grparse::code_formula_crops(document, false, true, 0.18, render);
  require(formulas.crops.size() == 2 && formulas.warnings.empty() && renders.count(2) == 0,
          "formula only: no code crop and no render of a page with only code");
  const auto neither = grparse::code_formula_crops(document, false, false, 0.18, render);
  require(neither.crops.empty() && neither.warnings.empty(), "neither job, no crops");
}

// Two formulas on page 1 and one code block on page 2, each under its own
// self_ref, on the two-page document.
docv1::Document three_item_document() {
  docv1::Document document = two_page_document();
  for (const int index : {0, 1}) {
    auto* formula = document.add_texts()->mutable_formula();
    formula->mutable_base()->set_self_ref("#/texts/" + std::to_string(index));
    add_box(formula->mutable_base()->mutable_prov(), 1, 100, 100 + 300 * index, 300,
            140 + 300 * index);
  }
  auto* code = document.add_texts()->mutable_code();
  code->set_self_ref("#/texts/2");
  add_box(code->mutable_prov(), 2, 100, 100, 600, 300);
  return document;
}

// The typed form every missing crop is reported in.
void require_missing_crop(const grparse::Warning& warning, const std::string& self_ref,
                          int page_no) {
  require(warning.code == docv1::WARNING_CODE_ENRICHMENT_CROP_MISSING,
          "a missing crop is typed as one: " + warning.message);
  require_equal(warning.item_ref, self_ref, "the warning names the item without a crop");
  require(warning.page_no == page_no, "the warning names the item's page: " + warning.message);
  require(!warning.count.has_value(), "a warning about one item stands for one");
}

// A page that does not render (an empty render, or a renderer that throws)
// costs every item on it its crop: each is named with its page, and the
// other pages keep theirs. The renderer's own words for a failure ride the
// message.
void verify_items_on_a_page_that_does_not_render_are_named() {
  const docv1::Document document = three_item_document();
  const grparse::CodeFormulaCrops blank = grparse::code_formula_crops(
      document, true, true, 0.18, [](int page_no) {
        return page_no == 2 ? cv::Mat() : cv::Mat(1000, 500, CV_8UC3, cv::Scalar(255, 255, 255));
      });
  require(blank.crops.size() == 2, "the formulas on the rendered page keep their crops");
  require_equal(blank.warnings.size(), size_t{1}, "an empty render is a missing crop too");
  require_missing_crop(blank.warnings[0], "#/texts/2", 2);

  const grparse::CodeFormulaPageRenderer render = [](int page_no) {
    if (page_no == 1) throw std::runtime_error("backend busy");
    return cv::Mat(1000, 500, CV_8UC3, cv::Scalar(255, 255, 255));
  };
  const grparse::CodeFormulaCrops cropped =
      grparse::code_formula_crops(document, true, true, 0.18, render);
  require(cropped.crops.size() == 1 && cropped.crops[0].self_ref == "#/texts/2",
          "the page that rendered keeps its crop");
  require_equal(cropped.warnings.size(), size_t{2}, "one warning per item left without a crop");
  require_missing_crop(cropped.warnings[0], "#/texts/0", 1);
  require_missing_crop(cropped.warnings[1], "#/texts/1", 1);
  require(cropped.warnings[0].message.contains("backend busy"),
          "the render's own reason rides the message: " + cropped.warnings[0].message);
}

// An item whose box maps to no pixel of its page's render has no crop to
// send, and says so by name like any other missing crop.
void verify_an_item_off_the_render_is_named() {
  docv1::Document document = two_page_document();
  auto* formula = document.add_texts()->mutable_formula();
  formula->mutable_base()->set_self_ref("#/texts/0");
  add_box(formula->mutable_base()->mutable_prov(), 1, 1200, 0, 1300, 10);
  const grparse::CodeFormulaCrops cropped = grparse::code_formula_crops(
      document, false, true, 0.0,
      [](int) { return cv::Mat(1000, 500, CV_8UC3, cv::Scalar(255, 255, 255)); });
  require(cropped.crops.empty(), "a box off the page crops nothing");
  require_equal(cropped.warnings.size(), size_t{1}, "the item without a crop is reported");
  require_missing_crop(cropped.warnings[0], "#/texts/0", 1);
}

// A request that is over (cancelled, past its deadline) is not a document
// with missing crops: the pages it left are not rendered and not warned
// about, and the request ends with its own status.
void verify_a_stopped_request_reports_no_missing_crops() {
  const docv1::Document document = three_item_document();
  int renders = 0;
  const grparse::CodeFormulaPageRenderer render = [&renders](int) {
    ++renders;
    return cv::Mat(1000, 500, CV_8UC3, cv::Scalar(255, 255, 255));
  };
  const grparse::CodeFormulaCrops stopped = grparse::code_formula_crops(
      document, true, true, 0.18, render, [&renders] { return renders >= 1; });
  require(renders == 1 && stopped.crops.size() == 2,
          "the page rendered before the stop keeps its crops");
  require(stopped.warnings.empty(), "the page the stop left is not a missing crop");

  // A render the stop itself cut short throws like any failed render.
  bool gone = false;
  const grparse::CodeFormulaCrops cut = grparse::code_formula_crops(
      document, true, true, 0.18,
      [&gone](int) -> cv::Mat {
        gone = true;
        throw std::runtime_error("PDF backend call cancelled");
      },
      [&gone] { return gone; });
  require(cut.crops.empty() && cut.warnings.empty(),
          "a render the request's end cut short is not a missing crop");
}

// A PDF that cannot be opened for rendering costs every item its crop. That
// is said once, about the document, with the number of items it stands for,
// not once per item.
void verify_a_document_that_does_not_open_is_reported_once_with_its_item_count() {
  // No PDF backend is configured, so the open itself fails.
  unsetenv("GRPARSE_PDF_BACKEND");
  const docv1::Document document = three_item_document();
  const auto bytes = std::make_shared<const std::string>("%PDF-crop-fixture");
  const grparse::CodeFormulaCrops cropped = grparse::render_code_formula_crops(
      bytes, document, true, true, 0.18, {}, std::chrono::system_clock::time_point::max());
  require(cropped.crops.empty(), "a document that does not open gives no crops");
  require_equal(cropped.warnings.size(), size_t{1}, "one warning about the document");
  const grparse::Warning& warning = cropped.warnings.front();
  require(warning.code == docv1::WARNING_CODE_ENRICHMENT_CROP_MISSING,
          "the warning is typed as a missing crop");
  require(!warning.page_no.has_value() && warning.item_ref.empty(),
          "it is about the document, not one page or item of it");
  require(warning.count == uint64_t{3}, "it stands for every item left without a crop");
  require(warning.message.contains("GRPARSE_PDF_BACKEND"),
          "the reason the document did not open rides the message: " + warning.message);

  // Formula enrichment alone leaves the code block out of the count.
  const grparse::CodeFormulaCrops formulas = grparse::render_code_formula_crops(
      bytes, document, false, true, 0.18, {}, std::chrono::system_clock::time_point::max());
  require(formulas.warnings.size() == 1 && formulas.warnings.front().count == uint64_t{2},
          "only the items that were to be cropped are counted");

  // A document with nothing to crop is never opened, so nothing is missing.
  const grparse::CodeFormulaCrops nothing = grparse::render_code_formula_crops(
      bytes, two_page_document(), true, true, 0.18, {},
      std::chrono::system_clock::time_point::max());
  require(nothing.crops.empty() && nothing.warnings.empty(),
          "no item to crop, no open, no warning");
}

void verify_non_pdf_bytes_render_nothing() {
  const docv1::Document document = two_page_document();
  const auto bytes = std::make_shared<const std::string>("not a pdf");
  const grparse::CodeFormulaCrops not_pdf = grparse::render_code_formula_crops(
      bytes, document, true, true, 0.18, {}, std::chrono::system_clock::time_point::max());
  require(not_pdf.crops.empty() && not_pdf.warnings.empty(),
          "bytes that are not a PDF give no crops");
  const grparse::CodeFormulaCrops no_bytes = grparse::render_code_formula_crops(
      nullptr, document, true, true, 0.18, {}, std::chrono::system_clock::time_point::max());
  require(no_bytes.crops.empty() && no_bytes.warnings.empty(), "no bytes, no crops");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("code-formula-crops-test", "all checks passed", {
      verify_box_is_the_first_page_union_grown_by_the_factor,
      verify_bottom_left_boxes_flip_to_top_left,
      verify_no_box_without_a_sized_page,
      verify_pixels_scale_to_the_raster_and_clamp,
      verify_crops_select_items_and_render_each_page_once,
      verify_items_on_a_page_that_does_not_render_are_named,
      verify_an_item_off_the_render_is_named,
      verify_a_stopped_request_reports_no_missing_crops,
      verify_a_document_that_does_not_open_is_reported_once_with_its_item_count,
      verify_non_pdf_bytes_render_nothing,
  });
}
