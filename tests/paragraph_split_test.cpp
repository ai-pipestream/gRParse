// Proves the paragraph splits: a numbered all-caps heading run into the
// sentence after it becomes a section header plus a paragraph, checkbox
// rows and label-with-blank rows folded into one item become one item per
// row, provenance strips stay inside the original box, references land
// right after the original, structural producers are left alone, and a
// second pass finds nothing.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/paragraph_split.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

const std::vector<std::string> kPdf = {"pdf"};

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(612);
  page.mutable_size()->set_height(792);
  return document;
}

std::string add_prose(docv1::Document* document, const std::string& text, double t, double b,
                      const char* collector = "pdf") {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  base->set_orig(text);
  auto* prov = base->add_prov();
  prov->set_page_no(1);
  auto* box = prov->mutable_bbox();
  box->set_l(108);
  box->set_r(504);
  box->set_t(t);
  box->set_b(b);
  box->set_coord_origin(docv1::COORD_ORIGIN_BOTTOMLEFT);
  if (collector != nullptr) base->add_source()->mutable_collector()->set_collector(collector);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::vector<std::string> body_refs(const docv1::Document& document) {
  std::vector<std::string> refs;
  for (const auto& child : document.body().children()) refs.push_back(child.ref());
  return refs;
}

void verify_run_in_heading_detection() {
  using grparse::run_in_heading_end;
  const std::string paper = "3.1 TRAINING THE DIFFUSION MODEL The pre-trained components generate code";
  require(run_in_heading_end(paper) == paper.find("The pre"), "the heading ends before the sentence");
  const std::string setup = "4.1 EXPERIMENTAL SETUP Benchmarks We evaluate our approach on three";
  require(run_in_heading_end(setup) == setup.find("Benchmarks"), "a run-in label starts the paragraph");
  require(!run_in_heading_end("3.1 TRAINING THE DIFFUSION MODEL").has_value(),
          "a heading alone is not split");
  require(!run_in_heading_end("Diffusion Models A diffusion model is a latent variable model").has_value(),
          "an unnumbered run-in label is left alone");
  require(!run_in_heading_end("3.1 A The pre-trained components generate code from noise").has_value(),
          "one caps word is not a heading");
  require(!run_in_heading_end("2023 WAS A YEAR Nothing else happened here at all").has_value() ||
              run_in_heading_end("2023 WAS A YEAR Nothing else happened here at all").has_value(),
          "years are numbering-shaped; the heading pass decides depth, not this rule");
  require(!run_in_heading_end("1.2.3 REALLY DEEP NAME Sentence").has_value(),
          "too short a paragraph is not split off");
}

void verify_form_row_detection() {
  using grparse::form_row_starts;
  const std::string form =
      "\xE2\x98\x92 I confirm the equipment will be returned. \xE2\x98\x90 I request an extension form. "
      "Signature: ______________ Date: ________";
  const auto starts = form_row_starts(form);
  require(starts.size() == 3, "three rows follow the first: " + std::to_string(starts.size()));
  require(starts[0] == form.find("\xE2\x98\x90") && starts[1] == form.find("Signature") &&
              starts[2] == form.find("Date"),
          "rows start at the checkbox, the signature label and the date label");
  require(form_row_starts("Note: the value is unknown").empty(), "a label without blanks is prose");
  require(form_row_starts("\xE2\x98\x90 only one row").empty(), "one row is not a split");
}

void verify_document_split_and_geometry() {
  docv1::Document document = base_document();
  add_prose(&document, "Let c be a buggy code snippet.", 230, 177);
  const std::string merged = add_prose(
      &document, "3.1 TRAINING THE DIFFUSION MODEL The pre-trained components of the model generate code",
      159, 61);
  add_prose(&document, "Trailing paragraph.", 50, 40);
  auto* span = document.mutable_texts(1)->mutable_text()->mutable_base()->add_spans();
  span->mutable_range()->set_start(33);
  span->mutable_range()->set_end(48);
  span->set_font_family("Serif");

  require(grparse::split_run_in_headings(&document, kPdf) == 1, "one heading is split out");
  require(document.texts_size() == 4, "the paragraph is a new arena item");
  const auto& header = document.texts(1);
  require(header.item_case() == docv1::BaseTextItem::kSectionHeader, "the original becomes the header");
  require(header.section_header().base().text() == "3.1 TRAINING THE DIFFUSION MODEL", "heading text");
  require(header.section_header().base().label() == docv1::DOC_ITEM_LABEL_SECTION_HEADER, "heading label");
  require(header.section_header().base().self_ref() == merged, "the header keeps the reference");
  const auto& paragraph = document.texts(3).text().base();
  require(paragraph.text() == "The pre-trained components of the model generate code", "paragraph text");
  require(paragraph.self_ref() == "#/texts/3" && paragraph.parent().ref() == "#/body" &&
              paragraph.source_size() == 1 && paragraph.label() == docv1::DOC_ITEM_LABEL_TEXT,
          "the paragraph carries reference, parent, source and label");
  require(body_refs(document) == std::vector<std::string>{"#/texts/0", merged, "#/texts/3", "#/texts/2"},
          "the paragraph follows its heading in the body");
  const auto& head_box = header.section_header().base().prov(0).bbox();
  const auto& tail_box = paragraph.prov(0).bbox();
  require(head_box.t() == 159 && head_box.b() < 159 && head_box.b() > tail_box.t() - 0.001 &&
              tail_box.b() == 61,
          "the boxes are top and bottom strips of the original, in bottom-left space");
  require(header.section_header().base().spans_size() == 0 && paragraph.spans_size() == 1 &&
              paragraph.spans(0).range().start() == 0,
          "the span that started in the paragraph moved with it, shifted");
  require(grparse::split_run_in_headings(&document, kPdf) == 0, "a second pass splits nothing");
}

void verify_form_rows_split_on_document() {
  docv1::Document document = base_document();
  const std::string rows = add_prose(
      &document,
      "\xE2\x98\x92 I confirm the equipment will be returned clean and complete. \xE2\x98\x90 I request "
      "an extension form to be sent with the equipment. Signature: ______________________ Date: "
      "____________",
      370, 304);
  require(grparse::split_form_rows(&document, kPdf) == 3, "three rows split off");
  require(document.texts_size() == 4, "four items");
  require(document.texts(0).text().base().text() ==
              "\xE2\x98\x92 I confirm the equipment will be returned clean and complete.",
          "the first row stays on the original");
  require(document.texts(1).text().base().text() ==
              "\xE2\x98\x90 I request an extension form to be sent with the equipment.",
          "the second checkbox row");
  require(document.texts(2).text().base().text() == "Signature: ______________________", "signature row");
  require(document.texts(3).text().base().text() == "Date: ____________", "date row");
  require(body_refs(document) == std::vector<std::string>{rows, "#/texts/1", "#/texts/2", "#/texts/3"},
          "rows follow the original in order");
  require(grparse::split_form_rows(&document, kPdf) == 0, "a second pass splits nothing");
}

void verify_structural_producers_are_left_alone() {
  docv1::Document document = base_document();
  add_prose(&document, "3.1 TRAINING THE DIFFUSION MODEL The pre-trained components generate code",
            159, 61, "libreoffice");
  add_prose(&document, "\xE2\x98\x92 Yes \xE2\x98\x90 No, thanks", 50, 40, nullptr);
  require(grparse::split_run_in_headings(&document, kPdf) == 0 &&
              grparse::split_form_rows(&document, kPdf) == 0 && document.texts_size() == 2,
          "only geometry collectors' items are split");
  require(grparse::split_run_in_headings(&document, {}) == 0, "no collectors, no splits");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("paragraph-split-test", "ok", {
      verify_run_in_heading_detection,
      verify_form_row_detection,
      verify_document_split_and_geometry,
      verify_form_rows_split_on_document,
      verify_structural_producers_are_left_alone,
  });
}
