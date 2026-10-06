// S3 eval findings on the NapierOne office files (doc, docx, ppt, pptx):
// most office documents failed the structural checks for reasons in the
// fold, not in the documents. Page headers and footers carried no page, so
// every header line failed provenance_present; an object anchored in a
// header streamed once per page, each copy with a box pages away from the
// page it named; a long table's caret box spanned the gap between pages; a
// frame's anchor plus size ran off its page. On decks, blank outline lines
// became empty text items, top-level bullets became section headings, a
// later slide's title became the deck title, every slide picture appeared
// twice, a deck's spreadsheet objects sat before the first slide, and
// shapes kept paint order rather than reading order. Each case below is a
// hand-built collector stream reproducing one finding.

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

constexpr long long kPageWidth = 11906;
constexpr long long kPageHeight = 16838;
constexpr long long kSlideWidth = 14400;
constexpr long long kSlideHeight = 10800;

using grparse_test::require;

officev1::StreamPagesResponse text_info(int pages) {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("report.doc");
  info->set_source_format("doc");
  info->set_page_count(pages);
  info->set_document_type("text");
  for (int i = 0; i < pages; i++) {
    officev1::PageRect* page = info->add_page_rects();
    page->set_y_twips(i * kPageHeight);
    page->set_width_twips(kPageWidth);
    page->set_height_twips(kPageHeight);
  }
  return event;
}

officev1::StreamPagesResponse deck_info(int slides) {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("deck.pptx");
  info->set_source_format("pptx");
  info->set_page_count(slides);
  info->set_document_type("presentation");
  for (int i = 0; i < slides; i++) {
    officev1::PageRect* page = info->add_page_rects();
    page->set_width_twips(kSlideWidth);
    page->set_height_twips(kSlideHeight);
  }
  return event;
}

