#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/confluence_storage.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::CollectorOutcome parse(const std::string& storage) {
  grparse::CollectorOutcome outcome = grparse::parse_confluence_storage(storage);
  require(outcome.success, "parse failed: " + outcome.error);
  const auto integrity = grparse::docling_integrity_errors(outcome.document);
  require(integrity.empty(),
          "the folded document must be structurally well formed: " +
              (integrity.empty() ? std::string() : integrity.front()));
  return outcome;
}

const docv1::TextItemBase& base_of(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kSectionHeader: return item.section_header().base();
    case docv1::BaseTextItem::kListItem: return item.list_item().base();
    case docv1::BaseTextItem::kText: return item.text().base();
    default:
      throw std::runtime_error("text item has no base to read");
  }
}

template <typename Meta>
std::string custom_field(const Meta& meta, const std::string& key) {
  const auto found = meta.custom_fields().find(key);
  return found == meta.custom_fields().end() ? std::string()
                                             : found->second.string_value();
}

// Every item the handler makes must be attributable, or an additive merge
// with another collector's output cannot tell the two apart.
void require_stamped(const google::protobuf::RepeatedPtrField<docv1::SourceType>& source,
                     const std::string& where) {
  require(source.size() == 1 && source.Get(0).has_collector(),
          where + " carries exactly one collector source");
  require(source.Get(0).collector().collector() == "confluence-storage" &&
              source.Get(0).collector().model() == "native",
          where + " names this handler as its source");
}

void verify_routing_predicate() {
  require(grparse::confluence_storage_format(
              "page.bin", "application/vnd.atlassian.confluence.storage+xhtml"),
          "the storage content type routes here");
  require(grparse::confluence_storage_format("handbook.confluence", ""),
          "the .confluence suffix routes here");
  require(grparse::confluence_storage_format("handbook.STORAGE.XHTML", ""),
          "the .storage.xhtml suffix routes here, case-insensitively");
  require(!grparse::confluence_storage_format("page.xhtml", ""),
          "a bare .xhtml is plain markup and stays with the markup collector");
  require(!grparse::confluence_storage_format("page.html", "text/html"),
          "plain HTML does not route here");
}

void verify_headings_map_by_level() {
  const auto outcome = parse(
      "<h1>One</h1><h2>Two</h2><h3>Three</h3><h4>Four</h4><h5>Five</h5>"
      "<h6>Six</h6>");
  const auto& document = outcome.document;
  require(document.texts_size() == 6,
          "one item per heading, saw " + std::to_string(document.texts_size()));
  for (int index = 0; index < 6; ++index) {
    const auto& item = document.texts(index);
    require(item.item_case() == docv1::BaseTextItem::kSectionHeader,
            "every heading is a section header");
    require(item.section_header().level() == index + 1,
            "the heading level is the tag's own number");
    require(base_of(item).label() == docv1::DOC_ITEM_LABEL_SECTION_HEADER,
            "section header label");
    require(base_of(item).parent().ref() == "#/body", "headings sit in the body");
    require_stamped(base_of(item).source(), "a heading");
  }
  require(document.body().children_size() == 6, "the body lists every heading");
}

void verify_uniform_and_mixed_formatting() {
  const auto uniform = parse("<p><strong>all of it is bold</strong></p>");
  const auto& bold = base_of(uniform.document.texts(0));
  require(bold.text() == "all of it is bold", "the paragraph keeps its text");
  require(bold.orig() == bold.text(), "orig mirrors the text");
  require(bold.has_formatting() && bold.formatting().bold(),
          "uniform bold lands on the item");
  require(!bold.formatting().italic(), "nothing else is claimed");

  const auto mixed = parse("<p>plain and <strong>bold</strong></p>");
  const auto& partly = base_of(mixed.document.texts(0));
  require(partly.text() == "plain and bold", "mixed text is still whole");
  require(!partly.has_formatting(),
          "mixed formatting stays unset rather than picking a winner");

  const auto plain = parse("<p>nothing special</p>");
  require(!base_of(plain.document.texts(0)).has_formatting(),
          "unformatted text carries no formatting message");
}

