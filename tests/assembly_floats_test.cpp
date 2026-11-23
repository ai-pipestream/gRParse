// Floats on a page: picture text, figure panels, panel and figure captions,
// and the line-break hyphen join. Split out of document-assembly-test to
// keep each test file small.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"
#include "grparse/document_assembly.h"
#include "grparse/line_joiner.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::OcrLine boxed_line(std::string text, int left, int top, int right, int bottom) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                          0.9F};
}

std::vector<std::string> body_refs(const ai::pipestream::parse::v1::PageData& data) {
  std::vector<std::string> order;
  for (const auto& ref : data.body_order()) order.push_back(ref.ref());
  return order;
}

// p772: the detector drew a text box around one End Bal amount, scoring it
// above the table. The amount belongs to its cell, not after the table's

// visual line) is a child text item of the picture, out of the body order,
// and tick values a column apart stay one item each.
void verify_picture_text_hangs_off_the_picture() {
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {boxed_line("Accuracy", 120, 200, 200, 215), boxed_line("(%)", 205, 200, 230, 215),
                         boxed_line("100", 300, 300, 330, 315), boxed_line("80", 500, 300, 520, 315),
                         boxed_line("Body", 100, 700, 160, 715)}};
  page.regions = {{"picture", 0.9F, 100, 150, 900, 400}, {"text", 0.9F, 90, 690, 900, 720}};

  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 1 && data.texts_size() == 4, "one picture, three labels and the body line");
  std::vector<std::string> labels;
  for (const auto& item : data.texts()) {
    if (item.text().base().parent().ref() == "#/pictures/0") labels.push_back(item.text().base().text());
  }
  require(labels == std::vector<std::string>{"Accuracy (%)", "100", "80"},
          "words of one label join, tick values stay apart, all under the picture");
  require(data.pictures(0).children_size() == 3, "the picture lists its labels as children");
  require(body_refs(data) == std::vector<std::string>{"#/pictures/0", "#/texts/3"},
          "picture labels stay out of the body order");
}

void verify_panels_hang_off_their_figure() {
  // A figure of two panels, each with its own "(a)" caption inside the
  // figure, and the figure caption below it all.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {boxed_line("(a) Train", 150, 420, 300, 435),
                         boxed_line("(b) Test", 550, 420, 700, 435),
                         boxed_line("Figure 2: Overview", 100, 460, 600, 475)}};
  page.regions = {{"picture", 0.9F, 100, 100, 900, 445},
                  {"picture", 0.9F, 110, 110, 490, 410},
                  {"picture", 0.9F, 510, 110, 890, 410},
                  {"caption", 0.9F, 140, 415, 310, 440},
                  {"caption", 0.9F, 540, 415, 710, 440},
                  {"caption", 0.9F, 90, 455, 610, 480}};
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 3, "the figure and both panels are pictures");
  std::string figure;
  for (const auto& picture : data.pictures()) {
    if (picture.parent().ref() == "#/body") {
      require(figure.empty(), "only the figure sits in the body");
      figure = picture.self_ref();
    }
  }
  size_t panels = 0;
  for (const auto& picture : data.pictures()) {
    if (picture.self_ref() == figure) continue;
    require(picture.parent().ref() == figure, "a panel hangs off its figure");
    require(picture.captions_size() == 1, "a panel keeps its own caption");
    ++panels;
  }
  require(panels == 2, "two panels");
  for (const auto& picture : data.pictures()) {
    if (picture.self_ref() != figure) continue;
    require(picture.captions_size() == 1, "the figure caption binds to the figure");
    const auto& caption_ref = picture.captions(0).ref();
    for (const auto& item : data.texts()) {
      if (item.text().base().self_ref() == caption_ref) {
        require(item.text().base().text() == "Figure 2: Overview", "the figure takes its own caption");
      }
    }
  }
  require(body_refs(data) == std::vector<std::string>{figure}, "panels and captions leave the body");
}

void verify_figure_caption_binds_to_the_figure_above() {
  // Next Token p10: "Figure 13" sits between Figure 13 and Figure 14, nearer
  // the one below; it labels the one above.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {boxed_line("Figure 13: Cross-dataset", 100, 312, 600, 327),
                         boxed_line("Figure 14: Few-shot", 100, 642, 600, 657)}};
  page.regions = {{"picture", 0.9F, 100, 100, 900, 300},
                  {"caption", 0.9F, 90, 310, 610, 330},
                  {"picture", 0.9F, 100, 335, 900, 630},
                  {"caption", 0.9F, 90, 640, 610, 660},
                  {"picture", 0.9F, 100, 665, 900, 950}};
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 3, "three figures");
  const auto caption_of = [&data](const ai::pipestream::document::v1::PictureItem& picture) {
    if (picture.captions_size() != 1) return std::string("(none)");
    for (const auto& item : data.texts()) {
      if (item.text().base().self_ref() == picture.captions(0).ref()) return item.text().base().text();
    }
    return std::string("(missing)");
  };
  require(caption_of(data.pictures(0)) == "Figure 13: Cross-dataset", "Figure 13 takes the caption under it");
  require(caption_of(data.pictures(1)) == "Figure 14: Few-shot", "Figure 14 takes the caption under it");
  require(data.pictures(2).captions_size() == 0, "the last figure has no caption on this page");
}