officev1::StreamPagesResponse page_image(int index, const std::string& style) {
  officev1::StreamPagesResponse event;
  officev1::PageImage* image = event.mutable_page_image();
  image->set_index(index);
  image->set_width_px(794);
  image->set_height_px(1123);
  image->set_dpi(96);
  image->set_png("png");
  image->set_format(officev1::PAGE_IMAGE_FORMAT_PNG);
  image->set_page_style(style);
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

officev1::TextRun run(const std::string& text) {
  officev1::TextRun out;
  out.set_text(text);
  out.set_char_offset(-1);
  out.set_char_length(static_cast<long long>(text.size()));
  return out;
}

officev1::StreamPagesResponse paragraph(const std::string& text, int page, long long y) {
  officev1::StreamPagesResponse event;
  officev1::Paragraph* item = event.mutable_paragraph();
  item->set_page_index(page);
  item->set_char_offset(0);
  item->set_list_level(-1);
  item->mutable_start()->set_x(1440);
  item->mutable_start()->set_y(page * kPageHeight + y);
  item->mutable_end()->set_x(9000);
  item->mutable_end()->set_y(page * kPageHeight + y + 280);
  *item->add_runs() = run(text);
  return event;
}

officev1::StreamPagesResponse slide(int index) {
  officev1::StreamPagesResponse event;
  event.mutable_slide()->set_index(index);
  event.mutable_slide()->set_name("Slide " + std::to_string(index + 1));
  return event;
}

officev1::StreamPagesResponse slide_shape(int slide_index, int z_order,
                                          const std::string& type,
                                          officev1::PlaceholderRole role,
                                          long long x, long long y,
                                          long long width, long long height,
                                          const std::vector<std::string>& lines) {
  officev1::StreamPagesResponse event;
  officev1::SlideShape* shape = event.mutable_slide_shape();
  shape->set_slide_index(slide_index);
  shape->set_z_order(z_order);
  shape->set_shape_type(type);
  shape->set_placeholder_role(role);
  shape->mutable_position()->set_x(x);
  shape->mutable_position()->set_y(y);
  shape->set_width_twips(width);
  shape->set_height_twips(height);
  for (const std::string& line : lines) {
    *shape->add_paragraphs()->add_runs() = run(line);
  }
  return event;
}

docv1::Document fold(const std::vector<officev1::StreamPagesResponse>& events) {
  grparse::DoclingMapper mapper;
  for (const auto& event : events) mapper.consume(event);
  require(mapper.finished(), "the stream finished");
  return mapper.document();
}

const docv1::TextItemBase* text_base(const docv1::Document& document, int index) {
  const docv1::BaseTextItem& item = document.texts(index);
  if (item.has_title()) return &item.title().base();
  if (item.has_section_header()) return &item.section_header().base();
  if (item.has_list_item()) return &item.list_item().base();
  return &item.text().base();
}

// A header's lines sit on every page laid out in the header's page style,
// and nowhere else; a blank header line is no item.
void verify_headers_sit_on_the_pages_of_their_style() {
  std::vector<officev1::StreamPagesResponse> events{text_info(3)};
  events.push_back(page_image(0, "First Page"));
  events.push_back(page_image(1, "Standard"));
  events.push_back(page_image(2, "Standard"));
  officev1::StreamPagesResponse standard;
  officev1::HeaderFooter* block = standard.mutable_header_footer();
  block->set_page_style("Standard");
  *block->add_paragraphs()->add_runs() = run("Annual report");
  *block->add_paragraphs()->add_runs() = run(" ");
  events.push_back(standard);
  officev1::StreamPagesResponse unused;
  unused.mutable_header_footer()->set_page_style("Convert 1");
  *unused.mutable_header_footer()->add_paragraphs()->add_runs() = run("never shown");
  events.push_back(unused);
  events.push_back(status_event());
  const docv1::Document document = fold(events);

  require(document.texts_size() == 1, "one non-blank header line of a used page style");
  const docv1::TextItemBase* header = text_base(document, 0);
  require(header->text() == "Annual report" &&
              header->label() == docv1::DOC_ITEM_LABEL_PAGE_HEADER,
          "the header line is a page header");
  require(header->prov_size() == 2 && header->prov(0).page_no() == 2 &&
              header->prov(1).page_no() == 3,
          "the header sits on the two Standard pages, not the first page");
  require(!header->prov(0).has_bbox(), "where on the page is not claimed");
  require(grparse::docling_integrity_errors(document).empty(), "headers stay well formed");
}

// A picture anchored in a header streams once per page, every copy the
// same event. One picture remains, in the furniture, with its page and no
// box.
void verify_header_object_copies_fold_to_one_furniture_picture() {
  std::vector<officev1::StreamPagesResponse> events{text_info(4)};
  officev1::StreamPagesResponse image;
  officev1::EmbeddedImage* picture = image.mutable_embedded_image();
  picture->set_page_index(0);
  picture->set_name("Logo");
  picture->set_mime_type("image/png");
  picture->set_data("png");
  picture->set_width_twips(2000);
  picture->set_height_twips(800);
  picture->mutable_anchor()->set_x(1440);
  // The caret names page 1, the box sits three pages down.
  picture->mutable_anchor()->set_y(3 * kPageHeight + 700);
  for (int copy = 0; copy < 4; copy++) events.push_back(image);
  events.push_back(status_event());
  const docv1::Document document = fold(events);

  require(document.pictures_size() == 1, "four copies of one header picture fold to one");
  const docv1::PictureItem& logo = document.pictures(0);
  require(logo.content_layer() == docv1::CONTENT_LAYER_FURNITURE &&
              logo.parent().ref() == "#/furniture",
          "the repeated picture is furniture");
  require(document.body().children_size() == 0, "and is not in the body");
  require(logo.prov_size() == 1 && logo.prov(0).page_no() >= 1 && !logo.prov(0).has_bbox(),
          "it keeps a page and claims no box");
  require(grparse::docling_integrity_errors(document).empty(), "the move stays well formed");
}

// A picture pushed to the next page keeps its anchor paragraph's caret on
// the page before: the page is the layout's, the caret box is not, so the
// picture keeps its page, claims no box, and reads before that page's
// first item.
void verify_box_off_its_page_keeps_the_page_only() {
  std::vector<officev1::StreamPagesResponse> events{text_info(3)};
  events.push_back(paragraph("End of page two.", 1, 14000));
  events.push_back(paragraph("Top of page three.", 2, 9000));
  officev1::StreamPagesResponse image;
  officev1::EmbeddedImage* picture = image.mutable_embedded_image();
  picture->set_page_index(2);
  picture->set_name("Figure");
  picture->set_mime_type("image/png");
  picture->set_data("png");
  picture->set_width_twips(4000);
  picture->set_height_twips(8000);
  picture->mutable_anchor()->set_x(1440);
  picture->mutable_anchor()->set_y(2 * kPageHeight - 6000);
  events.push_back(image);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::ProvenanceItem& prov = document.pictures(0).prov(0);
  require(prov.page_no() == 3, "the picture stays on the page the layout put it on");
  require(!prov.has_bbox(), "and claims no box built from the wrong caret");
  const auto& body = document.body().children();
  require(body.size() == 3 && body[0].ref() == "#/texts/0" &&
              body[1].ref() == "#/pictures/0" && body[2].ref() == "#/texts/1",
          "the picture reads before the first item of its page");
}

officev1::StreamPagesResponse anchored_picture(const std::string& name, int page,
                                               long long anchor_y) {
  officev1::StreamPagesResponse event;
  officev1::EmbeddedImage* picture = event.mutable_embedded_image();
  picture->set_page_index(page);
  picture->set_name(name);
  picture->set_mime_type("image/png");
  picture->set_data(name);
  picture->set_width_twips(9000);
  picture->set_height_twips(8000);
  picture->mutable_anchor()->set_x(1440);
  picture->mutable_anchor()->set_y(anchor_y);
  return event;
}

// An annex of one picture per page, each in a blank paragraph of its own,
// reads in page order; so does a picture between them whose anchor caret
// stood on the page before and keeps only its page.
void verify_pictures_on_blank_pages_keep_page_order() {
  std::vector<officev1::StreamPagesResponse> events{text_info(4)};
  events.push_back(paragraph("Annex.", 0, 1440));
  events.push_back(paragraph("", 1, 1440));
  events.push_back(paragraph("", 3, 1440));
  events.push_back(anchored_picture("one", 1, kPageHeight + 1440));
  events.push_back(anchored_picture("two", 2, kPageHeight + 9000));
  events.push_back(anchored_picture("three", 3, 3 * kPageHeight + 1440));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  std::vector<std::string> order;
  for (const docv1::RefItem& child : document.body().children()) order.push_back(child.ref());
  require(order == std::vector<std::string>{"#/texts/0", "#/pictures/0", "#/pictures/1",
                                            "#/pictures/2"},
          "the pictures read in page order after the annex heading");
}

// A logo anchored to a paragraph that has text reads right after that
// paragraph, not in a blank paragraph further down.
void verify_picture_reads_after_its_anchor_paragraph() {
  std::vector<officev1::StreamPagesResponse> events{text_info(1)};
  events.push_back(paragraph("Job description", 0, 1440));
  events.push_back(paragraph("Second line.", 0, 2000));
  events.push_back(paragraph("", 0, 2600));
  events.push_back(anchored_picture("logo", 0, 1440));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const auto& body = document.body().children();
  require(body.size() >= 2 && body[0].ref() == "#/texts/0" && body[1].ref() == "#/pictures/0",
          "the logo follows its anchor paragraph");
}

// A logo anchored in the page header is furniture, kept once however many
// pages repeat it, and never reads in the body after the title.
void verify_header_anchored_picture_is_furniture() {
  std::vector<officev1::StreamPagesResponse> events{text_info(2)};
  events.push_back(paragraph("Agenda", 0, 1505));
  for (int page = 0; page < 2; page++) {
    officev1::StreamPagesResponse logo = anchored_picture("logo", page, page * kPageHeight + 686);
    logo.mutable_embedded_image()->set_in_header_footer(true);
    events.push_back(logo);
  }
  officev1::StreamPagesResponse box;
  officev1::TextFrame* frame = box.mutable_text_frame();
  frame->set_name("HeaderBox");
  frame->set_page_index(0);
  frame->set_width_twips(4000);
  frame->set_height_twips(500);
  frame->mutable_anchor()->set_x(1440);
  frame->mutable_anchor()->set_y(700);
  frame->set_in_header_footer(true);
  *frame->add_runs() = run("Letterhead");
  events.push_back(box);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.pictures_size() == 1, "the repeated logo is kept once");
  require(document.pictures(0).parent().ref() == "#/furniture" &&
              document.pictures(0).content_layer() == docv1::CONTENT_LAYER_FURNITURE,
          "the logo is furniture");
  const auto& body = document.body().children();
  require(body.size() == 1 && body[0].ref() == "#/texts/0",
          "the body holds only the title");
  bool frame_in_furniture = false;
  for (int i = 0; i < document.texts_size(); i++) {
    const docv1::TextItemBase* base = text_base(document, i);
    if (base->text() == "Letterhead") {
      frame_in_furniture = base->content_layer() == docv1::CONTENT_LAYER_FURNITURE;
    }
  }
  require(frame_in_furniture, "a header text frame is furniture too");
}