void verify_inline_marks_and_scripts() {
  const auto italic = parse("<p><em>slanted</em></p>");
  require(base_of(italic.document.texts(0)).formatting().italic(), "em is italic");
  const auto underline = parse("<p><u>lined</u></p>");
  require(base_of(underline.document.texts(0)).formatting().underline(),
          "u is underline");
  const auto struck = parse("<p><del>gone</del></p>");
  require(base_of(struck.document.texts(0)).formatting().strikethrough(),
          "del is strikethrough");
  const auto struck_short = parse("<p><s>gone</s></p>");
  require(base_of(struck_short.document.texts(0)).formatting().strikethrough(),
          "s is strikethrough too");
  const auto sub = parse("<p><sub>below</sub></p>");
  require(base_of(sub.document.texts(0)).formatting().script() == docv1::SCRIPT_SUB,
          "sub is the sub script");
  const auto sup = parse("<p><sup>above</sup></p>");
  require(base_of(sup.document.texts(0)).formatting().script() == docv1::SCRIPT_SUPER,
          "sup is the super script");
  const auto broken = parse("<p>one<br />two</p>");
  require(broken.document.texts_size() == 1,
          "a line break stays inside its paragraph rather than splitting it");
  require(base_of(broken.document.texts(0)).text() == "one\ntwo",
          "and folds into the text as a newline");

  const auto mixed_script = parse("<p>x<sup>2</sup></p>");
  require(!base_of(mixed_script.document.texts(0)).has_formatting(),
          "a script on part of the text is not a script on the item");
}

void verify_hyperlinks_land_in_slot_and_custom_field() {
  const auto outcome = parse(
      "<p>see <a href=\"https://example.com/one\">one</a> and "
      "<a href=\"https://example.com/two\">two</a></p>");
  const auto& base = base_of(outcome.document.texts(0));
  require(base.text() == "see one and two", "link text is part of the paragraph");
  require(base.hyperlink() == "https://example.com/one",
          "the first link takes the hyperlink slot");
  const auto found = base.meta().custom_fields().find("hyperlinks");
  require(found != base.meta().custom_fields().end(),
          "every link is listed in the hyperlinks custom field");
  const auto& links = found->second.list_value();
  require(links.values_size() == 2, "both links are listed");
  const auto& first = links.values(0).struct_value().fields();
  require(first.at("url").string_value() == "https://example.com/one",
          "the first link's url");
  require(first.at("char_start").number_value() == 4 &&
              first.at("char_end").number_value() == 7,
          "the first link's character span");
  const auto& second = links.values(1).struct_value().fields();
  require(second.at("url").string_value() == "https://example.com/two",
          "the second link's url");
  require(second.at("char_start").number_value() == 12 &&
              second.at("char_end").number_value() == 15,
          "the second link's character span");
}

void verify_entities_and_cdata() {
  const auto outcome = parse(
      "<p>&amp; &lt; &gt; &quot; &apos; &#65; &#x42; &nbsp;end &notanentity; "
      "&#xZZ;</p>");
  const std::string text = base_of(outcome.document.texts(0)).text();
  require(text.find("& < > \" ' A B ") == 0,
          "the predefined and numeric references decode: " + text);
  require(text.find("\xC2\xA0") != std::string::npos,
          "the non-breaking space decodes to its code point");
  require(text.find("&notanentity;") != std::string::npos,
          "an unknown reference stays verbatim rather than being dropped");
  require(text.find("&#xZZ;") != std::string::npos,
          "a malformed numeric reference stays verbatim");

  const auto cdata = parse(
      "<ac:structured-macro ac:name=\"code\"><ac:plain-text-body>"
      "<![CDATA[if (a < b && c > d) { return \"<tag>\"; }]]>"
      "</ac:plain-text-body></ac:structured-macro>");
  require(cdata.document.texts(0).code().text() ==
              "if (a < b && c > d) { return \"<tag>\"; }",
          "CDATA is taken verbatim, entities and markup characters included");
}

