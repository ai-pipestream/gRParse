// The seams every export renderer shares: reference parsing, the text-variant
// accessor, heading ranks, trimming, the two escapes, code and human language
// tags, URI normalization, the two table grids, and the custom-field ordering
// that makes an unordered wire map export deterministically.

#include <optional>
#include <string>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "../src/render/renderer_base.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;
namespace render = grparse::render;

using grparse_test::add_cell;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

std::string kind_name(render::ArenaRef::Kind kind) {
  switch (kind) {
    case render::ArenaRef::kText: return "text";
    case render::ArenaRef::kTable: return "table";
    case render::ArenaRef::kPicture: return "picture";
    case render::ArenaRef::kGroup: return "group";
    case render::ArenaRef::kKeyValue: return "key_value";
    case render::ArenaRef::kForm: return "form";
    case render::ArenaRef::kFieldRegion: return "field_region";
    case render::ArenaRef::kFieldItem: return "field_item";
    case render::ArenaRef::kUnknown: break;
  }
  return "unknown";
}

// The cell layout as one string per row, for a readable grid comparison.
std::string grid_text(const std::vector<std::vector<const docv1::TableCell*>>& grid) {
  std::string out;
  for (const auto& row : grid) {
    if (!out.empty()) out.push_back('/');
    for (size_t col = 0; col < row.size(); ++col) {
      if (col > 0) out.push_back(',');
      out.append(row[col] == nullptr ? "." : row[col]->text());
    }
  }
  return out;
}

void verify_every_arena_prefix_parses() {
  const struct {
    std::string ref;
    std::string kind;
    int index;
  } cases[] = {
      {"#/texts/0", "text", 0},         {"#/tables/12", "table", 12},
      {"#/pictures/3", "picture", 3},   {"#/groups/7", "group", 7},
      {"#/key_value_items/1", "key_value", 1},
      {"#/form_items/2", "form", 2},    {"#/field_regions/4", "field_region", 4},
      {"#/field_items/5", "field_item", 5},
  };
  for (const auto& one : cases) {
    const render::ArenaRef ref = render::parse_ref(one.ref);
    require_equal(kind_name(ref.kind), one.kind, one.ref + " names its arena");
    require_equal(ref.index, one.index, one.ref + " names its index");
  }
}

void verify_a_reference_that_names_no_arena_slot_is_unknown() {
  const std::vector<std::string> refs{"#/body",   "#/furniture",        "#/texts/",
                                     "#/texts/x", "#/texts/-1",        "#/texts/1234567890",
                                     "texts/0",  "",                   "#/unknown/0"};
  for (const std::string& ref : refs) {
    const render::ArenaRef parsed = render::parse_ref(ref);
    require_equal(kind_name(parsed.kind), "unknown", "\"" + ref + "\" resolves to no arena");
    require_equal(parsed.index, -1, "\"" + ref + "\" carries no index");
  }
}

void verify_text_base_is_null_only_for_code_and_unset() {
  docv1::BaseTextItem item;
  require(render::text_base(item) == nullptr, "an unset variant has no base");
  item.mutable_code()->set_text("x");
  require(render::text_base(item) == nullptr, "a code item inlines its fields");

  docv1::BaseTextItem title;
  title.mutable_title()->mutable_base()->set_text("t");
  require(render::text_base(title) != nullptr && render::text_base(title)->text() == "t",
          "a title reaches its nested base");
  docv1::BaseTextItem heading;
  heading.mutable_field_heading()->mutable_base()->set_text("h");
  require(render::text_base(heading) != nullptr, "a field heading reaches its nested base");
  docv1::BaseTextItem value;
  value.mutable_field_value()->mutable_base()->set_text("v");
  require(render::text_base(value) != nullptr, "a field value reaches its nested base");
}

void verify_heading_rank_lifts_and_clamps() {
  const struct {
    int level;
    int rank;
  } cases[] = {{-3, 2}, {0, 2}, {1, 2}, {2, 3}, {4, 5}, {5, 6}, {6, 6}, {99, 6}};
  for (const auto& one : cases) {
    require_equal(render::heading_rank(one.level), one.rank,
                  "heading level " + std::to_string(one.level) + " maps to its rank");
  }
}

