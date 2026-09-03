// The LaTeX export, unit by unit: the escape map, the title hoist and
// preamble, the section-command mapping and its clamp, list environments and
// their indent, code and formula shapes, the floated tabular, the picture
// placeholder, and the placeholder comments for unmappable items. Cross-
// renderer properties (purity, layer exclusion, captions rendering once)
// live in render_determinism_test.cpp and document_render_test.cpp.

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_group;
using grparse_test::add_heading;
using grparse_test::add_owned_text;
using grparse_test::add_paragraph;
using grparse_test::add_picture;
using grparse_test::add_table;
using grparse_test::add_text;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

// The fixed preamble every export opens with; the title line joins it ahead
// of the blank line before \begin{document} when the document has one.
const std::string kPreamble =
    "\\documentclass[11pt,a4paper]{article}\n"
    "\n"
    "\\usepackage[utf8]{inputenc} % allow utf-8 input\n"
    "\\usepackage[T1]{fontenc}    % use 8-bit T1 fonts\n"
    "\\usepackage{hyperref}       % hyperlinks\n"
    "\\usepackage{url}            % simple URL typesetting\n"
    "\\usepackage{booktabs}       % professional-quality tables\n"
    "\\usepackage{amsfonts}       % blackboard math symbols\n"
    "\\usepackage{nicefrac}       % compact symbols for 1/2, etc.\n"
    "\\usepackage{microtype}      % microtypography\n"
    "\\usepackage{xcolor}         % colors\n"
    "\\usepackage{graphicx}       % graphics\n"
    "\\usepackage[normalem]{ulem} % strikethrough\n";

std::string wrapped(const std::string& body) {
  return kPreamble + "\n\\begin{document}\n\n" + body + "\n\n\\end{document}";
}

void verify_an_empty_document_is_the_bare_skeleton() {
  require_equal(grparse::render_latex(base_document("empty.pdf")),
                kPreamble + "\n\\begin{document}\n\n\\end{document}",
                "an empty body renders the preamble around an empty document environment");
}

void verify_escaping_covers_every_special_character() {
  docv1::Document document = base_document("escape.tex");
  add_paragraph(&document, "#/body", "100% of {a_b} & $x #y \\ ~home ^2");
  require_equal(grparse::render_latex(document),
                wrapped("100\\% of \\{a\\_b\\} \\& \\$x \\#y \\textbackslash{} "
                        "\\textasciitilde{}home \\textasciicircum{}2"),
                "every LaTeX special character escapes, char by char");
}

void verify_the_title_hoists_into_the_preamble() {
  docv1::Document document = base_document("titled.tex");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
           "Quarterly Report");
  add_paragraph(&document, "#/body", "Body prose.");

  const std::string rendered = grparse::render_latex(document);
  require_equal(rendered,
                kPreamble + "\\title{Quarterly Report}\n"
                            "\n\\begin{document}\n"
                            "\n\\maketitle\n"
                            "\nBody prose.\n"
                            "\n\\end{document}",
                "the title moves to the preamble and \\maketitle opens the body");
  require_equal(rendered.find("Quarterly Report"), rendered.rfind("Quarterly Report"),
                "the title text renders once, in the preamble only");
}

void verify_section_headers_map_by_level_and_clamp() {
  docv1::Document document = base_document("levels.tex");
  add_heading(&document, "#/body", "Chapter", 1);
  add_heading(&document, "#/body", "Section", 2);
  add_heading(&document, "#/body", "Sub", 3);
  // The reference raises outside [1, 3]; the export clamps instead.
  add_heading(&document, "#/body", "Deep", 7);
  // An unset level counts as 1.
  add_heading(&document, "#/body", "Unset", 0);

  require_equal(grparse::render_latex(document),
                wrapped("\\section{Chapter}\n\n\\subsection{Section}\n\n"
                        "\\subsubsection{Sub}\n\n\\subsubsection{Deep}\n\n\\section{Unset}"),
                "levels 1-3 map to section/subsection/subsubsection and the rest clamp");
}

