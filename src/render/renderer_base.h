// Internal seams shared by the export renderers (src/render/*.cpp): the
// arena-reference parser, the text-variant accessor, the table grid
// materializer, escape/trim helpers, and the RendererBase walk state. Not
// part of the public API; include/grparse/document_render.h stays the only
// public surface.
#ifndef GRPARSE_RENDER_RENDERER_BASE_H
#define GRPARSE_RENDER_RENDERER_BASE_H

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

// The table's cell layout as a row-major pointer grid. The grid field wins
// when populated; otherwise the flat cell list is placed by its offsets.
// A spanned cell appears at every position it covers; nullptr marks a
// position no cell reaches.
std::vector<std::vector<const ai::pipestream::document::v1::TableCell*>> table_grid(
    const ai::pipestream::document::v1::TableData& data);

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

  bool consume(const std::string& ref) { return consumed_.insert(ref).second; }

  // True for any content layer the exports leave out: everything except the
  // body layer and the unspecified default.
  bool excluded_layer(ai::pipestream::document::v1::ContentLayer layer) const {
    return layer != ai::pipestream::document::v1::CONTENT_LAYER_BODY &&
           layer != ai::pipestream::document::v1::CONTENT_LAYER_UNSPECIFIED;
  }

  // The caption texts a table or figure references, in reference order.
  // Each resolved caption is consumed so the tree walk skips it later.
  std::vector<std::string> caption_texts(
      const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::RefItem>&
          captions);
};

}  // namespace grparse::render

#endif
