// The AcroForm widget fold: a PdfBackendService's FormField messages become
// Document.field_items under one field region per page, in the page's own
// space, with the typed widget state (/Ff, /AS, kind, value) on the item and
// out of the canonical JSON. The fetch runs against an in-process fake
// backend, so the wire path (the handshake, the absent and unsupported
// verdicts, the target list) is covered without a real engine.

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/document_geometry.h"
#include "grparse/document_render.h"
#include "grparse/pdf_form_widgets.h"
#include "support/check.h"
#include "support/document_builder.h"
#include "support/fake_pdf_backend.h"

namespace docv1 = ai::pipestream::document::v1;
namespace pdfv1 = ai::protomolt::parse::pdf::v1;

using grparse_test::add_page;
using grparse_test::add_paragraph;
using grparse_test::add_prov;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

// Field-by-field equality: Document.pages is a map, whose wire order is
// not part of the document.
bool same(const docv1::Document& a, const docv1::Document& b) {
  return google::protobuf::util::MessageDifferencer::Equals(a, b);
}

constexpr double kWidthPts = 612.0;
constexpr double kHeightPts = 792.0;

pdfv1::FormField text_field() {
  pdfv1::FormField field;
  field.set_kind(pdfv1::FORM_FIELD_KIND_TEXT);
  field.set_name("customer_name");
  field.set_value("Jordan Example");
  field.set_alternate_name("Customer name");
  field.set_flags(0);
  auto* rect = field.mutable_rect();
  rect->set_x0(300);
  rect->set_y0(300);
  rect->set_x1(450);
  rect->set_y1(320);
  return field;
}

// rich.pdf's check box: /FT and /Ff (ReadOnly) inherited, /AS /Yes on the
// widget.
pdfv1::FormField check_box() {
  pdfv1::FormField field;
  field.set_kind(pdfv1::FORM_FIELD_KIND_CHECK_BOX);
  field.set_name("agree");
  field.set_value("Yes");
  field.set_alternate_name("I agree");
  field.set_flags(1);
  field.set_read_only(true);
  field.set_appearance_state("/Yes");
  auto* rect = field.mutable_rect();
  rect->set_x0(300);
  rect->set_y0(250);
  rect->set_x1(315);
  rect->set_y1(265);
  return field;
}

pdfv1::FormField combo_box() {
  pdfv1::FormField field;
  field.set_kind(pdfv1::FORM_FIELD_KIND_COMBO_BOX);
  field.set_name("form.country");
  field.set_value("Norway");
  field.set_flags(1U << 17);
  field.add_options("Denmark");
  field.add_options("Norway");
  auto* rect = field.mutable_rect();
  rect->set_x0(72);
  rect->set_y0(250);
  rect->set_x1(200);
  rect->set_y1(265);
  return field;
}

grparse::PdfPageWidgets page_one(std::vector<pdfv1::FormField> fields) {
  grparse::PdfPageWidgets page;
  page.page_number = 1;
  page.page_info.set_width_pts(kWidthPts);
  page.page_info.set_height_pts(kHeightPts);
  page.fields = std::move(fields);
  return page;
}

// A CV-path document: pages in raster pixels at 144 DPI (twice the
// points), top-left boxes, one paragraph on each of two pages.
docv1::Document cv_document() {
  docv1::Document document = base_document("form.pdf");
  add_page(&document, 1, kWidthPts * 2, kHeightPts * 2);
  add_page(&document, 2, kWidthPts * 2, kHeightPts * 2);
  add_paragraph(&document, "#/body", "page one text");
  add_prov(document.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1, 100,
           100, 500, 140);
  add_paragraph(&document, "#/body", "page two text");
  add_prov(document.mutable_texts(1)->mutable_text()->mutable_base()->mutable_prov(), 2, 100,
           100, 500, 140);
  return document;
}

const docv1::TextItemBase& text_at(const docv1::Document& document, const std::string& ref) {
  const int index = std::stoi(ref.substr(std::string("#/texts/").size()));
  const auto* base = grparse::text_base_of(document.texts(index));
  require(base != nullptr, "field children are text items with a base");
  return *base;
}

