// Anti-drift: geometry ordering does not depend on the order it was told.
//
// A text-layer collector reports its items in whatever order it walked the
// page, and the ordering pass exists precisely because that order is not the
// reading order. So the pass must be a pure function of the geometry: feed
// the same items in every permutation and the body has to come out the same
// way every time, both for the reading-order cut and for picture anchoring by
// provenance. The expected sequences are pinned, not merely compared between
// permutations, so a pass that became consistently wrong still fails.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_reading_order.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

constexpr const char* kPdf = "pdf";

// One body item as a collector reports it: what it says, where it sits, and
// what kind of item it is. Position in the input is deliberately not part of
// this: that is what the permutations vary.
struct Item {
  std::string text;
  int page = 1;
  double l = 0;
  double t = 0;
  double r = 0;
  double b = 0;
  docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT;
};

docv1::Document base_document(int pages) {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  for (int page = 1; page <= pages; ++page) {
    auto& item = (*document.mutable_pages())[page];
    item.set_page_no(page);
    item.mutable_size()->set_width(800);
    item.mutable_size()->set_height(1000);
  }
  return document;
}

void add_item(docv1::Document* document, const Item& item) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  docv1::TextItemBase* base = nullptr;
  switch (item.label) {
    case docv1::DOC_ITEM_LABEL_SECTION_HEADER:
      base = document->add_texts()->mutable_section_header()->mutable_base();
      break;
    default:
      base = document->add_texts()->mutable_text()->mutable_base();
      break;
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(item.label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(item.text);
  auto* prov = base->add_prov();
  prov->set_page_no(item.page);
  auto* box = prov->mutable_bbox();
  box->set_l(item.l);
  box->set_t(item.t);
  box->set_r(item.r);
  box->set_b(item.b);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  base->mutable_source()->Add()->mutable_collector()->set_collector(kPdf);
  document->mutable_body()->add_children()->set_ref(ref);
}

// The body's texts in order, read out by what they say rather than by their
// arena reference: a permuted input numbers the arena differently, and the
// reading order is a statement about content, not about numbering.
std::vector<std::string> body_texts(const docv1::Document& document) {
  std::vector<std::string> out;
  for (const auto& child : document.body().children()) {
    const int index = std::stoi(child.ref().substr(std::string("#/texts/").size()));
    const docv1::BaseTextItem& item = document.texts(index);
    out.push_back(item.has_section_header() ? item.section_header().base().text()
                                            : item.text().base().text());
  }
  return out;
}

std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) out += "[" + part + "]";
  return out;
}

// Two pages of a paper: a full-width heading over two columns on page one
// with a footnote under them, then a full-width paragraph on page two.
std::vector<Item> paper_items() {
  return {
      {"heading", 1, 60, 60, 740, 100, docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"left one", 1, 60, 140, 380, 300},
      {"left two", 1, 60, 320, 380, 520},
      {"right one", 1, 420, 140, 740, 300},
      {"right two", 1, 420, 320, 740, 520},
      {"footnote", 1, 60, 900, 380, 940, docv1::DOC_ITEM_LABEL_FOOTNOTE},
      {"page two", 2, 60, 100, 740, 400},
  };
}

// Every permutation of the reported order folds to one reading order.
void verify_body_order_is_independent_of_report_order() {
  const std::vector<std::string> expected = {"heading",  "left one", "left two", "right one",
                                             "right two", "footnote", "page two"};
  std::vector<Item> items = paper_items();
  // Sorting by text first makes the permutation walk start from a fixed
  // place, so the set of orders visited is the same on every machine.
  std::ranges::sort(items, {}, &Item::text);
  int permutations = 0;
  do {
    docv1::Document document = base_document(2);
    for (const Item& item : items) add_item(&document, item);
    const grparse::BodyOrderReport report = grparse::order_body_by_geometry(&document);
    require(body_texts(document) == expected,
            "permutation " + std::to_string(permutations) + " ordered to " +
                joined(body_texts(document)));
    require(report.pages_reordered >= 0, "the report is filled in");
    permutations++;
  } while (std::next_permutation(items.begin(), items.end(),
                                 [](const Item& a, const Item& b) { return a.text < b.text; }));
  require(permutations == 5040, "seven items have 7! orders; walked " +
                                    std::to_string(permutations));
}

// The pass settles: a second run over its own output moves nothing at all.
void verify_body_order_is_idempotent() {
  docv1::Document document = base_document(2);
  for (const Item& item : paper_items()) add_item(&document, item);
  grparse::order_body_by_geometry(&document);
  const docv1::Document once = document;
  const grparse::BodyOrderReport again = grparse::order_body_by_geometry(&document);
  require(again.items_moved == 0 && again.pages_reordered == 0,
          "a second ordering pass reported work");
  require(google::protobuf::util::MessageDifferencer::Equals(once, document),
          "a second ordering pass changed the document");
}