// A frame whose anchor plus size overruns its page reports the part on
// the page.
void verify_frame_overrunning_its_page_is_clipped() {
  std::vector<officev1::StreamPagesResponse> events{text_info(1)};
  officev1::StreamPagesResponse event;
  officev1::TextFrame* frame = event.mutable_text_frame();
  frame->set_name("Sidebar");
  frame->set_page_index(0);
  frame->set_width_twips(6000);
  frame->set_height_twips(4000);
  frame->mutable_anchor()->set_x(9000);
  frame->mutable_anchor()->set_y(15000);
  *frame->add_runs() = run("Key facts");
  events.push_back(event);
  // A frame with no text makes no item.
  officev1::StreamPagesResponse empty;
  empty.mutable_text_frame()->set_name("Border");
  empty.mutable_text_frame()->set_width_twips(100);
  empty.mutable_text_frame()->set_height_twips(100);
  events.push_back(empty);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.texts_size() == 1, "the textless frame makes no text item");
  const docv1::BoundingBox& box = text_base(document, 0)->prov(0).bbox();
  require(box.r() == kPageWidth && box.b() == kPageHeight,
          "the box stops at the page's right and bottom edges");
  require(box.l() == 9000 && box.t() == 15000, "and keeps its origin");
}

// A table whose carets stand on two pages gets one box per page.
void verify_caret_box_splits_across_pages() {
  std::vector<officev1::StreamPagesResponse> events{text_info(2)};
  officev1::StreamPagesResponse event;
  officev1::TableData* table = event.mutable_table();
  table->set_page_index(0);
  table->set_rows(1);
  table->set_columns(1);
  officev1::TableCellData* cell = table->add_cells();
  cell->set_text("x");
  table->mutable_start()->set_x(1440);
  table->mutable_start()->set_y(12000);
  table->mutable_end()->set_x(10000);
  table->mutable_end()->set_y(kPageHeight + 3000);
  events.push_back(event);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const auto& prov = document.tables(0).prov();
  require(prov.size() == 2, "one box on each page the table runs over");
  require(prov[0].page_no() == 1 && prov[0].bbox().t() == 12000 &&
              prov[0].bbox().b() == kPageHeight,
          "from the start caret to the first page's foot");
  require(prov[1].page_no() == 2 && prov[1].bbox().t() == 0 && prov[1].bbox().b() == 3000,
          "and from the next page's head to the end caret");
}