void folds_into_the_cv_page_space() {
  docv1::Document document = cv_document();
  grparse::fold_pdf_form_widgets({page_one({text_field(), check_box(), combo_box()})},
                                 "grpc-pdfium", &document);

  require_equal(document.field_regions_size(), 1, "one region for the one page with widgets");
  require_equal(document.field_items_size(), 3, "one field item per widget");
  // The region enters the body after page one's text, before page two's.
  require_equal(document.body().children_size(), 3, "the region joins the body");
  require_equal(document.body().children(1).ref(), std::string("#/field_regions/0"),
                "region sits after the last page-one item");
  const auto& region = document.field_regions(0);
  require(region.label() == docv1::DOC_ITEM_LABEL_FIELD_REGION, "region label");
  require_equal(region.parent().ref(), std::string("#/body"), "region parent");
  require_equal(region.children_size(), 3, "region lists its fields");

  // Reading order: the text field (top edge 320) first, then the row at
  // 265, left to right: the combo box at x 72 before the check box at 300.
  const auto& text = document.field_items(0);
  const auto& combo = document.field_items(1);
  const auto& box = document.field_items(2);
  require_equal(text.field_name(), std::string("customer_name"), "top widget first");
  require_equal(combo.field_name(), std::string("form.country"), "left widget of the row next");
  require_equal(box.field_name(), std::string("agree"), "right widget of the row last");

  // Contract points (bottom-left) to the page's top-left pixels.
  const auto& bbox = text.prov(0).bbox();
  require(bbox.coord_origin() == docv1::COORD_ORIGIN_TOPLEFT, "the page's origin");
  require_equal(bbox.l(), 600.0, "left scales by size over points");
  require_equal(bbox.r(), 900.0, "right scales");
  require_equal(bbox.t(), (kHeightPts - 320) * 2, "top flips against the page height");
  require_equal(bbox.b(), (kHeightPts - 300) * 2, "bottom flips");
  require_equal(text.prov(0).page_no(), 1, "page number");
  require_equal(region.prov(0).bbox().l(), 144.0, "region box spans its widgets");
  require_equal(region.prov(0).bbox().t(), (kHeightPts - 320) * 2, "region top");

  // The typed widget state.
  require(text.field_kind() == docv1::FORM_FIELD_KIND_TEXT, "text kind");
  require_equal(text.value(), std::string("Jordan Example"), "text value");
  require_equal(text.description(), std::string("Customer name"), "tooltip");
  require(text.has_field_flags() && text.field_flags() == 0, "empty /Ff kept");
  require(!text.read_only(), "editable");
  require(!text.has_appearance_state(), "no /AS on a text widget");
  require(box.field_kind() == docv1::FORM_FIELD_KIND_CHECK_BOX, "check box kind");
  require_equal(box.field_flags(), 1U, "inherited /Ff");
  require_equal(box.appearance_state(), std::string("/Yes"), "/AS keeps its slash");
  require(box.read_only(), "read-only from /Ff");
  require_equal(box.value(), std::string("Yes"), "button value");
  require(combo.field_kind() == docv1::FORM_FIELD_KIND_COMBO_BOX, "combo kind");
  require_equal(combo.options_size(), 2, "choice options");
  require(combo.has_selected_index() && combo.selected_index() == 1,
          "the value names the selected option");

  // Children: a field_key and a value of the upstream shape.
  require_equal(text.children_size(), 2, "key and value");
  const auto& key = text_at(document, text.children(0).ref());
  require(key.label() == docv1::DOC_ITEM_LABEL_FIELD_KEY, "key label");
  require_equal(key.text(), std::string("Customer name"), "key reads the tooltip");
  require_equal(key.parent().ref(), std::string("#/field_items/0"), "key parent");
  const int value_index = std::stoi(text.children(1).ref().substr(8));
  require(document.texts(value_index).item_case() == docv1::BaseTextItem::kFieldValue,
          "text value is a field_value item");
  require_equal(document.texts(value_index).field_value().kind(), std::string("fillable"),
                "an editable field is fillable");
  require_equal(document.texts(value_index).field_value().base().text(),
                std::string("Jordan Example"), "value text");
  const auto& state = text_at(document, box.children(1).ref());
  require(state.label() == docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED, "/AS /Yes is selected");
  require_equal(state.text(), std::string("Yes"), "state name without its slash");
  require(box.source(0).collector().collector() == "grparse" &&
              box.source(0).collector().model() == "grpc-pdfium",
          "attributed to the backend engine");
}