void verify_lists_nest_and_carry_markers() {
  const auto outcome = parse(
      "<ol><li><p>first</p></li><li><p>second</p>"
      "<ul><li><p>inner</p></li></ul></li></ol>");
  const auto& document = outcome.document;
  require(document.groups_size() == 2, "the outer list and its sublist");
  require(document.groups(0).label() == docv1::GROUP_LABEL_ORDERED_LIST,
          "an ol is an ordered list group");
  require(document.groups(0).parent().ref() == "#/body", "the list sits in the body");
  require(document.groups(1).label() == docv1::GROUP_LABEL_LIST,
          "a ul is a plain list group");
  require(document.groups(1).parent().ref() == document.groups(0).self_ref(),
          "the sublist nests under the list it was written inside");

  require(document.texts_size() == 3, "one item per li");
  const auto& first = document.texts(0).list_item();
  require(first.base().text() == "first" && first.enumerated(),
          "ordered items are enumerated");
  require(first.marker() == "1." && document.texts(1).list_item().marker() == "2.",
          "ordered markers count from one");
  require(base_of(document.texts(0)).label() == docv1::DOC_ITEM_LABEL_LIST_ITEM,
          "list item label");
  const auto& inner = document.texts(2).list_item();
  require(inner.base().text() == "inner" && !inner.enumerated() &&
              inner.marker() == "-",
          "unordered items keep the plain marker");
  require(inner.base().parent().ref() == document.groups(1).self_ref(),
          "the nested item belongs to the nested group");
  require(document.groups(0).children_size() == 3,
          "the list lists its two items and the sublist group");
}

void verify_table_headers_spans_and_grid() {
  const auto outcome = parse(
      "<table><tbody>"
      "<tr><th><p>H1</p></th><th colspan=\"2\"><p>H2</p></th></tr>"
      "<tr><td rowspan=\"2\"><p>A</p></td><td><p>B</p></td><td><p>C</p></td></tr>"
      "<tr><td><p>D</p></td><td><p>E</p></td></tr>"
      "</tbody></table>");
  const auto& document = outcome.document;
  require(document.tables_size() == 1, "one table item");
  const auto& table = document.tables(0);
  require(table.label() == docv1::DOC_ITEM_LABEL_TABLE, "table label");
  require(table.parent().ref() == "#/body", "the table sits in the body");
  require_stamped(table.source(), "the table");
  const auto& data = table.data();
  require(data.num_rows() == 3 && data.num_cols() == 3, "the declared grid extent");
  require(data.table_cells_size() == 7, "every placed cell is kept");

  const auto& header = data.table_cells(1);
  require(header.text() == "H2" && header.column_header(),
          "a th in the first row is a column header");
  require(header.start_col_offset_idx() == 1 && header.end_col_offset_idx() == 3 &&
              header.col_span() == 2,
          "the column span is taken from the attribute");
  const auto& spanning = data.table_cells(2);
  require(spanning.text() == "A" && spanning.row_span() == 2 &&
              spanning.start_row_offset_idx() == 1 && spanning.end_row_offset_idx() == 3,
          "the row span is taken from the attribute");
  const auto& shifted = data.table_cells(5);
  require(shifted.text() == "D" && shifted.start_col_offset_idx() == 1,
          "a cell under a row span takes the next free column");

  require(data.grid_size() == 3 && data.grid(0).cells_size() == 3,
          "the grid materializes at the declared extent");
  require(data.grid(0).cells(1).text() == "H2" && data.grid(0).cells(2).text() == "H2",
          "a spanning cell fills every slot it covers");
  require(data.grid(2).cells(0).text() == "A",
          "a row span reaches the row below it");
  require(data.grid(2).cells(1).text() == "D" && data.grid(2).cells(2).text() == "E",
          "the last row keeps its own cells");
}