void verify_panel_label_is_the_panel_caption() {
  // Next Token p2: "(a) Augmentation recognition" has no layout region of
  // its own and falls into panel (a); it is that panel's caption.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {boxed_line("(a)", 150, 404, 175, 419), boxed_line("Augmentation", 180, 404, 330, 419),
                         boxed_line("recognition", 335, 404, 450, 419),
                         boxed_line("(b)Test", 550, 380, 650, 395),
                         boxed_line("(%)", 560, 200, 590, 215),
                         boxed_line("(1) Personal Validity", 120, 520, 300, 535),
                         boxed_line("(c) Figure 5:", 520, 120, 700, 135)}};
  page.regions = {{"picture", 0.9F, 100, 100, 900, 445},
                  {"picture", 0.9F, 110, 110, 490, 400},
                  {"picture", 0.9F, 510, 110, 890, 400},
                  {"picture", 0.9F, 100, 500, 900, 700}};  // a diagram, no panels
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  std::vector<std::string> captions;
  for (const auto& picture : data.pictures()) {
    for (const auto& ref : picture.captions()) {
      for (const auto& item : data.texts()) {
        if (item.text().base().self_ref() == ref.ref()) {
          require(item.text().base().label() == ai::pipestream::document::v1::DOC_ITEM_LABEL_CAPTION,
                  "a panel label is a caption");
          require(item.text().base().parent().ref() == picture.self_ref(), "and hangs off its panel");
          captions.push_back(item.text().base().text());
        }
      }
    }
  }
  std::ranges::sort(captions);
  require(captions == std::vector<std::string>{"(a) Augmentation recognition", "(b)Test"},
          "each panel takes its label, one just under its panel too; a unit, a number "
          "outside a panel and a figure caption stay picture text");
}

void verify_panel_caption_and_figure_caption_in_one_box_split() {
  // Attribution-Aware p5: one caption box holds panel (c)'s label and the
  // figure's own caption under it, both inside the figure.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1000, 1000,
                        {boxed_line("(c)", 520, 410, 545, 425), boxed_line("Average", 550, 410, 640, 425),
                         boxed_line("ARI", 645, 410, 690, 425),
                         boxed_line("Figure", 300, 440, 370, 455), boxed_line("2:", 375, 440, 395, 455),
                         boxed_line("Query", 400, 440, 470, 455)}};
  page.regions = {{"picture", 0.9F, 100, 100, 900, 460},
                  {"picture", 0.9F, 110, 110, 490, 400},
                  {"picture", 0.9F, 510, 110, 890, 400},
                  {"caption", 0.9F, 290, 405, 700, 458}};
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  const auto text_of = [&data](const std::string& ref) {
    for (const auto& item : data.texts()) {
      if (item.text().base().self_ref() == ref) return item.text().base().text();
    }
    return std::string("(missing)");
  };
  std::vector<std::string> figure_captions;
  std::vector<std::string> panel_captions;
  for (const auto& picture : data.pictures()) {
    for (const auto& ref : picture.captions()) {
      (picture.parent().ref() == "#/body" ? figure_captions : panel_captions).push_back(text_of(ref.ref()));
    }
  }
  require(figure_captions == std::vector<std::string>{"Figure 2: Query"}, "the figure takes its own caption");
  require(panel_captions == std::vector<std::string>{"(c) Average ARI"}, "the panel takes its label");
}

void verify_caption_a_body_line_under_its_figure_binds() {
  // Next Token p3: "Figure 3: Framework of NextHAR." sits 41 px under its
  // figure on a 2200 px page; a CC badge with a caption as far off does not
  // take it.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1700, 2200,
                        {boxed_line("Figure 3: Framework of NextHAR.", 300, 941, 900, 968),
                         boxed_line("Figure 4: Badge", 1300, 1975, 1600, 2002)}};
  grparse::LayoutRegion badge{"picture", 0.9F, 1300, 1800, 1600, 1900};
  badge.figure_classes = {{"icon", 0.9F}};
  page.regions = {{"picture", 0.9F, 200, 300, 1000, 900},
                  {"caption", 0.9F, 290, 936, 910, 972},
                  badge,
                  {"caption", 0.9F, 1290, 1970, 1610, 2006}};
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  require(data.pictures_size() == 2, "two pictures");
  require(data.pictures(0).captions_size() == 1, "the figure takes the caption a line under it");
  require(data.pictures(1).captions_size() == 0, "a badge keeps the tight reach");
}