void verify_lists_choose_their_environment_and_indent() {
  docv1::Document document = base_document("lists.tex");
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "beta");
  const std::string nested =
      add_group(&document, list, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "one", 0, true);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "two", 0, true);

  require_equal(grparse::render_latex(document),
                wrapped("\\begin{itemize}\n"
                        "\\item alpha\n"
                        "\\item beta\n"
                        "  \\begin{enumerate}\n"
                        "\\item one\n"
                        "\\item two\n"
                        "  \\end{enumerate}\n"
                        "\\end{itemize}"),
                "the first item's enumerated flag picks the environment and nested "
                "lists indent by two spaces per level");
}

// A list item the producer left outside any list group is re-homed into a
// synthesized one by the load normalization, exactly like the reference.
void verify_a_stray_list_item_gets_a_list() {
  docv1::Document document = base_document("stray.tex");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "orphan");
  require_equal(grparse::render_latex(document),
                wrapped("\\begin{itemize}\n\\item orphan\n\\end{itemize}"),
                "a stray list item renders inside a synthesized itemize");
}

void verify_a_table_renders_as_a_floated_tabular() {
  docv1::Document document = base_document("table.tex");
  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(
      add_owned_text(&document, table->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION,
                     "Fuel & rates"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  add_cell(data, nullptr, "Fuel", true, 0, 0);
  add_cell(data, nullptr, "Rate_per_mile", true, 0, 1);
  add_cell(data, nullptr, "Diesel", false, 1, 0);
  add_cell(data, nullptr, "0.9\navg", false, 1, 1);

  require_equal(grparse::render_latex(document),
                wrapped("\\begin{table}[h]\n"
                        "\\caption{Fuel \\& rates}\n"
                        "\\begin{tabular}{|l|l|}\n"
                        "\\hline\n"
                        "Fuel & Rate\\_per\\_mile \\\\ \\hline\n"
                        "Diesel & 0.9 avg \\\\ \\hline\n"
                        "\\end{tabular}\n"
                        "\\end{table}"),
                "a table is a captioned table[h] float around a tabular with hlines, "
                "escaped cells, and newlines folded to spaces");
}

void verify_formulas_wrap_in_math_delimiters() {
  docv1::Document document = base_document("math.tex");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula,
           docv1::DOC_ITEM_LABEL_FORMULA, "E = mc^2");
  // Undecoded but with a source form: the reference's comment placeholder.
  const std::string undecoded = add_text(&document, "#/body", docv1::BaseTextItem::kFormula,
                                         docv1::DOC_ITEM_LABEL_FORMULA, "");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_formula()
      ->mutable_base()
      ->set_orig("![equation](img.png)");

  require_equal(grparse::render_latex(document),
                wrapped("$$E = mc^2$$\n\n% formula-not-decoded"),
                "formulas wrap in $$..$$ unescaped, undecoded ones leave a comment");

  // Inside an inline group a formula takes the inline $..$ form.
  docv1::Document inline_doc = base_document("inline.tex");
  const std::string group =
      add_group(&inline_doc, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&inline_doc, group, "see");
  add_text(&inline_doc, group, docv1::BaseTextItem::kFormula,
           docv1::DOC_ITEM_LABEL_FORMULA, "x^2");
  require_equal(grparse::render_latex(inline_doc), wrapped("see $x^2$"),
                "an inline formula wraps in single dollars");
}

void verify_code_uses_verbatim_blocks_and_inline_texttt() {
  docv1::Document document = base_document("code.tex");
  add_code(&document, "#/body", "if (a & b) { return \"x\"; }",
           docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  const std::string group = add_group(&document, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, group, "call");
  add_code(&document, group, "#define X 1", docv1::CODE_LANGUAGE_LABEL_C);

  require_equal(grparse::render_latex(document),
                wrapped("\\begin{verbatim}\nif (a & b) { return \"x\"; }\n\\end{verbatim}\n\n"
                        "call \\texttt{\\\\#define X 1}"),
                "block code is verbatim and unescaped; inline code is \\texttt with "
                "only the macro parameter character escaped");
}

void verify_a_picture_is_a_figure_with_the_placeholder() {
  docv1::Document document = base_document("fig.tex");
  auto* figure = add_picture(&document, "#/body", "figs/one.png");
  figure->add_captions()->set_ref(add_owned_text(&document, figure->self_ref(),
                                                 docv1::DOC_ITEM_LABEL_CAPTION, "A chart"));
  figure->mutable_meta()->mutable_description()->set_text("a bar chart of sales");
  auto* prediction = figure->mutable_meta()->mutable_classification()->add_predictions();
  prediction->set_class_name("bar_chart");

  require_equal(grparse::render_latex(document),
                wrapped("\\begin{figure}[h]\n"
                        "% image\n"
                        "\\caption{A chart}\n"
                        "% annotation[classification]: bar chart\n"
                        "% annotation[description]: a bar chart of sales\n"
                        "\\end{figure}"),
                "a picture is a figure float with the placeholder, its caption, and "
                "its annotations as comments");
}

void verify_missing_items_degrade_to_comments() {
  docv1::Document document = base_document("forms.tex");
  const std::string kv_ref = "#/key_value_items/0";
  auto* kv = document.add_key_value_items();
  kv->set_self_ref(kv_ref);
  kv->mutable_parent()->set_ref("#/body");
  kv->set_label(docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION);
  kv->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_body()->add_children()->set_ref(kv_ref);
  const std::string form_ref = "#/form_items/0";
  auto* form = document.add_form_items();
  form->set_self_ref(form_ref);
  form->mutable_parent()->set_ref("#/body");
  form->set_label(docv1::DOC_ITEM_LABEL_FORM);
  form->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_body()->add_children()->set_ref(form_ref);

  require_equal(grparse::render_latex(document),
                wrapped("% missing-key-value-item\n\n% missing-form-item"),
                "key-value and form items degrade to the reference's comments");
}

void verify_a_non_body_layer_item_inside_a_group_stays_out() {
  docv1::Document document = base_document("layers.tex");
  const std::string chapter = add_group(&document, "#/body", docv1::GROUP_LABEL_CHAPTER);
  add_paragraph(&document, chapter, "kept");
  add_paragraph(&document, chapter, "dropped");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  require_equal(grparse::render_latex(document), wrapped("kept"),
                "the layer filter reaches inside groups, not just the body root");
}

void verify_formatting_and_hyperlinks_wrap_the_escaped_text() {
  docv1::Document document = base_document("fmt.tex");
  add_paragraph(&document, "#/body", "plain");
  const std::string bold = add_paragraph(&document, "#/body", "bold_text");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->mutable_formatting()
      ->set_bold(true);
  const std::string both = add_paragraph(&document, "#/body", "both");
  auto* base = document.mutable_texts(document.texts_size() - 1)
                   ->mutable_text()
                   ->mutable_base();
  base->mutable_formatting()->set_bold(true);
  base->mutable_formatting()->set_italic(true);
  const std::string link = add_paragraph(&document, "#/body", "link here");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_hyperlink("https://EXAMPLE.com/a_b");

  require_equal(grparse::render_latex(document),
                wrapped("plain\n\n"
                        "\\textbf{bold\\_text}\n\n"
                        "\\textit{\\textbf{both}}\n\n"
                        "\\href{https://example.com/a\\_b}{link here}"),
                "formatting wraps innermost-first and the hyperlink normalizes and "
                "escapes its target");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("latex-renderer-test", "ok", {
      verify_an_empty_document_is_the_bare_skeleton,
      verify_escaping_covers_every_special_character,
      verify_the_title_hoists_into_the_preamble,
      verify_section_headers_map_by_level_and_clamp,
      verify_lists_choose_their_environment_and_indent,
      verify_a_stray_list_item_gets_a_list,
      verify_a_table_renders_as_a_floated_tabular,
      verify_formulas_wrap_in_math_delimiters,
      verify_code_uses_verbatim_blocks_and_inline_texttt,
      verify_a_picture_is_a_figure_with_the_placeholder,
      verify_missing_items_degrade_to_comments,
      verify_a_non_body_layer_item_inside_a_group_stays_out,
      verify_formatting_and_hyperlinks_wrap_the_escaped_text,
  });
}