void verify_row_header_and_thead_section() {
  const auto outcome = parse(
      "<table><thead><tr><th><p>Name</p></th><th><p>Port</p></th></tr></thead>"
      "<tbody><tr><th><p>parser</p></th><td><p>50051</p></td></tr></tbody></table>");
  const auto& data = outcome.document.tables(0).data();
  require(data.table_cells(0).column_header() && data.table_cells(1).column_header(),
          "the thead row is the column header row");
  require(data.table_cells(2).row_header() && !data.table_cells(2).column_header(),
          "a th in a body row is a row header");
  require(!data.table_cells(3).row_header() && !data.table_cells(3).column_header(),
          "a plain cell is neither");
}

void verify_out_of_range_span_is_clamped() {
  const auto outcome = parse(
      "<table><tbody><tr><td rowspan=\"9\"><p>tall</p></td><td><p>b</p></td>"
      "</tr></tbody></table>");
  const auto& data = outcome.document.tables(0).data();
  require(data.num_rows() == 1, "the table declares the rows it has");
  require(data.table_cells(0).row_span() == 1 &&
              data.table_cells(0).end_row_offset_idx() == 1,
          "a span past the table is clamped to it");
  require(!outcome.warnings.empty(), "the clamp is reported as a warning");
  require(data.grid_size() == 1 && data.grid(0).cells_size() == 2,
          "the grid stays inside the clamped extent");
}

void verify_oversized_table_keeps_cells_only() {
  // 65 x 64 is past the grid ceiling, so the placed cells survive and the
  // materialized grid is skipped instead of allocating the rectangle.
  std::string storage = "<table><tbody>";
  for (int row = 0; row < 65; ++row) {
    storage += "<tr>";
    for (int column = 0; column < 64; ++column) storage += "<td>x</td>";
    storage += "</tr>";
  }
  storage += "</tbody></table>";
  const auto outcome = parse(storage);
  const auto& data = outcome.document.tables(0).data();
  require(data.num_rows() == 65 && data.num_cols() == 64, "the extent is reported");
  require(data.table_cells_size() == 65 * 64, "every cell is kept");
  require(data.grid_size() == 0, "the grid is skipped above the ceiling");
}

void verify_code_macro_language_mapping() {
  const auto known = parse(
      "<ac:structured-macro ac:name=\"code\">"
      "<ac:parameter ac:name=\"language\">python</ac:parameter>"
      "<ac:plain-text-body><![CDATA[print(1)]]></ac:plain-text-body>"
      "</ac:structured-macro>");
  const auto& code = known.document.texts(0).code();
  require(code.self_ref() == "#/texts/0" && code.parent().ref() == "#/body",
          "the code item is a body child");
  require(code.label() == docv1::DOC_ITEM_LABEL_CODE, "code label");
  require(code.text() == "print(1)" && code.orig() == code.text(),
          "the macro body is the code text");
  require(code.code_language() == docv1::CODE_LANGUAGE_LABEL_PYTHON,
          "a language the schema names maps to its enum value");
  require(!code.has_code_language_raw(),
          "a mapped language does not also carry a raw string");
  require_stamped(code.source(), "the code item");

  const auto unknown = parse(
      "<ac:structured-macro ac:name=\"code\">"
      "<ac:parameter ac:name=\"language\">hcl</ac:parameter>"
      "<ac:plain-text-body><![CDATA[resource {}]]></ac:plain-text-body>"
      "</ac:structured-macro>");
  const auto& raw = unknown.document.texts(0).code();
  require(raw.code_language() == docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED,
          "an unmapped language never guesses a neighbouring value");
  require(raw.code_language_raw() == "hcl", "an unmapped language is kept raw");

  const auto bare = parse(
      "<ac:structured-macro ac:name=\"code\"><ac:plain-text-body>"
      "<![CDATA[plain]]></ac:plain-text-body></ac:structured-macro>");
  require(!bare.document.texts(0).code().has_code_language_raw() &&
              bare.document.texts(0).code().code_language() ==
                  docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED,
          "a macro with no language parameter claims none");
}

