// Internal seams shared by the export renderers (src/render/*.cpp): the
// arena-reference parser, the text-variant accessor, the table grid
// materializer, escape/trim helpers, and the RendererBase walk state. Not
// part of the public API; include/grparse/document_render.h stays the only
// public surface.
#ifndef GRPARSE_RENDER_RENDERER_BASE_H
#define GRPARSE_RENDER_RENDERER_BASE_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// A parsed "#/<arena>/<index>" reference. The body and furniture roots and
// anything unparseable resolve to kUnknown; renderers skip those rather than
// guess.
struct ArenaRef {
  enum Kind {
    kText,
    kTable,
    kPicture,
    kGroup,
    kKeyValue,
    kForm,
    kFieldRegion,
    kFieldItem,
    kUnknown,
  };
  Kind kind = kUnknown;
  int index = -1;
};

ArenaRef parse_ref(const std::string& ref);

// The shared base fields of any text variant that carries a nested base;
// nullptr for CodeItem (inline fields) and unset variants.
const ai::pipestream::document::v1::TextItemBase* text_base(
    const ai::pipestream::document::v1::BaseTextItem& item);

// Heading depth for a section header: docling maps level L to "##"×(L+1) in
// Markdown and <h(L+1)> in HTML, clamped to h6. An unset proto level (0)
// counts as level 1.
int heading_rank(int level);

// Whitespace-trimmed copy, mirroring docling's str.strip() on item text.
std::string trimmed(const std::string& text);

// The fence info string for a code block, preferring the collector's raw
// language string over the enum. Enum names lower-case cleanly except the
// spelled-out punctuation ones.
std::string code_fence_language(const ai::pipestream::document::v1::CodeItem& code);

// What one document's tables may cost together. Each table is already
// capped on its own, but a document of many capped tables could still
// render gigabytes; a renderer (or the chunker) keeps one budget per
// document and every grid it builds spends from it. The positions bound
// the grid itself. A spanned cell repeats at every position it covers, so
// its extra positions and the text they repeat spend from two more pools:
// without them one cell spanning a large grid would print its text
// millions of times. A table built after the positions run out keeps
// none; a spanned cell whose repeats no longer fit lands at its first
// position only. Either way one stderr line per document reports it.
class GridBudget {
 public:
  static constexpr std::int64_t kDocumentPositions = std::int64_t{1} << 23;
  static constexpr std::int64_t kDocumentRepeatPositions = std::int64_t{1} << 23;
  static constexpr std::int64_t kDocumentRepeatBytes = std::int64_t{1} << 26;
  std::int64_t remaining() const { return remaining_; }
  void spend(std::int64_t positions) { remaining_ -= std::min(positions, remaining_); }

  // Spends `repeats` extra positions of a cell whose text is `bytes` long;
  // false, spending nothing, when either pool would run out.
  bool spend_repeats(std::int64_t repeats, std::int64_t bytes) {
    if (repeats <= 0) return true;
    if (repeats > repeat_positions_) return false;
    if (bytes > 0 && repeats > repeat_bytes_ / bytes) return false;
    repeat_positions_ -= repeats;
    repeat_bytes_ -= repeats * std::max<std::int64_t>(bytes, 0);
    return true;
  }
  std::int64_t repeat_positions_remaining() const { return repeat_positions_; }
  std::int64_t repeat_bytes_remaining() const { return repeat_bytes_; }

  // True on the first truncation of the document, so it logs once.
  bool first_truncation() { return !std::exchange(truncated_, true); }
  bool truncated() const { return truncated_; }

 private:
  std::int64_t remaining_ = kDocumentPositions;
  std::int64_t repeat_positions_ = kDocumentRepeatPositions;
  std::int64_t repeat_bytes_ = kDocumentRepeatBytes;
  bool truncated_ = false;
};

// The table's cell layout as a row-major pointer grid. The grid field wins
// when populated; otherwise the flat cell list is placed by its offsets.
// A spanned cell appears at every position it covers while `budget` can
// pay for the repeats, and at its first position only once it cannot;
// nullptr marks a position no cell reaches. The declared dimensions are
// untrusted: a grid above a fixed position budget, or above what `budget`
// has left, keeps only its leading rows and columns, and a warning goes to
// stderr (derived_table_grid caps the same way). A wire grid counts as its row
// count times its widest row, since a jagged grid renders padded to that.
std::vector<std::vector<const ai::pipestream::document::v1::TableCell*>> table_grid(
    const ai::pipestream::document::v1::TableData& data, GridBudget& budget);