void keeps_points_for_a_points_page() {
  // The inspector's fast path: a page in points with no size, boxes
  // bottom-left.
  docv1::Document document = base_document("form.pdf");
  (*document.mutable_pages())[1].set_page_no(1);
  (*document.mutable_pages())[1].set_unit("pt");
  add_paragraph(&document, "#/body", "text");
  add_prov(document.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1, 72,
           700, 300, 680, docv1::COORD_ORIGIN_BOTTOMLEFT);
  pdfv1::FormField off = check_box();
  off.set_appearance_state("/Off");
  off.clear_read_only();
  off.set_flags(0);
  grparse::fold_pdf_form_widgets({page_one({off})}, "grpc-qparse", &document);
  const auto& bbox = document.field_items(0).prov(0).bbox();
  require(bbox.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT, "points stay bottom-left");
  require_equal(bbox.l(), 300.0, "no scale on a points page");
  require_equal(bbox.t(), 265.0, "top is the upper y");
  require_equal(bbox.b(), 250.0, "bottom is the lower y");
  const auto& state = text_at(document, document.field_items(0).children(1).ref());
  require(state.label() == docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED, "/AS /Off is unselected");
  require_equal(document.body().children(1).ref(), std::string("#/field_regions/0"),
                "region after the page's text");
}

void read_only_values_and_bare_widgets() {
  docv1::Document document = cv_document();
  pdfv1::FormField locked = text_field();
  locked.set_flags(1);
  pdfv1::FormField push;
  push.set_kind(pdfv1::FORM_FIELD_KIND_PUSH_BUTTON);
  push.set_name("submit");
  push.mutable_rect()->set_x0(10);
  push.mutable_rect()->set_y0(10);
  push.mutable_rect()->set_x1(20);
  push.mutable_rect()->set_y1(20);
  grparse::fold_pdf_form_widgets({page_one({locked, push})}, "", &document);
  const int value_index = std::stoi(document.field_items(0).children(1).ref().substr(8));
  require_equal(document.texts(value_index).field_value().kind(), std::string("read_only"),
                "/Ff ReadOnly fixes the value");
  require(document.field_items(0).read_only(), "read-only from the flags alone");
  require_equal(document.field_items(1).children_size(), 1, "a push button has only its key");
}

void deterministic_whatever_the_backend_order() {
  docv1::Document first = cv_document();
  docv1::Document second = cv_document();
  grparse::fold_pdf_form_widgets({page_one({text_field(), check_box(), combo_box()})}, "x",
                                 &first);
  grparse::fold_pdf_form_widgets({page_one({combo_box(), check_box(), text_field()})}, "x",
                                 &second);
  require(same(first, second), "widget order on the wire does not change the document");
  docv1::Document again = cv_document();
  grparse::fold_pdf_form_widgets({page_one({text_field(), check_box(), combo_box()})}, "x",
                                 &again);
  require(grparse::render_canonical_json(first) == grparse::render_canonical_json(again),
          "same widgets, same canonical JSON");
}

void canonical_json_keeps_extensions_out() {
  docv1::Document document = cv_document();
  grparse::fold_pdf_form_widgets({page_one({text_field(), check_box()})}, "grpc-pdfium",
                                 &document);
  const std::string json = grparse::render_canonical_json(document);
  require(json.find("\"field_items\"") != std::string::npos, "field items are upstream");
  require(json.find("\"field_regions\"") != std::string::npos, "field regions are upstream");
  require(json.find("\"field_key\"") != std::string::npos, "keys render");
  require(json.find("\"fillable\"") != std::string::npos, "value kind renders");
  require(json.find("\"checkbox_selected\"") != std::string::npos, "check state renders");
  for (const char* extension : {"appearance_state", "field_flags", "field_kind", "read_only\":",
                                "description", "field_name", "/Yes"}) {
    require(json.find(extension) == std::string::npos,
            std::string("extension stays out of the canonical JSON: ") + extension);
  }
}

void nothing_to_fold() {
  docv1::Document document = cv_document();
  const docv1::Document before = document;
  grparse::fold_pdf_form_widgets({}, "x", &document);
  grparse::PdfPageWidgets empty = page_one({});
  grparse::fold_pdf_form_widgets({empty}, "x", &document);
  require(same(document, before), "no widgets leaves the document alone");
}

