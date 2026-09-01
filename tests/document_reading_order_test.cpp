// Proves the Document-level reading order: a text-layer collector's body
// re-cut page by page (two columns under a full-width title, a footnote
// band, a figure across both columns with its caption, page furniture
// last, boxless items riding with their predecessor), the producer gate
// that leaves structural collectors alone, idempotence, the paper's own
// page-2 anchors, and deterministic picture anchoring by provenance.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "grparse/document_reading_order.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

constexpr const char* kPdf = "pdf";

docv1::Document base_document(int pages, double width, double height) {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  for (int page = 1; page <= pages; ++page) {
    auto& item = (*document.mutable_pages())[page];
    item.set_page_no(page);
    item.mutable_size()->set_width(width);
    item.mutable_size()->set_height(height);
  }
  return document;
}

void add_source(const char* collector,
                google::protobuf::RepeatedPtrField<docv1::SourceType>* sources) {
  if (collector == nullptr) return;
  sources->Add()->mutable_collector()->set_collector(collector);
}

void set_box(docv1::ProvenanceItem* prov, int page, double l, double t, double r, double b,
             docv1::CoordOrigin origin) {
  prov->set_page_no(page);
  auto* box = prov->mutable_bbox();
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(origin);
}

// A body text item with one box; page 0 leaves the item without provenance.
std::string add_text(docv1::Document* document, const std::string& text, int page, double l,
                     double t, double r, double b,
                     docv1::CoordOrigin origin = docv1::COORD_ORIGIN_TOPLEFT,
                     docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT,
                     const char* collector = kPdf) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  docv1::TextItemBase* base = nullptr;
  if (label == docv1::DOC_ITEM_LABEL_SECTION_HEADER) {
    base = document->add_texts()->mutable_section_header()->mutable_base();
  } else {
    base = document->add_texts()->mutable_text()->mutable_base();
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  if (page > 0) set_box(base->add_prov(), page, l, t, r, b, origin);
  add_source(collector, base->mutable_source());
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_picture(docv1::Document* document, int page, double l, double t, double r,
                        double b, docv1::CoordOrigin origin = docv1::COORD_ORIGIN_TOPLEFT,
                        const char* collector = kPdf) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  if (page > 0) set_box(picture->add_prov(), page, l, t, r, b, origin);
  add_source(collector, picture->mutable_source());
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_table(docv1::Document* document, int page, double l, double t, double r, double b,
                      docv1::CoordOrigin origin = docv1::COORD_ORIGIN_TOPLEFT) {
  const std::string ref = "#/tables/" + std::to_string(document->tables_size());
  auto* table = document->add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  set_box(table->add_prov(), page, l, t, r, b, origin);
  add_source(kPdf, table->mutable_source());
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
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

// A two-column page under a full-width title, delivered column-interleaved
// with the footnote in the middle and a running header at the end, all in
// bottom-left coordinates as the pdf collector emits them (page 1000 high).
void verify_columns_title_footnote_and_furniture() {
  docv1::Document document = base_document(1, 800, 1000);
  constexpr auto kBottomLeft = docv1::COORD_ORIGIN_BOTTOMLEFT;
  const std::string a1 = add_text(&document, "A1", 1, 50, 800, 380, 720, kBottomLeft);
  const std::string b1 = add_text(&document, "B1", 1, 420, 800, 750, 720, kBottomLeft);
  const std::string footnote = add_text(&document, "1 footnote", 1, 50, 120, 380, 110, kBottomLeft,
                                        docv1::DOC_ITEM_LABEL_FOOTNOTE);
  const std::string a2 = add_text(&document, "A2", 1, 50, 700, 380, 620, kBottomLeft);
  const std::string b2 = add_text(&document, "B2", 1, 420, 700, 750, 620, kBottomLeft);
  const std::string title = add_text(&document, "Title", 1, 100, 950, 700, 910, kBottomLeft,
                                     docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  const std::string a3 = add_text(&document, "A3", 1, 50, 600, 380, 400, kBottomLeft);
  const std::string b3 = add_text(&document, "B3", 1, 420, 600, 750, 400, kBottomLeft);
  const std::string header = add_text(&document, "Running header", 1, 50, 990, 750, 975,
                                      kBottomLeft, docv1::DOC_ITEM_LABEL_PAGE_HEADER);

  const grparse::BodyOrderReport report = grparse::order_body_by_geometry(&document);
  const std::vector<std::string> expected = {title, a1, a2, a3, b1, b2, b3, footnote, header};
  require(body_refs(document) == expected,
          "title, left column, right column, footnote, furniture; got " +
              joined(body_refs(document)));
  require(report.pages_reordered == 1 && report.items_moved > 0, "the report counts the page");

  const docv1::Document once = document;
  const grparse::BodyOrderReport again = grparse::order_body_by_geometry(&document);
  require(again.items_moved == 0 && google::protobuf::util::MessageDifferencer::Equals(once, document),
          "a second pass changes nothing");
}

// A figure across both columns with its caption below it: the caption is
// bound to the figure and follows it, and the columns below both come
// after; an item with no box rides with the item before it.
void verify_figure_caption_and_boxless_items() {
  docv1::Document document = base_document(1, 800, 1000);
  const std::string a1 = add_text(&document, "A1", 1, 50, 100, 380, 200);
  const std::string b1 = add_text(&document, "B1", 1, 420, 100, 750, 200);
  const std::string caption = add_text(&document, "Figure 1: both columns", 1, 200, 520, 600, 540,
                                       docv1::COORD_ORIGIN_TOPLEFT, docv1::DOC_ITEM_LABEL_CAPTION);
  const std::string caption_tail = add_text(&document, "continues without a box", 0, 0, 0, 0, 0);
  const std::string figure = add_picture(&document, 1, 100, 250, 700, 500);
  const std::string a2 = add_text(&document, "A2", 1, 50, 600, 380, 900);
  const std::string b2 = add_text(&document, "B2", 1, 420, 600, 750, 900);

  grparse::order_body_by_geometry(&document);
  const std::vector<std::string> expected = {a1, b1, figure, caption, caption_tail, a2, b2};
  require(body_refs(document) == expected,
          "figure, its caption and the caption's boxless tail sit between the column bands; got " +
              joined(body_refs(document)));
}

// The pages come out in page order even when the producer appended a
// page's items late; a group orders by the union of its children.
void verify_pages_and_groups() {
  docv1::Document document = base_document(2, 800, 1000);
  const std::string p2 = add_text(&document, "second page", 2, 50, 100, 750, 200);
  const std::string p1_late = add_text(&document, "first page, late", 1, 50, 600, 750, 700);
  const std::string p1_early = add_text(&document, "first page, early", 1, 50, 100, 750, 200);
  // A list group between them: its items sit at 300..500 on page 1.
  const std::string group_ref = "#/groups/0";
  auto* group = document.add_groups();
  group->set_self_ref(group_ref);
  group->set_label(docv1::GROUP_LABEL_LIST);
  for (int item = 0; item < 2; ++item) {
    const std::string ref = "#/texts/" + std::to_string(document.texts_size());
    auto* base = document.add_texts()->mutable_list_item()->mutable_base();
    base->set_self_ref(ref);
    base->mutable_parent()->set_ref(group_ref);
    base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
    set_box(base->add_prov(), 1, 80, 300 + item * 100, 700, 380 + item * 100,
            docv1::COORD_ORIGIN_TOPLEFT);
    add_source(kPdf, base->mutable_source());
    group->add_children()->set_ref(ref);
  }
  document.mutable_body()->add_children()->set_ref(group_ref);

  grparse::order_body_by_geometry(&document);
  const std::vector<std::string> expected = {p1_early, group_ref, p1_late, p2};
  require(body_refs(document) == expected,
          "page 1 before page 2, the list where its items sit; got " + joined(body_refs(document)));
  require(document.groups(0).children_size() == 2, "the group keeps its own children");
}

// A body any structural producer contributed to is never re-cut, and a
// body nothing attributes is not either.
void verify_producer_gate() {
  docv1::Document office = base_document(1, 800, 1000);
  add_text(&office, "later on the page", 1, 50, 600, 750, 700, docv1::COORD_ORIGIN_TOPLEFT,
           docv1::DOC_ITEM_LABEL_TEXT, "libreoffice");
  add_text(&office, "earlier on the page", 1, 50, 100, 750, 200, docv1::COORD_ORIGIN_TOPLEFT,
           docv1::DOC_ITEM_LABEL_TEXT, kPdf);
  const std::vector<std::string> before = body_refs(office);
  require(grparse::order_body_by_geometry(&office).items_moved == 0 && body_refs(office) == before,
          "a document with a structural producer's item keeps its order");

  docv1::Document unattributed = base_document(1, 800, 1000);
  add_text(&unattributed, "later", 1, 50, 600, 750, 700, docv1::COORD_ORIGIN_TOPLEFT,
           docv1::DOC_ITEM_LABEL_TEXT, nullptr);
  add_text(&unattributed, "earlier", 1, 50, 100, 750, 200, docv1::COORD_ORIGIN_TOPLEFT,
           docv1::DOC_ITEM_LABEL_TEXT, nullptr);
  require(grparse::order_body_by_geometry(&unattributed).items_moved == 0,
          "a document with no producer attribution keeps its order");

  grparse::BodyOrderOptions widened;
  widened.geometry_collectors = {"pdf", "libreoffice"};
  require(grparse::order_body_by_geometry(&office, widened).items_moved == 2,
          "the collector set is the gate");
}

// A page where the collector dropped most boxes is left in its own order,
// and an unplaced item after a footnote rides with the main text before
// the footnote, never with the footnote.
void verify_coverage_gate_and_aside_attachment() {
  docv1::Document document = base_document(1, 800, 1000);
  const std::string later = add_text(&document, "later", 1, 50, 600, 750, 700);
  const std::string boxless_1 = add_text(&document, "no box 1", 0, 0, 0, 0, 0);
  const std::string boxless_2 = add_text(&document, "no box 2", 0, 0, 0, 0, 0);
  const std::string earlier = add_text(&document, "earlier", 1, 50, 100, 750, 200);
  const std::string boxless_3 = add_text(&document, "no box 3", 0, 0, 0, 0, 0);
  const std::vector<std::string> before = body_refs(document);
  require(grparse::order_body_by_geometry(&document).items_moved == 0 && body_refs(document) == before,
          "two boxes out of five items is not enough geometry to re-cut the page");

  docv1::Document aside = base_document(1, 800, 1000);
  const std::string a1 = add_text(&aside, "A1", 1, 50, 100, 380, 300);
  const std::string footnote = add_text(&aside, "1 footnote", 1, 50, 900, 380, 915,
                                        docv1::COORD_ORIGIN_TOPLEFT, docv1::DOC_ITEM_LABEL_FOOTNOTE);
  const std::string follower = add_text(&aside, "rides with A1", 0, 0, 0, 0, 0);
  const std::string b1 = add_text(&aside, "B1", 1, 420, 100, 750, 300);
  const std::string a2 = add_text(&aside, "A2", 1, 50, 320, 380, 500);
  grparse::order_body_by_geometry(&aside);
  require(body_refs(aside) == std::vector<std::string>{a1, follower, a2, b1, footnote},
          "the unplaced item follows the main text, the footnote comes last; got " +
              joined(body_refs(aside)));
}

// Page 2 of the eleven-page paper as the pdf collector delivers it (points,
// bottom-left, page 612x792): four figure panels and the figure's own text
// first, the table before the caption, the prose after. The cut puts the
// figure text where it sits, the panels between their labels and the code
// line, the caption (with its boxless second line) after the figure, the
// prose in page order, and the table where it overlaps the last paragraph.
void verify_paper_page_two_anchors() {
  docv1::Document document = base_document(2, 612, 792);
  constexpr auto kBl = docv1::COORD_ORIGIN_BOTTOMLEFT;
  std::vector<std::string> pictures;
  pictures.push_back(add_picture(&document, 2, 145, 687, 220, 645, kBl));
  pictures.push_back(add_picture(&document, 2, 238, 687, 313, 645, kBl));
  pictures.push_back(add_picture(&document, 2, 332, 687, 406, 645, kBl));
  pictures.push_back(add_picture(&document, 2, 425, 687, 499, 645, kBl));
  const std::string forward = add_text(&document, "Forward process", 2, 146, 709, 389, 700, kBl);
  const std::string label_a = add_text(&document, "a", 2, 122, 676, 126, 667, kBl);
  const std::string images = add_text(&document, "Images", 2, 112, 660, 136, 651, kBl);
  const std::string code = add_text(&document, "=SUMIFS(", 2, 122, 624, 489, 609, kBl);
  const std::string table = add_table(&document, 2, 73, 175, 449, 71, kBl);
  const std::string caption = add_text(&document, "Figure 1: Example", 2, 108, 554, 504, 543, kBl);
  const std::string caption_tail = add_text(&document, "sample from the target", 2, 0, 0, 0, 0, kBl);
  const std::string prose_1 = add_text(&document, "training repair systems", 2, 108, 509, 504, 499, kBl);
  const std::string prose_2 = add_text(&document, "2023) and out-of", 2, 108, 498, 504, 334, kBl);
  const std::string background = add_text(&document, "2 BACKGROUND", 2, 108, 315, 200, 303, kBl,
                                          docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  const std::string prose_3 = add_text(&document, "Diffusion Models", 2, 108, 289, 504, 266, kBl);
  const std::string prose_4 = add_text(&document, "2020). The sequence", 2, 108, 267, 504, 153, kBl);
  const std::string prose_5 = add_text(&document, "2022). At inference", 2, 108, 153, 504, 61, kBl);

  grparse::order_body_by_geometry(&document);
  const std::vector<std::string> expected = {
      forward,  label_a, images,  pictures[0], pictures[1], pictures[2], pictures[3], code,
      caption,  caption_tail, prose_1, prose_2, background, prose_3, prose_4, table, prose_5,
  };
  require(body_refs(document) == expected, "paper page 2 reads top to bottom; got " +
                                               joined(body_refs(document)));
}

// Pictures anchor after the first item they overlap vertically (the
// paragraph beside them), else before the first item of their page whose
// top edge is at or below theirs, after the page's last item otherwise,
// and the outcome does not depend on the order they were reported in.
void verify_picture_anchoring() {
  const auto build = [](bool reversed) {
    docv1::Document document = base_document(3, 800, 1000);
    add_text(&document, "heading", 1, 50, 100, 750, 140, docv1::COORD_ORIGIN_TOPLEFT,
             docv1::DOC_ITEM_LABEL_TEXT, "libreoffice");
    add_text(&document, "beside the picture", 1, 300, 300, 750, 340, docv1::COORD_ORIGIN_TOPLEFT,
             docv1::DOC_ITEM_LABEL_TEXT, "libreoffice");
    add_text(&document, "below the picture", 1, 50, 600, 750, 640, docv1::COORD_ORIGIN_TOPLEFT,
             docv1::DOC_ITEM_LABEL_TEXT, "libreoffice");
    add_text(&document, "page three", 3, 50, 100, 750, 140, docv1::COORD_ORIGIN_TOPLEFT,
             docv1::DOC_ITEM_LABEL_TEXT, "libreoffice");
    std::vector<std::string> pictures;
    const auto add = [&](int page, double l, double t, double r, double b) {
      pictures.push_back(add_picture(&document, page, l, t, r, b, docv1::COORD_ORIGIN_TOPLEFT,
                                     "grparse"));
    };
    if (reversed) {
      add(0, 0, 0, 0, 0);          // no provenance at all
      add(2, 50, 100, 400, 300);   // a page with no items
      add(1, 50, 700, 400, 900);   // below every page-1 item
      add(1, 50, 300, 250, 500);   // overlaps "beside the picture"
    } else {
      add(1, 50, 300, 250, 500);
      add(1, 50, 700, 400, 900);
      add(2, 50, 100, 400, 300);
      add(0, 0, 0, 0, 0);
    }
    const grparse::PictureAnchorReport report =
        grparse::anchor_pictures_by_provenance(&document, pictures);
    require(report.anchored == 3 && report.appended == 1, "three anchored, one appended");
    std::vector<std::string> labels;
    for (const auto& child : document.body().children()) {
      if (child.ref().starts_with("#/texts/")) {
        labels.push_back(document.texts(std::stoi(child.ref().substr(8))).text().base().text());
      } else {
        const auto& picture = document.pictures(std::stoi(child.ref().substr(11)));
        labels.push_back(picture.prov_size() == 0
                             ? "picture:none"
                             : "picture:p" + std::to_string(picture.prov(0).page_no()) + "@" +
                                   std::to_string(static_cast<int>(picture.prov(0).bbox().t())));
      }
    }
    return labels;
  };
  const std::vector<std::string> expected = {
      "heading",        "beside the picture", "picture:p1@300", "below the picture",
      "picture:p1@700", "picture:p2@100",     "page three",     "picture:none",
  };
  const auto forward = build(false);
  require(forward == expected, "pictures land at their provenance positions; got " + joined(forward));
  require(build(true) == forward, "the detector's report order does not matter");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("document-reading-order-test", "ok", {
      verify_columns_title_footnote_and_furniture,
      verify_figure_caption_and_boxless_items,
      verify_pages_and_groups,
      verify_producer_gate,
      verify_coverage_gate_and_aside_attachment,
      verify_paper_page_two_anchors,
      verify_picture_anchoring,
  });
}