void verify_trimming_strips_the_whitespace_family() {
  require_equal(render::trimmed("  hi  "), "hi", "spaces come off both ends");
  require_equal(render::trimmed("\t\n\r\f\vhi\v\f\r\n\t"), "hi",
                "every whitespace character in the family comes off");
  require_equal(render::trimmed("a  b"), "a  b", "inner whitespace is kept");
  require_equal(render::trimmed("   "), "", "an all-whitespace string trims to nothing");
  require_equal(render::trimmed(""), "", "an empty string stays empty");
}

void verify_the_two_escapes_differ_only_on_the_quote() {
  require_equal(render::escape_html_text("a & b < c > d \"e\""),
                "a &amp; b &lt; c &gt; d \"e\"",
                "text escaping leaves the quote alone");
  require_equal(render::escape_html_attribute("a & b < c > d \"e\""),
                "a &amp; b &lt; c &gt; d &quot;e&quot;",
                "attribute escaping also escapes the quote");
  require_equal(render::escape_html_text("&amp;"), "&amp;amp;",
                "an already escaped entity escapes again, which is what a raw string means");
}

void verify_the_code_fence_language_lower_cases_all_but_the_punctuated_tags() {
  docv1::CodeItem code;
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  require_equal(render::code_fence_language(code), "cpp", "C++ spells out as cpp in a fence");
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_C_SHARP);
  require_equal(render::code_fence_language(code), "csharp", "C# spells out as csharp in a fence");
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_PYTHON);
  require_equal(render::code_fence_language(code), "python", "an ordinary tag lower-cases");
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_UNKNOWN);
  require_equal(render::code_fence_language(code), "", "an unknown tag names no language");
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED);
  require_equal(render::code_fence_language(code), "", "an unset tag names no language");
  code.set_code_language(docv1::CODE_LANGUAGE_LABEL_PYTHON);
  code.set_code_language_raw("jinja");
  require_equal(render::code_fence_language(code), "jinja",
                "the collector's raw string outranks the enum");
}

void verify_the_code_language_string_is_the_canonical_spelling() {
  const struct {
    docv1::CodeLanguageLabel tag;
    std::string spelling;
  } cases[] = {
      {docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS, "C++"},
      {docv1::CODE_LANGUAGE_LABEL_C_SHARP, "C#"},
      {docv1::CODE_LANGUAGE_LABEL_C, "C"},
      {docv1::CODE_LANGUAGE_LABEL_BC, "bc"},
      {docv1::CODE_LANGUAGE_LABEL_DC, "dc"},
      {docv1::CODE_LANGUAGE_LABEL_PYTHON, "Python"},
      {docv1::CODE_LANGUAGE_LABEL_JAVASCRIPT, "JavaScript"},
      {docv1::CODE_LANGUAGE_LABEL_OCAML, "OCaml"},
      {docv1::CODE_LANGUAGE_LABEL_SQL, "SQL"},
      {docv1::CODE_LANGUAGE_LABEL_FORTRAN, "FORTRAN"},
      {docv1::CODE_LANGUAGE_LABEL_LATEX, "Latex"},
      {docv1::CODE_LANGUAGE_LABEL_UNKNOWN, "unknown"},
  };
  for (const auto& one : cases) {
    const std::optional<std::string_view> spelling = render::code_language_string(one.tag);
    require(spelling.has_value(),
            "a known code language tag has a spelling: " + one.spelling);
    require_equal(std::string(*spelling), one.spelling, "the tag's canonical spelling");
  }
  require(!render::code_language_string(docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED).has_value(),
          "an unset code language tag has no spelling");
}

void verify_the_human_language_string_is_a_bcp47_code() {
  require_equal(*render::human_language_string(docv1::HUMAN_LANGUAGE_LABEL_EN), "en",
                "a human language tag lower-cases into its subtag");
  require(!render::human_language_string(docv1::HUMAN_LANGUAGE_LABEL_UNSPECIFIED).has_value(),
          "an unset human language tag has no code");
}