void verify_task_list_checkbox_states() {
  const auto outcome = parse(
      "<ac:task-list>"
      "<ac:task><ac:task-id>1</ac:task-id><ac:task-status>complete</ac:task-status>"
      "<ac:task-body>ship it</ac:task-body></ac:task>"
      "<ac:task><ac:task-id>2</ac:task-id><ac:task-status>incomplete</ac:task-status>"
      "<ac:task-body>wire the <strong>crawler</strong></ac:task-body></ac:task>"
      "</ac:task-list>");
  const auto& document = outcome.document;
  require(document.groups_size() == 1 &&
              document.groups(0).label() == docv1::GROUP_LABEL_LIST,
          "tasks sit in a list group");
  require(document.groups(0).name() == "task-list", "the group names the construct");
  require(document.texts_size() == 2, "one item per task");
  require(base_of(document.texts(0)).label() == docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED,
          "a complete task is a selected checkbox");
  require(base_of(document.texts(0)).text() == "ship it", "the task body is the text");
  require(base_of(document.texts(1)).label() ==
              docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED,
          "an incomplete task is an unselected checkbox");
  require(base_of(document.texts(1)).text() == "wire the crawler",
          "task bodies keep their inline markup as text");
  require(document.texts(1).list_item().marker() == "-" &&
              !document.texts(1).list_item().enumerated(),
          "tasks are unenumerated list items");
}

void verify_panel_macro_marks_items() {
  const auto outcome = parse(
      "<ac:structured-macro ac:name=\"info\"><ac:rich-text-body>"
      "<p>read this</p><p>and this</p></ac:rich-text-body></ac:structured-macro>");
  const auto& document = outcome.document;
  require(document.texts_size() == 2, "the panel body parses recursively");
  for (int index = 0; index < 2; ++index) {
    const auto& base = base_of(document.texts(index));
    require(base.label() == docv1::DOC_ITEM_LABEL_TEXT,
            "a panel invents no label of its own");
    require(custom_field(base.meta(), "panel") == "info",
            "every item inside the panel names it");
    require(base.parent().ref() == "#/body",
            "panel content stays where the macro stood");
  }

  const auto note = parse(
      "<ac:structured-macro ac:name=\"warning\"><ac:rich-text-body>"
      "<h2>careful</h2></ac:rich-text-body></ac:structured-macro>");
  require(custom_field(base_of(note.document.texts(0)).meta(), "panel") == "warning",
          "the whole panel family is recognized");
}

void verify_unknown_macro_body_survives() {
  const auto outcome = parse(
      "<ac:structured-macro ac:name=\"expand\"><ac:parameter ac:name=\"title\">"
      "More</ac:parameter><ac:rich-text-body><p>hidden but present</p>"
      "<ac:structured-macro ac:name=\"code\"><ac:parameter ac:name=\"language\">"
      "bash</ac:parameter><ac:plain-text-body><![CDATA[ls]]></ac:plain-text-body>"
      "</ac:structured-macro></ac:rich-text-body></ac:structured-macro>");
  const auto& document = outcome.document;
  require(document.texts_size() == 2, "the unknown macro's body is not dropped");
  require(base_of(document.texts(0)).text() == "hidden but present",
          "its text survives");
  require(custom_field(base_of(document.texts(0)).meta(), "macro") == "expand",
          "the macro name rides on the items it produced");
  require(document.texts(1).code().code_language() == docv1::CODE_LANGUAGE_LABEL_BASH,
          "a known macro nested inside an unknown one still maps");
  require(custom_field(document.texts(1).code().meta(), "macro") == "expand",
          "the enclosing macro name reaches nested items too");

  const auto plain = parse(
      "<ac:structured-macro ac:name=\"noformat\"><ac:plain-text-body>"
      "<![CDATA[as written]]></ac:plain-text-body></ac:structured-macro>");
  require(base_of(plain.document.texts(0)).text() == "as written",
          "a plain-text body of an unmapped macro becomes text");
  require(custom_field(base_of(plain.document.texts(0)).meta(), "macro") == "noformat",
          "and names its macro");

  const auto bodiless = grparse::parse_confluence_storage(
      "<ac:structured-macro ac:name=\"toc\"><ac:parameter ac:name=\"maxLevel\">"
      "3</ac:parameter></ac:structured-macro>");
  require(bodiless.success, "a bodiless macro is not a failure");
  require(bodiless.document.texts_size() == 0,
          "a bodiless macro invents no content");
  require(!bodiless.warnings.empty(),
          "a bodiless macro says out loud that its parameters were not mapped");
}