void verify_titled_captions_bind_to_their_own_figure() {
  // Quantifying Event Impacts p8: "Figure 3" sits under a row of narrow
  // panels with no figure box, and over the wide figure that "Figure 4"
  // labels; the wide figure keeps only its own caption.
  grparse::AssemblyCursor cursor;
  grparse::OcrPage page{1700, 2200,
                        {boxed_line("Figure 3: Analysis", 200, 610, 1500, 637),
                         boxed_line("Figure 4: Prices", 200, 1010, 1500, 1037)}};
  page.regions = {{"picture", 0.9F, 200, 300, 600, 560},
                  {"picture", 0.9F, 650, 300, 1050, 560},
                  {"picture", 0.9F, 1100, 300, 1500, 560},
                  {"caption", 0.9F, 190, 605, 1510, 642},
                  {"picture", 0.9F, 200, 660, 1500, 980},
                  {"caption", 0.9F, 190, 1005, 1510, 1042}};
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(page, 1, &cursor, &data);
  const auto text_of = [&data](const std::string& ref) {
    for (const auto& item : data.texts()) {
      if (item.text().base().self_ref() == ref) return item.text().base().text();
    }
    return std::string("(missing)");
  };
  bool figure_four = false;
  for (const auto& picture : data.pictures()) {
    require(picture.captions_size() <= 1, "no figure holds two captions");
    if (picture.captions_size() == 1 && text_of(picture.captions(0).ref()) == "Figure 4: Prices") {
      figure_four = true;
    }
  }
  require(figure_four, "the wide figure keeps its own caption");

  // MTBTime p2: the right-hand panel reaches below its figure's box, nearer
  // the caption; "Figure 1" still labels the figure.
  grparse::AssemblyCursor second_cursor;
  grparse::OcrPage second{1700, 2200, {boxed_line("Figure 1: Backdoor", 520, 445, 880, 470)}};
  second.regions = {{"picture", 0.9F, 100, 100, 900, 400},
                    {"picture", 0.9F, 110, 110, 480, 390},
                    {"picture", 0.9F, 500, 150, 890, 430},
                    {"caption", 0.9F, 515, 440, 885, 475}};
  ai::pipestream::parse::v1::PageData second_data;
  grparse::append_page_data(second, 1, &second_cursor, &second_data);
  for (const auto& picture : second_data.pictures()) {
    const bool figure = picture.parent().ref() == "#/body";
    require(picture.captions_size() == (figure ? 1 : 0), "the figure, not its panel, takes Figure 1");
  }
}

void verify_line_break_hyphens_join_the_word() {
  // pdfium leaves U+FFFE where it took a hyphen for a line break; the ACM
  // sample read "nar\uFFFE ratives" in a quarter of its paragraphs.
  const std::string mark = "\xEF\xBF\xBE";
  grparse::LineJoiner prose;
  require(prose.add("telling nar" + mark) == 0, "first line starts at zero");
  require(prose.add("ratives and Sci" + mark + " entific") == 11, "a broken word joins with no separator");
  require(prose.add("COVID" + mark) == 34, "a whole line takes a separator");
  prose.add("19 cases");
  require(prose.text() == "telling narratives and Scientific COVID-19 cases",
          "marks go, and stay a hyphen before a digit");
  require(prose.code_points() == prose.text().size(), "code points follow the cleaned text");

  grparse::LineJoiner trailing;
  trailing.add("end" + mark);
  require(trailing.text() == "end", "a mark with no next line leaves no trace");

  grparse::LineJoiner code('\n');
  code.add("a = 1");
  code.add("b = 2");
  require(code.text() == "a = 1\nb = 2", "code lines keep their separator");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("assembly-floats-test", {
      verify_picture_text_hangs_off_the_picture,
      verify_line_break_hyphens_join_the_word,
      verify_panels_hang_off_their_figure,
      verify_figure_caption_binds_to_the_figure_above,
      verify_panel_label_is_the_panel_caption,
      verify_panel_caption_and_figure_caption_in_one_box_split,
      verify_caption_a_body_line_under_its_figure_binds,
      verify_titled_captions_bind_to_their_own_figure,
  });
}