void verify_uri_normalization_touches_only_what_the_model_touches() {
  const struct {
    std::string in;
    std::string out;
    std::string what;
  } cases[] = {
      {"HTTPS://Example.COM/Path", "https://example.com/Path",
       "a special scheme lower-cases its scheme and host but not its path"},
      {"https://Example.com", "https://example.com/",
       "an empty path on a special scheme becomes a single slash"},
      {"https://Example.com?q=A", "https://example.com/?q=A",
       "a query with no path gets the slash before it"},
      {"https://User@Example.COM:8443/x", "https://User@example.com:8443/x",
       "userinfo and port survive the host lower-casing"},
      {"MAILTO:Someone@Example.com", "mailto:Someone@Example.com",
       "a scheme that is not special keeps everything after the colon"},
      {"figs/one.png", "figs/one.png", "a string with no scheme passes through"},
      {"", "", "an empty uri stays empty"},
      {"1http://x", "1http://x", "a scheme must start with a letter"},
  };
  for (const auto& one : cases) {
    require_equal(render::normalized_uri(one.in), one.out, one.what);
  }
}

void verify_the_wire_grid_wins_over_the_flat_cell_list() {
  docv1::TableData data;
  data.set_num_rows(1);
  data.set_num_cols(2);
  add_cell(&data, data.add_grid(), "from-grid", false, 0, 0);
  data.mutable_table_cells(0)->set_text("from-flat");

  require_equal(grid_text(render::table_grid(data)), "from-grid",
                "a populated wire grid is the layout, whatever the flat list says");
}

void verify_the_flat_cell_list_places_itself_when_there_is_no_grid() {
  docv1::TableData data;
  data.set_num_rows(2);
  data.set_num_cols(3);
  add_cell(&data, nullptr, "A", false, 0, 0, 2, 1);
  add_cell(&data, nullptr, "B", false, 0, 1, 1, 2);
  add_cell(&data, nullptr, "C", false, 1, 1);

  require_equal(grid_text(render::table_grid(data)), "A,B,B/A,C,.",
                "a spanned cell appears at every position it covers and a gap stays null");
}

void verify_a_cell_reaching_past_the_declared_grid_is_capped() {
  docv1::TableData data;
  data.set_num_rows(1);
  data.set_num_cols(2);
  add_cell(&data, nullptr, "wide", false, 0, 0, 5, 9);
  require_equal(grid_text(render::table_grid(data)), "wide,wide",
                "a span past the declared size stops at the edge");
}

void verify_a_table_without_a_declared_size_has_no_derived_layout() {
  docv1::TableData data;
  add_cell(&data, nullptr, "orphan", false, 0, 0);
  require(render::table_grid(data).empty(),
          "a flat cell list with no declared row and column count places nothing");
}

void verify_the_derived_grid_wraps_a_negative_offset() {
  docv1::TableData data;
  data.set_num_rows(2);
  data.set_num_cols(2);
  auto* cell = data.add_table_cells();
  cell->set_text("last");
  cell->set_start_row_offset_idx(-1);
  cell->set_end_row_offset_idx(0);
  cell->set_start_col_offset_idx(-1);
  cell->set_end_col_offset_idx(0);
  require_equal(grid_text(render::derived_table_grid(data)), ".,./.,last",
                "a negative offset counts back from the end, as the host language's indexing does");

  auto* off_front = data.add_table_cells();
  off_front->set_text("gone");
  off_front->set_start_row_offset_idx(-9);
  off_front->set_end_row_offset_idx(-8);
  off_front->set_start_col_offset_idx(-9);
  off_front->set_end_col_offset_idx(-8);
  require_equal(grid_text(render::derived_table_grid(data)), ".,./.,last",
                "an offset so negative it falls off the front reaches no position");
}