void verify_attachment_and_url_images() {
  const auto attached = parse(
      "<p><ac:image ac:alt=\"the diagram\">"
      "<ri:attachment ri:filename=\"diagram.png\" /></ac:image></p>");
  require(attached.document.pictures_size() == 1, "one picture item");
  const auto& picture = attached.document.pictures(0);
  require(picture.label() == docv1::DOC_ITEM_LABEL_PICTURE, "picture label");
  require(picture.parent().ref() == "#/body", "the picture sits in the body");
  require(picture.image().uri() == "confluence-attachment:diagram.png",
          "an attachment becomes a pointer, never invented bytes");
  require(picture.image().mimetype().empty() && !picture.image().has_size(),
          "nothing beyond the pointer is claimed");
  require(custom_field(picture.meta(), "alt") == "the diagram",
          "the alternative text is kept");
  require_stamped(picture.source(), "the picture");

  const auto external = parse(
      "<ac:image><ri:url ri:value=\"https://example.com/x.png\" /></ac:image>");
  require(external.document.pictures(0).image().uri() == "https://example.com/x.png",
          "an external image keeps the uri as given");
}

void verify_page_link_pointer() {
  const auto outcome = parse(
      "<p>see <ac:link><ri:page ri:content-title=\"Runbook\" />"
      "<ac:plain-text-link-body><![CDATA[the runbook]]></ac:plain-text-link-body>"
      "</ac:link> first</p>");
  const auto& base = base_of(outcome.document.texts(0));
  require(base.text() == "see the runbook first",
          "the link body is part of its paragraph");
  require(base.hyperlink() == "confluence-page:Runbook",
          "a page link with no url keeps a resolvable pointer");

  const auto bare = parse(
      "<p><ac:link><ri:page ri:content-title=\"Onboarding\" /></ac:link></p>");
  require(base_of(bare.document.texts(0)).text() == "Onboarding",
          "a bodiless link renders as its target's title");
  require(base_of(bare.document.texts(0)).hyperlink() == "confluence-page:Onboarding",
          "and still points at the page");

  const auto attachment = parse(
      "<p><ac:link><ri:attachment ri:filename=\"notes.pdf\" /></ac:link></p>");
  require(base_of(attachment.document.texts(0)).hyperlink() ==
              "confluence-attachment:notes.pdf",
          "an attachment link points at the attachment");
}

void verify_unknown_tags_descend_transparently() {
  const auto outcome = parse(
      "<div class=\"wrapper\"><section><p>inside</p>"
      "<blink>loose text</blink></section></div>");
  const auto& document = outcome.document;
  require(document.texts_size() == 2,
          "an unknown container contributes its content, not itself");
  require(base_of(document.texts(0)).text() == "inside", "the paragraph survives");
  require(base_of(document.texts(1)).text() == "loose text",
          "text with no paragraph of its own is still kept");
  require(base_of(document.texts(1)).parent().ref() == "#/body",
          "transparent containers do not create parents");
}