// A one-page backend answering the form-field family the way the fleet's
// backends do: header, then one page chunk, then the trailer.
class FormBackend final : public pdfv1::PdfBackendService::Service {
 public:
  pdfv1::FamilySupport support = pdfv1::FAMILY_SUPPORT_SUPPORTED;
  bool cache_required = false;
  bool cached = false;
  int parses = 0;

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest* request,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    ++parses;
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_backend_name("fake-forms");
    if (cache_required && !cached && request->document().data().empty()) {
      caps->set_load_status(pdfv1::LOAD_STATUS_BYTES_REQUIRED);
      writer->Write(header);
      return grpc::Status::OK;
    }
    if (!request->document().data().empty()) cached = true;
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(2);
    auto* verdict = caps->add_families();
    verdict->set_family(pdfv1::PDF_FAMILY_FORM_FIELDS);
    verdict->set_support(support);
    for (uint32_t index = 0; index < 2; ++index) {
      auto* info = header.mutable_header()->add_pages();
      info->set_page_index(index);
      info->set_width_pts(kWidthPts);
      info->set_height_pts(kHeightPts);
    }
    if (!writer->Write(header)) return grpc::Status::OK;
    if (support != pdfv1::FAMILY_SUPPORT_SUPPORTED) return grpc::Status::OK;
    require(request->families_size() == 1 &&
                request->families(0) == pdfv1::PDF_FAMILY_FORM_FIELDS,
            "the fetch asks for the form-field family alone");
    pdfv1::ParseResponse first;
    first.mutable_page()->set_page_index(0);
    writer->Write(first);
    pdfv1::ParseResponse second;
    second.mutable_page()->set_page_index(1);
    *second.mutable_page()->add_form_fields() = text_field();
    *second.mutable_page()->add_form_fields() = check_box();
    writer->Write(second);
    return grpc::Status::OK;
  }
};

std::string serve(FormBackend& backend, std::unique_ptr<grpc::Server>& server) {
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&backend);
  server = builder.BuildAndStart();
  require(server != nullptr && port != 0, "fake backend started");
  return "127.0.0.1:" + std::to_string(port);
}

std::chrono::system_clock::time_point soon() {
  return std::chrono::system_clock::now() + std::chrono::seconds(20);
}

void fetches_over_the_wire() {
  FormBackend backend;
  backend.cache_required = true;
  std::unique_ptr<grpc::Server> server;
  const std::string target = serve(backend, server);
  const auto fetch = grparse::fetch_pdf_form_widgets("%PDF-fake", target, soon());
  require(fetch.ok, "fetch succeeded: " + fetch.error);
  require_equal(backend.parses, 2, "a cache miss earns exactly one upload");
  require_equal(fetch.engine, std::string("fake-forms"), "engine from the header");
  require_equal(fetch.pages.size(), size_t{1}, "only pages with widgets");
  require_equal(fetch.pages[0].page_number, 2, "one-based page number");
  require_equal(fetch.pages[0].page_info.width_pts(), kWidthPts, "page info from the header");
  require_equal(fetch.pages[0].fields.size(), size_t{2}, "both widgets");
  server->Shutdown();
}

void absent_and_unsupported_families() {
  FormBackend absent;
  absent.support = pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT;
  std::unique_ptr<grpc::Server> server;
  std::string target = serve(absent, server);
  auto fetch = grparse::fetch_pdf_form_widgets("%PDF-fake", target, soon());
  require(fetch.ok && !fetch.unsupported && fetch.pages.empty(),
          "a document without a form is an answer with no widgets");
  server->Shutdown();

  FormBackend unsupported;
  unsupported.support = pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND;
  target = serve(unsupported, server);
  fetch = grparse::fetch_pdf_form_widgets("%PDF-fake", target, soon());
  require(fetch.ok && fetch.unsupported, "a backend without the family says so");
  server->Shutdown();
}

void folds_from_the_configured_backends() {
  FormBackend unsupported;
  unsupported.support = pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND;
  FormBackend good;
  std::unique_ptr<grpc::Server> first;
  std::unique_ptr<grpc::Server> second;
  const std::string no_forms = serve(unsupported, first);
  const std::string with_forms = serve(good, second);

  // Consensus-style list: the first target cannot read forms, the second
  // answers and wins.
  ::setenv("GRPARSE_PDF_BACKEND", (no_forms + "," + with_forms).c_str(), 1);
  docv1::Document document = cv_document();
  auto warning = grparse::fold_pdf_form_widgets_from_backend("%PDF-fake", soon(), &document);
  require(!warning.has_value(), "no warning when a backend answered");
  require_equal(document.field_items_size(), 2, "widgets folded from the second target");
  require_equal(document.field_items(0).prov(0).page_no(), 2, "page two widgets");
  require_equal(document.body().children(2).ref(), std::string("#/field_regions/0"),
                "page two's region after page two's text");

  // Every target failing leaves the document alone and says why.
  ::setenv("GRPARSE_PDF_BACKEND", "127.0.0.1:1", 1);
  docv1::Document untouched = cv_document();
  const docv1::Document before = untouched;
  warning = grparse::fold_pdf_form_widgets_from_backend("%PDF-fake", soon(), &untouched);
  require(warning.has_value() && warning->find("127.0.0.1:1") != std::string::npos,
          "the warning names the failed target");
  require(same(untouched, before), "a failed fetch changes nothing");

  // Unset: the in-process path has no forms surface; nothing happens.
  ::unsetenv("GRPARSE_PDF_BACKEND");
  warning = grparse::fold_pdf_form_widgets_from_backend("%PDF-fake", soon(), &untouched);
  require(!warning.has_value() && untouched.field_items_size() == 0,
          "no backend configured, no widgets and no warning");
  first->Shutdown();
  second->Shutdown();
}

