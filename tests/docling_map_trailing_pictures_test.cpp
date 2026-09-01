// S3 eval finding: a 200-page Writer document arrived with eight of its
// pictures appended after the last paragraph, a page-23 figure behind page
// 208's prose, because their anchors met no empty paragraph to take the
// place of. Once the stream is in, a picture that sits behind a later body
// item on the page plane is placed by its provenance, after the paragraph
// beside it, in page order. Pictures already in reading order stay put.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

constexpr long long kPageHeight = 16838;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

officev1::StreamPagesResponse info_event(int pages) {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("long.docx");
  info->set_source_format("docx");
  info->set_page_count(pages);
  info->set_document_type("text");
  for (int i = 0; i < pages; i++) {
    officev1::PageRect* page = info->add_page_rects();
    page->set_y_twips(i * kPageHeight);
    page->set_width_twips(11906);
    page->set_height_twips(kPageHeight);
  }
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

// A prose paragraph with a measured line box on `page`, `y` twips from the
// page's top edge.
officev1::StreamPagesResponse paragraph(const std::string& text, int page, long long y) {
  officev1::StreamPagesResponse event;
  officev1::Paragraph* item = event.mutable_paragraph();
  item->set_page_index(page);
  item->set_char_offset(0);
  item->set_outline_level(0);
  item->set_list_level(-1);
  const long long absolute = page * kPageHeight + y;
  item->mutable_start()->set_y(absolute);
  item->mutable_end()->set_y(absolute);
  officev1::TextRun* run = item->add_runs();
  run->set_text(text);
  run->set_char_offset(0);
  run->set_char_length(static_cast<long long>(text.size()));
  officev1::LineBox* line = item->add_line_rects();
  line->set_page_index(page);
  line->set_x_twips(1800);
  line->set_y_twips(absolute);
  line->set_width_twips(8000);
  line->set_height_twips(280);
  line->set_char_start(-1);
  line->set_char_end(-1);
  return event;
}

// A picture anchored beside prose (no empty paragraph of its own).
officev1::StreamPagesResponse picture(int index, int page, long long y, long long height) {
  officev1::StreamPagesResponse event;
  officev1::EmbeddedImage* image = event.mutable_embedded_image();
  image->set_index(index);
  image->set_page_index(page);
  image->set_name("Picture " + std::to_string(index + 1));
  image->set_mime_type("image/png");
  image->set_data("png");
  image->set_width_twips(4000);
  image->set_height_twips(height);
  image->mutable_anchor()->set_x(1800);
  image->mutable_anchor()->set_y(page * kPageHeight + y);
  return event;
}

docv1::Document fold(const std::vector<officev1::StreamPagesResponse>& events) {
  grparse::DoclingMapper mapper;
  for (const auto& event : events) mapper.consume(event);
  if (!mapper.finished()) throw std::runtime_error("the stream did not finish");
  return mapper.document();
}

std::vector<std::string> body_order(const docv1::Document& document) {
  std::vector<std::string> refs;
  for (const auto& child : document.body().children()) refs.push_back(child.ref());
  return refs;
}

std::string joined(const std::vector<std::string>& refs) {
  std::string out;
  for (const auto& ref : refs) out += ref + " ";
  return out;
}

// Three pages of prose; two pictures arrive after every paragraph, one
// beside page 1's second paragraph and one beside page 3's first. Without
// placement both would trail page 3's last paragraph.
void verify_trailing_pictures_are_placed_by_page_and_position() {
  std::vector<officev1::StreamPagesResponse> events{info_event(3)};
  events.push_back(paragraph("Page one, first.", 0, 2000));
  events.push_back(paragraph("Page one, second.", 0, 6000));
  events.push_back(paragraph("Page two, first.", 1, 2000));
  events.push_back(paragraph("Page three, first.", 2, 2000));
  events.push_back(paragraph("Page three, second.", 2, 9000));
  events.push_back(picture(0, 2, 2500, 3000));
  events.push_back(picture(1, 0, 6100, 3000));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const std::vector<std::string> order = body_order(document);
  const std::vector<std::string> expected = {"#/texts/0", "#/texts/1", "#/pictures/1", "#/texts/2",
                                             "#/texts/3", "#/pictures/0", "#/texts/4"};
  require(order == expected, "pictures sit after the paragraph beside them, in page order: " + joined(order));
  require(grparse::docling_integrity_errors(document).empty(), "the placement keeps the document well formed");
  require(document.pictures(1).prov(0).page_no() == 1 && document.pictures(0).prov(0).page_no() == 3,
          "the pictures keep their own provenance");
}

// A picture the stream already delivered in reading order is not moved.
void verify_pictures_in_order_stay_where_the_fold_put_them() {
  std::vector<officev1::StreamPagesResponse> events{info_event(2)};
  events.push_back(paragraph("Page one, first.", 0, 2000));
  events.push_back(picture(0, 0, 2400, 3000));
  events.push_back(paragraph("Page one, after the figure.", 0, 6000));
  events.push_back(paragraph("Page two.", 1, 2000));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const std::vector<std::string> order = body_order(document);
  require(order == std::vector<std::string>{"#/texts/0", "#/pictures/0", "#/texts/1", "#/texts/2"},
          "an in-order picture is left alone: " + joined(order));
}

// A picture that took an empty paragraph's place keeps it even when its
// top edge rises above the prose before it: the anchor paragraph is the
// document's own word on where the figure belongs.
void verify_slotted_pictures_keep_their_paragraphs_place() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(paragraph("Before the figure.", 0, 4000));
  {
    officev1::StreamPagesResponse empty;
    officev1::Paragraph* item = empty.mutable_paragraph();
    item->set_page_index(0);
    item->set_char_offset(0);
    item->set_outline_level(0);
    item->set_list_level(-1);
    item->mutable_start()->set_y(6000);
    item->mutable_end()->set_y(6000);
    events.push_back(empty);
  }
  events.push_back(paragraph("After the figure.", 0, 9000));
  events.push_back(picture(0, 0, 3500, 3000));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const std::vector<std::string> order = body_order(document);
  // The empty anchor paragraph emits no text item of its own, so the body
  // is the prose before, the picture in the paragraph's place, the prose after.
  require(order == std::vector<std::string>{"#/texts/0", "#/pictures/0", "#/texts/1"},
          "a slotted picture stays in its anchor paragraph's place: " + joined(order));
}

// Two trailing pictures from the same page keep their vertical order.
void verify_two_trailing_pictures_on_one_page_keep_their_order() {
  std::vector<officev1::StreamPagesResponse> events{info_event(2)};
  events.push_back(paragraph("Page one.", 0, 1500));
  events.push_back(paragraph("Page two.", 1, 1500));
  events.push_back(picture(0, 0, 9000, 2000));
  events.push_back(picture(1, 0, 3000, 2000));
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const std::vector<std::string> order = body_order(document);
  require(order == std::vector<std::string>{"#/texts/0", "#/pictures/1", "#/pictures/0", "#/texts/1"},
          "same-page pictures follow their tops: " + joined(order));
}

}  // namespace

int main() {
  try {
    verify_trailing_pictures_are_placed_by_page_and_position();
    verify_pictures_in_order_stay_where_the_fold_put_them();
    verify_slotted_pictures_keep_their_paragraphs_place();
    verify_two_trailing_pictures_on_one_page_keep_their_order();
  } catch (const std::exception& error) {
    std::println(stderr, "docling_map_trailing_pictures_test: {}", error.what());
    return 1;
  }
  std::println("docling_map_trailing_pictures_test: ok");
  return 0;
}