void verify_malformed_markup_recovers() {
  const auto outcome = parse("<p>opened but never closed<h2>next</h2>");
  require(outcome.document.texts_size() == 2,
          "an unclosed tag keeps the content around it");
  require(!outcome.warnings.empty(), "the damage is reported as a warning");

  const auto stray = parse("<p>text</p></div>");
  require(stray.document.texts_size() == 1, "a stray end tag drops nothing");
  require(!stray.warnings.empty(), "and is reported");
}

void verify_empty_body_is_rejected() {
  const auto outcome = grparse::parse_confluence_storage("   just words   ");
  require(!outcome.success, "a body with no markup is not a storage document");
  require(outcome.code == grpc::StatusCode::INVALID_ARGUMENT,
          "and the rejection is the caller's fault");
  const auto comment_only = grparse::parse_confluence_storage("<!-- nothing -->");
  require(!comment_only.success, "a comment is not markup either");
}

// The body of a real page, fetched from the wiki API and kept verbatim: the
// pinned shape below is what this handler must keep producing for it.
constexpr char kRealPage[] =
    R"STORAGE(<h1>Engineering Handbook</h1><p>This page exercises the <strong>storage format</strong> constructs a parser must honor: <em>emphasis</em>, <u>underline</u>, <del>strikethrough</del>, <sub>sub</sub> and <sup>sup</sup>, plus a <a href="https://example.com/spec">hyperlink</a>.</p><h2>Setup steps</h2><ol><li><p>Install the toolchain</p></li><li><p>Configure the runtime</p><ul><li><p>nested unordered item</p></li><li><p>second nested item</p></li></ul><p /></li></ol><h2>Reference table</h2><table><tbody><tr><th><p>Component</p></th><th><p>Port</p></th><th><p>Notes</p></th></tr><tr><td><p>parser</p></td><td><p>50051</p></td><td><p>pipe | in cell</p></td></tr><tr><td><p>proxy</p></td><td><p>18081</p></td><td><p>front door</p></td></tr></tbody></table><h2>Snippet</h2><ac:structured-macro ac:name="code" ac:schema-version="1" ac:macro-id="2db0f889-42ba-4a9a-9e8e-7635e10faaa3"><ac:parameter ac:name="language">python</ac:parameter><ac:plain-text-body><![CDATA[def parse(doc):
    return [block for block in doc if block.text]]]></ac:plain-text-body></ac:structured-macro><h2>Tasks</h2><ac:task-list><ac:task><ac:task-status>complete</ac:task-status><ac:task-body>ship the parser</ac:task-body></ac:task><ac:task><ac:task-status>incomplete</ac:task-status><ac:task-body>wire the crawler</ac:task-body></ac:task></ac:task-list><ac:structured-macro ac:name="info" ac:schema-version="1" ac:macro-id="323765ee-be7a-47c5-a0a2-9d8ce76f98de"><ac:rich-text-body><p>This is an info panel with a <strong>bold</strong> note.</p></ac:rich-text-body></ac:structured-macro>)STORAGE";