// Outline lines are bullets: blank lines make nothing and top-level lines
// are list items, not section headings.
void verify_outline_lines_are_list_items() {
  std::vector<officev1::StreamPagesResponse> events{deck_info(1), slide(0)};
  events.push_back(slide_shape(0, 0, "com.sun.star.presentation.OutlinerShape",
                               officev1::PLACEHOLDER_ROLE_OUTLINE, 720, 2000, 12000, 6000,
                               {"First point", "", " ", "Second point"}));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.texts_size() == 2, "two non-blank outline lines");
  for (int i = 0; i < 2; i++) {
    require(document.texts(i).has_list_item(), "an outline line is a list item");
  }
}

// The deck title is the first slide's title; a deck whose first slide has
// none has no title, and later slide titles are section headings.
void verify_deck_title_only_from_the_first_slide() {
  std::vector<officev1::StreamPagesResponse> events{deck_info(2), slide(0)};
  events.push_back(slide_shape(0, 0, "com.sun.star.drawing.CustomShape",
                               officev1::PLACEHOLDER_ROLE_NONE, 720, 2000, 12000, 2000,
                               {"Cover"}));
  events.push_back(slide(1));
  events.push_back(slide_shape(1, 0, "com.sun.star.presentation.TitleTextShape",
                               officev1::PLACEHOLDER_ROLE_TITLE, 720, 400, 12000, 1200,
                               {"Agenda"}));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  for (const docv1::BaseTextItem& item : document.texts()) {
    require(!item.has_title(), "no title outside the first slide");
  }
  require(document.texts(1).has_section_header(), "the second slide's title heads a section");
}

// A slide picture streams as a graphic shape and then as its image; the
// two are one picture, carrying the image.
void verify_slide_picture_is_not_doubled() {
  std::vector<officev1::StreamPagesResponse> events{deck_info(1), slide(0)};
  events.push_back(slide_shape(0, 0, "com.sun.star.drawing.GraphicObjectShape",
                               officev1::PLACEHOLDER_ROLE_NONE, 1076, 5918, 6404, 2489, {""}));
  officev1::StreamPagesResponse image;
  officev1::EmbeddedImage* picture = image.mutable_embedded_image();
  picture->set_page_index(0);
  picture->set_name("Content Placeholder 3");
  picture->set_mime_type("image/jpeg");
  picture->set_data("jpeg");
  picture->set_width_twips(6404);
  picture->set_height_twips(2489);
  picture->mutable_anchor()->set_x(1076);
  picture->mutable_anchor()->set_y(5918);
  events.push_back(image);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.pictures_size() == 1, "one picture for the shape and its image");
  require(document.pictures(0).image().mimetype() == "image/jpeg" &&
              document.pictures(0).shape().name() == "Content Placeholder 3",
          "the picture carries the image and its name");
  require(grparse::docling_integrity_errors(document).empty(), "the deck stays well formed");
}

