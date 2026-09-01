// Anti-drift: the same collector input assembles to the same bytes.
//
// Assembly is where a page of recognized lines, layout regions, captured
// figure bytes and page previews becomes arena items, references and
// provenance. Nothing in that fold may depend on iteration order, address
// order, or how many times it has run in this process: two runs over one
// input must render to identical canonical JSON, and the sequence they
// produce is pinned here rather than left to "no crash". The repair pass is
// folded in as well, because the service hands out the repaired document and
// a nondeterministic repair would be just as invisible.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_assembly.h"
#include "grparse/document_render.h"
#include "grparse/document_repair.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

grparse::OcrLine line(std::string text, int left, int top, int right, int bottom) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                          0.875F};
}

// Minimal PNG prefix: signature, IHDR length and tag, width 300, height 200.
const std::vector<unsigned char> kPng = {0x89, 'P', 'N',  'G',  0x0D, 0x0A, 0x1A, 0x0A,
                                         0,    0,   0,    0x0D, 'I',  'H',  'D',  'R',
                                         0,    0,   0x01, 0x2C, 0,    0,    0,    0xC8};

// Three pages that touch every arena assembly writes into: a title region, a
// table region with two bound lines, a classified figure carrying captured
// bytes and a barcode, running furniture, and a page preview.
std::vector<grparse::OcrPage> collector_pages() {
  grparse::OcrPage first{1000, 1400,
                         {line("Quarterly Field Report", 60, 40, 940, 90),
                          line("Prepared for the survey committee.", 60, 140, 940, 170),
                          line("Region", 80, 320, 300, 350),
                          line("North", 80, 380, 300, 410)}};
  first.regions = {
      {"title", 0.94F, 40, 20, 960, 100},
      {"table", 0.88F, 60, 300, 940, 430},
  };
  first.layout_model = "layout-test";
  first.preview_png = kPng;

  grparse::OcrPage second{1000, 1400,
                          {line("Quarterly Field Report", 60, 20, 940, 45),
                           line("The instruments were calibrated before every tra-", 60, 200,
                                480, 230),
                           line("verse of the site.", 60, 240, 480, 270),
                           line("2", 480, 1350, 520, 1380)}};
  grparse::LayoutRegion header{"page_header", 0.8F, 40, 10, 960, 60};
  grparse::LayoutRegion footer{"page_footer", 0.8F, 440, 1340, 560, 1390};
  second.regions = {header, footer};

  grparse::OcrPage third{1000, 1400, {line("Figure 1: the north traverse.", 60, 900, 600, 930)}};
  grparse::LayoutRegion figure{"picture", 0.77F, 60, 400, 940, 880, kPng};
  figure.figure_classes = {{"bar_chart", 0.91F}, {"map", 0.05F}};
  figure.barcodes = {{"QRCode", "https://example.org/traverse"}};
  third.regions = {figure, {"caption", 0.7F, 60, 890, 940, 940}};
  third.layout_model = "layout-test";

  return {first, second, third};
}

// One assembly run: a fresh cursor, every page folded in page order.
docv1::Document assemble(const std::vector<grparse::OcrPage>& pages, std::string* plain_text) {
  grparse::AssemblyCursor cursor;
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  int page_number = 1;
  for (const grparse::OcrPage& page : pages) {
    grparse::append_page_to_document(page, page_number++, &cursor, &document, plain_text);
  }
  return document;
}

std::vector<std::string> body_refs(const docv1::Document& document) {
  std::vector<std::string> refs;
  for (const auto& child : document.body().children()) refs.push_back(child.ref());
  return refs;
}

std::string joined(const std::vector<std::string>& refs) {
  std::string out;
  for (const auto& ref : refs) out += ref + " ";
  return out;
}

