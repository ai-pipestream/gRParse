// Proves the code/formula crop recipe: Docling's box (the item's boxes on
// its first page, grown by 0.18 of the union's size on each side), both
// coordinate origins, the page-to-raster mapping and its clamp, which items
// get a crop and under which self_ref, one render per page, and the
// non-PDF path that renders nothing.

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "grparse/code_formula_crops.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

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
  const auto both = grparse::code_formula_crops(document, true, true, 0.18, render);
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
  require(formulas.size() == 2 && renders.count(2) == 0,
          "formula only: no code crop and no render of a page with only code");
  require(grparse::code_formula_crops(document, false, false, 0.18, render).empty(),
          "neither job, no crops");
}

void verify_non_pdf_bytes_render_nothing() {
  const docv1::Document document = two_page_document();
  const auto bytes = std::make_shared<const std::string>("not a pdf");
  require(grparse::render_code_formula_crops(bytes, document, true, true, 0.18, {},
                                             std::chrono::system_clock::time_point::max())
              .empty(),
          "bytes that are not a PDF give no crops");
  require(grparse::render_code_formula_crops(nullptr, document, true, true, 0.18, {},
                                             std::chrono::system_clock::time_point::max())
              .empty(),
          "no bytes, no crops");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("code-formula-crops-test", "all checks passed", {
      verify_box_is_the_first_page_union_grown_by_the_factor,
      verify_bottom_left_boxes_flip_to_top_left,
      verify_no_box_without_a_sized_page,
      verify_pixels_scale_to_the_raster_and_clamp,
      verify_crops_select_items_and_render_each_page_once,
      verify_non_pdf_bytes_render_nothing,
  });
}