// The fold measures widgets on the rendered page the way the page source
// measures text: a backend on the contract frame (PAGE_SPACE_CROP_BOX)
// reports rects already shifted by the CropBox origin, an older one
// reports unshifted user space, and either way, on every quarter turn,
// the widgets land on the same document boxes.
void widget_frames_agree_on_cropped_and_rotated_pages() {
  const std::vector<double> crop = {36, 48, 576, 756};
  for (const int rotation : {0, 90, 180, 270}) {
    const bool quarter_turn = rotation % 180 != 0;
    const double width = quarter_turn ? crop[3] - crop[1] : crop[2] - crop[0];
    const double height = quarter_turn ? crop[2] - crop[0] : crop[3] - crop[1];
    std::vector<docv1::Document> folded;
    for (const auto space : {pdfv1::PAGE_SPACE_UNSPECIFIED, pdfv1::PAGE_SPACE_CROP_BOX}) {
      grparse_test::ScopedPdfBackend pdf_backend;
      pdf_backend.backend().set_page_space(space);
      grparse_test::FakePdfPage page;
      page.rotation_degrees = rotation;
      page.crop_box = crop;
      page.form_fields = {text_field(), check_box(), combo_box()};
      const std::string bytes = "%PDF-widgets-" + std::to_string(rotation);
      pdf_backend.backend().add_document(bytes, {page});
      const auto fetch = grparse::fetch_pdf_form_widgets(bytes, pdf_backend.target(), soon());
      require(fetch.ok && fetch.pages.size() == 1, "widgets fetched: " + fetch.error);

      // The CV path's page: the rendered page at twice the points.
      docv1::Document document = base_document("form.pdf");
      add_page(&document, 1, width * 2, height * 2);
      add_paragraph(&document, "#/body", "page one text");
      add_prov(document.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1,
               10, 10, 50, 30);
      grparse::fold_pdf_form_widgets(fetch.pages, fetch.engine, &document);
      require_equal(document.field_items_size(), 3, "every widget folded");
      folded.push_back(std::move(document));
    }
    const std::string what = "/Rotate " + std::to_string(rotation);
    require(same(folded[0], folded[1]), what + ": both frames fold to the same document");

    // Pinned: the text field's rect (300, 300)-(450, 320) in user space.
    const docv1::FieldItem* text = nullptr;
    for (const auto& item : folded[1].field_items()) {
      if (item.field_name() == "customer_name") text = &item;
    }
    require(text != nullptr, what + ": the text field folded");
    const auto& bbox = text->prov(0).bbox();
    if (rotation == 0) {
      require_equal(bbox.l(), (300 - crop[0]) * 2, "upright: left from the CropBox edge");
      require_equal(bbox.t(), (crop[3] - 320) * 2, "upright: top from the CropBox top");
    } else if (rotation == 90) {
      // Reading order follows the turned page: the combo box (user x 72)
      // is now nearest the top.
      require_equal(folded[1].field_items(0).field_name(), std::string("form.country"),
                    "quarter turn: reading order as shown");
      // Turned clockwise: user y runs left to right across the page.
      require_equal(bbox.l(), (300 - crop[1]) * 2, "quarter turn: left from user y");
      require_equal(bbox.r(), (320 - crop[1]) * 2, "quarter turn: right from user y");
      require_equal(bbox.t(), (300 - crop[0]) * 2, "quarter turn: top from user x");
      require_equal(bbox.b(), (450 - crop[0]) * 2, "quarter turn: bottom from user x");
    }
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main(
      "pdf-form-widgets-test", "AcroForm widget fold passed",
      {folds_into_the_cv_page_space, keeps_points_for_a_points_page,
       read_only_values_and_bare_widgets, deterministic_whatever_the_backend_order,
       canonical_json_keeps_extensions_out, nothing_to_fold, fetches_over_the_wire,
       absent_and_unsupported_families, folds_from_the_configured_backends,
       widget_frames_agree_on_cropped_and_rotated_pages});
}