// The shape one assembly of collector_pages() produces, pinned so a change in
// how regions bind, how floats anchor, or how furniture is split shows up as
// a failure here and not only as a moved scorecard baseline.
void verify_pinned_assembly_shape() {
  std::string plain_text;
  const docv1::Document document = assemble(collector_pages(), &plain_text);

  const std::vector<std::string> expected = {
      "#/texts/0", "#/texts/1", "#/tables/0", "#/texts/3", "#/texts/4", "#/pictures/0",
  };
  require(body_refs(document) == expected,
          "the body reads title, lead paragraph, table, the two second-page lines and the "
          "figure; got " +
              joined(body_refs(document)));
  require(document.pictures(0).captions_size() == 1 &&
              document.pictures(0).captions(0).ref() == "#/texts/6",
          "the caption binds to the figure rather than trailing the body");
  require(document.furniture().children_size() == 2,
          "the running header and the page number are furniture");
  require(document.texts(0).has_title(), "the first-page title region makes a TITLE item");
  require(document.tables_size() == 1 && document.tables(0).data().num_rows() == 2 &&
              document.tables(0).data().num_cols() == 1,
          "the two bound lines make a two-row geometry grid");
  require(document.pictures_size() == 1 && document.pictures(0).has_image() &&
              document.pictures(0).image().size().width() == 300,
          "the captured figure bytes ride on the picture");
  require(document.pages_size() == 3, "three pages");
  require(plain_text ==
              "Quarterly Field Report\nPrepared for the survey committee.\n"
              "Quarterly Field Report\n"
              "The instruments were calibrated before every tra-\nverse of the site.\n"
              "2\n"
              "Figure 1: the north traverse.",
          "the text stream is every recognized line in page order, furniture included, with "
          "the table's interior text left to the table");
}

// Three assemblies of one input render to the same bytes: no counter, cache,
// map iteration order or arena address leaks into the fold.
void verify_assembly_is_byte_identical_across_runs() {
  const std::vector<grparse::OcrPage> pages = collector_pages();
  std::string first_text;
  std::string second_text;
  std::string third_text;
  const docv1::Document first = assemble(pages, &first_text);
  const docv1::Document second = assemble(pages, &second_text);
  const std::string rendered = grparse::render_canonical_json(first);
  require(!rendered.empty(), "the canonical rendering is not empty");
  require(rendered == grparse::render_canonical_json(second),
          "a second assembly of the same input renders different canonical JSON");
  require(first_text == second_text, "the text stream differs between two identical assemblies");
  require(google::protobuf::util::MessageDifferencer::Equals(first, second),
          "a second assembly of the same input is not the same message");

  // A fresh input object, built from scratch rather than reused, must fold the
  // same way: the pages are data, not identity.
  const docv1::Document third = assemble(collector_pages(), &third_text);
  require(rendered == grparse::render_canonical_json(third),
          "assembly depends on the identity of its input pages, not only their content");
}

// The document the service hands out is the repaired one, so the repair pass
// has to be deterministic over an identical assembly too, and idempotent on
// the result.
void verify_repaired_assembly_is_byte_identical_and_settled() {
  std::string first_text;
  std::string second_text;
  docv1::Document first = assemble(collector_pages(), &first_text);
  docv1::Document second = assemble(collector_pages(), &second_text);
  const grparse::RepairReport first_report = grparse::repair_document(&first);
  const grparse::RepairReport second_report = grparse::repair_document(&second);
  require(first_report.hyphens_rejoined == second_report.hyphens_rejoined &&
              first_report.paragraphs_merged == second_report.paragraphs_merged &&
              first_report.furniture_demoted == second_report.furniture_demoted,
          "two repairs of the same document report different work");
  require(grparse::render_canonical_json(first) == grparse::render_canonical_json(second),
          "two repairs of the same document render different canonical JSON");

  const std::string once = grparse::render_canonical_json(first);
  const grparse::RepairReport again = grparse::repair_document(&first);
  require(!again.changed_anything(), "a second repair of a repaired document found work to do");
  require(grparse::render_canonical_json(first) == once,
          "a second repair of a repaired document moved bytes");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("assembly-determinism-test", "ok", {
      verify_pinned_assembly_shape,
      verify_assembly_is_byte_identical_across_runs,
      verify_repaired_assembly_is_byte_identical_and_settled,
  });
}