// ---- picture anchoring ----------------------------------------------------

struct Picture {
  std::string name;
  int page = 1;
  double t = 0;
  double b = 0;
};

std::string add_picture(docv1::Document* document, const Picture& picture) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* item = document->add_pictures();
  item->set_self_ref(ref);
  item->mutable_parent()->set_ref("#/body");
  item->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  item->mutable_meta()->set_accessibility_title(picture.name);
  if (picture.page > 0) {
    auto* prov = item->add_prov();
    prov->set_page_no(picture.page);
    auto* box = prov->mutable_bbox();
    box->set_l(100);
    box->set_t(picture.t);
    box->set_r(700);
    box->set_b(picture.b);
    box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  }
  item->mutable_source()->Add()->mutable_collector()->set_collector(kPdf);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

// The body read out as names: a text item says its text, a picture says the
// title it was given, so a permuted arena is still comparable.
std::vector<std::string> body_names(const docv1::Document& document) {
  std::vector<std::string> out;
  for (const auto& child : document.body().children()) {
    if (child.ref().starts_with("#/texts/")) {
      const int index = std::stoi(child.ref().substr(std::string("#/texts/").size()));
      const docv1::BaseTextItem& item = document.texts(index);
      out.push_back(item.has_section_header() ? item.section_header().base().text()
                                              : item.text().base().text());
    } else {
      const int index = std::stoi(child.ref().substr(std::string("#/pictures/").size()));
      out.push_back(document.pictures(index).meta().accessibility_title());
    }
  }
  return out;
}

// Pictures land at their provenance positions whatever order the detector
// reported them in, and whatever order they were appended to the arena.
void verify_picture_anchoring_is_independent_of_report_order() {
  const std::vector<Picture> pictures = {
      {"figure a", 1, 300, 500},
      {"figure b", 1, 700, 900},
      {"figure c", 2, 100, 300},
      {"figure d", 0, 0, 0},
  };
  const std::vector<std::string> expected = {
      "heading",  "beside the figure", "figure a", "below the figure",
      "figure b", "figure c",          "page three", "figure d",
  };
  std::vector<int> order = {0, 1, 2, 3};
  int permutations = 0;
  do {
    docv1::Document document = base_document(3);
    add_item(&document, {"heading", 1, 60, 60, 740, 100, docv1::DOC_ITEM_LABEL_SECTION_HEADER});
    add_item(&document, {"beside the figure", 1, 60, 320, 740, 460});
    add_item(&document, {"below the figure", 1, 60, 560, 740, 640});
    add_item(&document, {"page three", 3, 60, 100, 740, 300});
    std::vector<std::string> refs;
    for (const int index : order) refs.push_back(add_picture(&document, pictures[index]));
    const grparse::PictureAnchorReport report =
        grparse::anchor_pictures_by_provenance(&document, refs);
    require(report.anchored == 3 && report.appended == 1,
            "three pictures anchor, the boxless one is appended");
    require(body_names(document) == expected,
            "picture order permutation " + std::to_string(permutations) + " gave " +
                joined(body_names(document)));
    permutations++;
  } while (std::ranges::next_permutation(order).found);
  require(permutations == 24, "four pictures have 4! orders; walked " +
                                  std::to_string(permutations));
}

// Anchoring settles too: the same refs offered a second time leave the body
// exactly as it is.
void verify_picture_anchoring_is_idempotent() {
  docv1::Document document = base_document(2);
  add_item(&document, {"lead", 1, 60, 60, 740, 200});
  const std::string first = add_picture(&document, {"figure a", 1, 300, 500});
  add_item(&document, {"tail", 1, 60, 600, 740, 800});
  const std::string second = add_picture(&document, {"figure b", 2, 100, 300});
  const std::vector<std::string> refs = {first, second};
  grparse::anchor_pictures_by_provenance(&document, refs);
  const docv1::Document once = document;
  grparse::anchor_pictures_by_provenance(&document, refs);
  require(google::protobuf::util::MessageDifferencer::Equals(once, document),
          "anchoring the same pictures twice moved them");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("geometry-order-determinism-test", "ok", {
      verify_body_order_is_independent_of_report_order,
      verify_body_order_is_idempotent,
      verify_picture_anchoring_is_independent_of_report_order,
      verify_picture_anchoring_is_idempotent,
  });
}