void verify_the_derived_grid_ignores_the_wire_grid() {
  docv1::TableData data;
  data.set_num_rows(1);
  data.set_num_cols(1);
  add_cell(&data, data.add_grid(), "flat", false, 0, 0);
  data.mutable_grid(0)->mutable_cells(0)->set_text("wire");
  require_equal(grid_text(render::derived_table_grid(data)), "flat",
                "the wire grid is a redundant projection the derived layout never reads");
}

void verify_custom_fields_order_by_their_final_name() {
  google::protobuf::Map<std::string, google::protobuf::Value> fields;
  fields["zeta__b"].set_string_value("z");
  fields["alpha__a"].set_string_value("a");
  const auto ordered = render::ordered_custom_fields(fields);
  require_equal(ordered.size(), std::size_t{2}, "both conforming names are kept");
  require_equal(ordered[0].first, "alpha__a", "the conforming names sort in byte order");
  require_equal(ordered[1].first, "zeta__b", "the second conforming name follows");
}

void verify_a_non_conforming_name_moves_under_the_pipestream_namespace() {
  google::protobuf::Map<std::string, google::protobuf::Value> fields;
  fields["cell ref"].set_string_value("A1");
  fields["ok__name"].set_string_value("kept");
  const auto ordered = render::ordered_custom_fields(fields);
  require_equal(ordered.size(), std::size_t{2}, "both entries survive the rename");
  require_equal(ordered[0].first, "ok__name", "a conforming name is left where it is");
  require_equal(ordered[1].first, "pipestream__cell_ref",
                "a name without a namespace moves under pipestream with its specials folded");
}

void verify_a_rename_collision_takes_a_numeric_suffix() {
  google::protobuf::Map<std::string, google::protobuf::Value> fields;
  fields["cell ref"].set_string_value("first");
  fields["cell-ref"].set_string_value("second");
  const auto ordered = render::ordered_custom_fields(fields);
  require_equal(ordered.size(), std::size_t{2}, "both colliding names survive");
  require_equal(ordered[0].first, "pipestream__cell_ref", "the first by byte order keeps the name");
  require_equal(ordered[1].first, "pipestream__cell_ref_2",
                "the collision takes the next suffix, decided in byte order");
}

void verify_a_null_payload_is_dropped() {
  google::protobuf::Map<std::string, google::protobuf::Value> fields;
  fields["a__null"].set_null_value(google::protobuf::NULL_VALUE);
  fields["a__unset"];
  fields["a__kept"].set_number_value(1);
  const auto ordered = render::ordered_custom_fields(fields);
  require_equal(ordered.size(), std::size_t{1}, "only the entry with a payload survives");
  require_equal(ordered[0].first, "a__kept", "the surviving entry is the one with a value");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("renderer-base-test", "ok", {
      verify_every_arena_prefix_parses,
      verify_a_reference_that_names_no_arena_slot_is_unknown,
      verify_text_base_is_null_only_for_code_and_unset,
      verify_heading_rank_lifts_and_clamps,
      verify_trimming_strips_the_whitespace_family,
      verify_the_two_escapes_differ_only_on_the_quote,
      verify_the_code_fence_language_lower_cases_all_but_the_punctuated_tags,
      verify_the_code_language_string_is_the_canonical_spelling,
      verify_the_human_language_string_is_a_bcp47_code,
      verify_uri_normalization_touches_only_what_the_model_touches,
      verify_the_wire_grid_wins_over_the_flat_cell_list,
      verify_the_flat_cell_list_places_itself_when_there_is_no_grid,
      verify_a_cell_reaching_past_the_declared_grid_is_capped,
      verify_a_table_without_a_declared_size_has_no_derived_layout,
      verify_the_derived_grid_wraps_a_negative_offset,
      verify_the_derived_grid_ignores_the_wire_grid,
      verify_custom_fields_order_by_their_final_name,
      verify_a_non_conforming_name_moves_under_the_pipestream_namespace,
      verify_a_rename_collision_takes_a_numeric_suffix,
      verify_a_null_payload_is_dropped,
  });
}