void verify_real_page_fixture_shape() {
  const auto outcome = parse(kRealPage);
  const auto& document = outcome.document;
  require(outcome.warnings.empty(),
          "a real page body parses without recovering from anything");
  require(document.texts_size() == 14, "the page's text item count");
  require(document.groups_size() == 3,
          "the ordered list, its sublist, and the task list");
  require(document.tables_size() == 1, "the page's one table");
  require(document.pictures_size() == 0, "the page carries no image");
  require(document.body().children_size() == 11, "the body's top-level children");

  require(document.texts(0).section_header().level() == 1 &&
              base_of(document.texts(0)).text() == "Engineering Handbook",
          "the page opens with its h1 as a section header");

  const auto& intro = base_of(document.texts(1));
  require(intro.text() ==
              "This page exercises the storage format constructs a parser must "
              "honor: emphasis, underline, strikethrough, sub and sup, plus a "
              "hyperlink.",
          "the intro paragraph's text: " + intro.text());
  require(!intro.has_formatting(),
          "the intro mixes formatting, so the item claims none");
  require(intro.hyperlink() == "https://example.com/spec", "the intro's link");
  const auto& span = intro.meta()
                         .custom_fields()
                         .at("hyperlinks")
                         .list_value()
                         .values(0)
                         .struct_value()
                         .fields();
  const auto expected_start = static_cast<double>(intro.text().find("hyperlink"));
  require(span.at("char_start").number_value() == expected_start &&
              span.at("char_end").number_value() == expected_start + 9,
          "the link's character span covers its own text");

  require(document.texts(2).section_header().level() == 2 &&
              base_of(document.texts(2)).text() == "Setup steps",
          "the first h2");
  require(document.groups(0).label() == docv1::GROUP_LABEL_ORDERED_LIST &&
              document.groups(1).label() == docv1::GROUP_LABEL_LIST &&
              document.groups(1).parent().ref() == document.groups(0).self_ref(),
          "the ordered list carries the nested unordered one");
  require(base_of(document.texts(3)).text() == "Install the toolchain" &&
              document.texts(3).list_item().marker() == "1.",
          "the first step");
  require(base_of(document.texts(4)).text() == "Configure the runtime" &&
              document.texts(4).list_item().marker() == "2.",
          "the second step, whose empty paragraph adds nothing");
  require(base_of(document.texts(5)).text() == "nested unordered item" &&
              base_of(document.texts(6)).text() == "second nested item",
          "both nested items");

  const auto& data = document.tables(0).data();
  require(data.num_rows() == 3 && data.num_cols() == 3, "the reference table extent");
  require(data.table_cells(0).text() == "Component" &&
              data.table_cells(0).column_header(),
          "the header row");
  require(data.grid(1).cells(2).text() == "pipe | in cell",
          "a cell containing a pipe survives intact");
  require(data.grid(2).cells(0).text() == "proxy", "the last row");

  const auto& code = document.texts(9).code();
  require(code.code_language() == docv1::CODE_LANGUAGE_LABEL_PYTHON,
          "the code macro's declared language");
  require(code.text() ==
              "def parse(doc):\n    return [block for block in doc if block.text]",
          "the code body is verbatim, indentation included: " + code.text());

  require(base_of(document.texts(11)).label() ==
                  docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED &&
              base_of(document.texts(11)).text() == "ship the parser",
          "the completed task");
  require(base_of(document.texts(12)).label() ==
                  docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED &&
              base_of(document.texts(12)).text() == "wire the crawler",
          "the open task");

  const auto& panel = base_of(document.texts(13));
  require(panel.text() == "This is an info panel with a bold note.",
          "the info panel's paragraph");
  require(custom_field(panel.meta(), "panel") == "info",
          "the panel names itself on the item");
}

}  // namespace

int main() {
  try {
    verify_routing_predicate();
    verify_headings_map_by_level();
    verify_uniform_and_mixed_formatting();
    verify_inline_marks_and_scripts();
    verify_hyperlinks_land_in_slot_and_custom_field();
    verify_entities_and_cdata();
    verify_lists_nest_and_carry_markers();
    verify_table_headers_spans_and_grid();
    verify_row_header_and_thead_section();
    verify_out_of_range_span_is_clamped();
    verify_oversized_table_keeps_cells_only();
    verify_code_macro_language_mapping();
    verify_task_list_checkbox_states();
    verify_panel_macro_marks_items();
    verify_unknown_macro_body_survives();
    verify_attachment_and_url_images();
    verify_page_link_pointer();
    verify_unknown_tags_descend_transparently();
    verify_malformed_markup_recovers();
    verify_empty_body_is_rejected();
    verify_real_page_fixture_shape();
    std::println("confluence-storage-test: all checks passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "confluence-storage-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