// A deck's spreadsheet object streams before the slides; its OLE2 shape
// places it on its slide, as one table, not a table plus a picture.
void verify_slide_spreadsheet_object_sits_on_its_slide() {
  std::vector<officev1::StreamPagesResponse> events{deck_info(1)};
  officev1::StreamPagesResponse object_event;
  officev1::EmbeddedObject* object = object_event.mutable_embedded_object();
  object->set_kind(officev1::EMBEDDED_OBJECT_KIND_SPREADSHEET);
  object->set_page_index(0);
  object->mutable_position()->set_x(120);
  object->mutable_position()->set_y(360);
  object->set_width_twips(14000);
  object->set_height_twips(9000);
  object->mutable_inner_table()->set_rows(1);
  object->mutable_inner_table()->set_columns(1);
  object->mutable_inner_table()->add_cells()->set_text("42");
  events.push_back(object_event);
  events.push_back(slide(0));
  events.push_back(slide_shape(0, 0, "com.sun.star.drawing.OLE2Shape",
                               officev1::PLACEHOLDER_ROLE_NONE, 120, 360, 14000, 9000, {""}));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.tables_size() == 1 && document.pictures_size() == 0,
          "the object is one table and no placeholder picture");
  require(document.tables(0).parent().ref() == document.groups(0).self_ref(),
          "the table sits under its slide");
  require(document.body().children_size() == 1, "the body holds only the slide");
  require(grparse::docling_integrity_errors(document).empty(), "the deck stays well formed");
}

// Slide shapes stream in paint order; the slide reads top to bottom, and
// left to right along a row. A shape past the slide's edge reports the
// visible part.
void verify_slide_reads_in_geometric_order() {
  std::vector<officev1::StreamPagesResponse> events{deck_info(1), slide(0)};
  events.push_back(slide_shape(0, 0, "com.sun.star.drawing.CustomShape",
                               officev1::PLACEHOLDER_ROLE_NONE, 720, 8000, 12000, 1000,
                               {"Footer note"}));
  events.push_back(slide_shape(0, 1, "com.sun.star.drawing.CustomShape",
                               officev1::PLACEHOLDER_ROLE_NONE, 7600, 3000, 9000, 2000,
                               {"Right"}));
  events.push_back(slide_shape(0, 2, "com.sun.star.drawing.CustomShape",
                               officev1::PLACEHOLDER_ROLE_NONE, 720, 3100, 6000, 2000,
                               {"Left"}));
  events.push_back(slide_shape(0, 3, "com.sun.star.presentation.TitleTextShape",
                               officev1::PLACEHOLDER_ROLE_TITLE, 720, 400, 12000, 1200,
                               {"Heading"}));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  std::vector<std::string> order;
  for (const docv1::RefItem& child : document.groups(0).children()) {
    const int index = std::stoi(child.ref().substr(std::string("#/texts/").size()));
    order.push_back(text_base(document, index)->text());
  }
  require(order == std::vector<std::string>{"Heading", "Left", "Right", "Footer note"},
          "title, the row left to right, then the note below");
  const docv1::BoundingBox& right = text_base(document, 1)->prov(0).bbox();
  require(right.r() == kSlideWidth, "the shape past the slide's right edge is cut there");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("docling_map_office_layout_test", "ok", {
      verify_headers_sit_on_the_pages_of_their_style,
      verify_header_object_copies_fold_to_one_furniture_picture,
      verify_box_off_its_page_keeps_the_page_only,
      verify_frame_overrunning_its_page_is_clipped,
      verify_caret_box_splits_across_pages,
      verify_outline_lines_are_list_items,
      verify_deck_title_only_from_the_first_slide,
      verify_slide_picture_is_not_doubled,
      verify_slide_spreadsheet_object_sits_on_its_slide,
      verify_slide_reads_in_geometric_order,
      verify_pictures_on_blank_pages_keep_page_order,
      verify_picture_reads_after_its_anchor_paragraph,
      verify_header_anchored_picture_is_furniture,
  });
}