// The cell layout the model derives for its computed grid field: a
// num_rows x num_cols rectangle of empty positions that every declared cell
// overwrites wherever it reaches. The wire grid is a redundant projection and
// is not read. An offset past the last row or column is capped, and a
// negative one counts back from the end, as the host language's indexing
// does; an offset so negative that it falls off the front reaches no position
// at all (the model raises there, which an export must not). nullptr marks a
// position no cell covers. Repeats spend from `budget` as in table_grid.
std::vector<std::vector<const ai::pipestream::document::v1::TableCell*>>
derived_table_grid(const ai::pipestream::document::v1::TableData& data, GridBudget& budget);

// The model layer parses hyperlink/uri strings through a URL type whose
// serializer normalizes them; the states this service and its collectors
// produce are covered here: scheme lowercasing and, for the special schemes,
// host lowercasing plus an explicit "/" path when the path is empty. Strings
// without a scheme pass through untouched (path semantics). Exotic
// normalizations (percent-encoding, IDNA) are out of scope and would surface
// in the validation oracles if a producer ever hit them.
std::string normalized_uri(const std::string& uri);

// The canonical spelling of a code language tag ("C++", "Python"), or empty
// for an unset, unspecified, or unknown tag.
std::optional<std::string_view> code_language_string(
    ai::pipestream::document::v1::CodeLanguageLabel tag);

// The BCP-47 code of a human language tag ("en"), or empty for an unset,
// unspecified, or unknown tag.
std::optional<std::string> human_language_string(
    ai::pipestream::document::v1::HumanLanguageLabel tag);

// The custom meta fields of one node, in the order the exports emit them.
// The model layer requires a "namespace__field_name" key, so a name without
// one moves under the "pipestream" namespace with every character outside
// [A-Za-z0-9_] folded to an underscore and a _2, _3, ... suffix breaking a
// collision; renaming considers the names in byte order so the suffix always
// lands on the same entry. The result is ordered by the final name, which
// makes an unordered wire map export deterministically. Entries with a null
// payload are dropped: the model's dump excludes them.
std::vector<std::pair<std::string, const google::protobuf::Value*>>
ordered_custom_fields(
    const google::protobuf::Map<std::string, google::protobuf::Value>& fields);

std::string escape_html_text(const std::string& text);

std::string escape_html_attribute(const std::string& text);

// XML 1.0 escaping for the DocLang exports: & < > (and " plus TAB, LF, CR as
// character references in an attribute), with every byte that is not valid
// UTF-8 and every code point outside the XML Char production (C0 controls
// other than TAB, LF, CR; U+FFFE; U+FFFF) replaced by U+FFFD, so a strict XML
// parser accepts the output whatever the source text held.
std::string escape_xml_text(std::string_view text);

std::string escape_xml_attribute(std::string_view text);

// The picture's description text. The meta field wins; the annotation list
// is the fallback for producers that still write description annotations.
// Empty when the picture carries no description.
std::string picture_description(
    const ai::pipestream::document::v1::PictureItem& picture);

// The picture's top classification class name: the highest-confidence meta
// prediction wins, the first annotation classification falls back. Empty
// when the picture carries no classification.
std::string picture_classification_class(
    const ai::pipestream::document::v1::PictureItem& picture);

// Both renderers walk the body tree the same way: resolve each child
// reference, render the item, and record caption items when a table or
// figure claims them so a caption linked into the tree twice never renders
// twice. Only body-layer items render: an unspecified layer is the producer
// default and counts as body, while every other layer (furniture, notes,
// invisible, ...) is one the producer chose deliberately and is excluded
// from the exports.
class RendererBase {
 protected:
  explicit RendererBase(const ai::pipestream::document::v1::Document& document)
      : document_(document) {}

  const ai::pipestream::document::v1::Document& document_;
  std::set<std::string> consumed_;
  // Every table grid this document's render builds spends from it; mutable
  // because the const serializers build grids too.
  mutable GridBudget grid_budget_;

  bool consume(const std::string& ref) { return consumed_.insert(ref).second; }

  // True for any content layer the exports leave out: everything except the
  // body layer and the unspecified default.
  bool excluded_layer(ai::pipestream::document::v1::ContentLayer layer) const {
    return layer != ai::pipestream::document::v1::CONTENT_LAYER_BODY &&
           layer != ai::pipestream::document::v1::CONTENT_LAYER_UNSPECIFIED;
  }

  // The caption items a table or figure references, in reference order;
  // entries whose reference does not resolve to a text base are skipped.
  // Each resolved caption is consumed so the tree walk skips it later.
  std::vector<const ai::pipestream::document::v1::TextItemBase*> caption_bases(
      const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::RefItem>&
          captions);

  // The caption texts a table or figure references, in reference order.
  // Each resolved caption is consumed so the tree walk skips it later.
  std::vector<std::string> caption_texts(
      const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::RefItem>&
          captions);
};

}  // namespace grparse::render

#endif
