// The structural half of the Markdown export: reference resolution over the
// arenas, the caption/footnote/exclusion sets, the pre-order body walk, and
// the group, list and inline-group shapes the walk assembles its parts into.
// Per-item emission is the derived renderer's job and reaches this class
// through the one `serialize` seam. Internal to the export renderers;
// include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_MARKDOWN_WALK_H
#define GRPARSE_RENDER_MARKDOWN_WALK_H

#include <set>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "renderer_base.h"

namespace grparse::render {

class MarkdownWalk : protected RendererBase {
 protected:
  explicit MarkdownWalk(const ai::pipestream::document::v1::Document& document)
      : RendererBase(document) {
    collect_reference_sets();
  }
  virtual ~MarkdownWalk() = default;

  // One serialized sibling. `first_span` is the reference of the first
  // document item the part's text came from, which is the one structural
  // fact a list group needs about a part: a part whose first item sits in an
  // inline group is appended to the preceding line instead of starting a new
  // one. Empty when the part covers no document item.
  struct Part {
    std::string text;
    std::string first_span;
  };

  // The per-item emission seam. The walk calls it for every node it yields;
  // the derived renderer owns what a node's text is.
  virtual std::string serialize(const std::string& ref, int list_level,
                                bool inline_scope,
                                std::string* first_span = nullptr) = 0;

  // -- reference resolution -------------------------------------------------

  const ai::pipestream::document::v1::GroupItem* group_at(
      const std::string& ref) const;
  const ai::pipestream::document::v1::BaseTextItem* text_at(
      const std::string& ref) const;
  const ai::pipestream::document::v1::TableItem* table_at(
      const std::string& ref) const;
  const ai::pipestream::document::v1::PictureItem* picture_at(
      const std::string& ref) const;

  // The child references of any node, in document order.
  std::vector<std::string> children_of(const std::string& ref) const;

  // The reference a node declares as its parent, or empty when it declares
  // none.
  std::string parent_of(const std::string& ref) const;

  bool excluded(const std::string& ref) const {
    return excluded_refs_.contains(ref);
  }

  bool is_inline_group(const std::string& ref) const;
  bool is_list_group(const std::string& ref) const;

  // -- the walk -------------------------------------------------------------

  // The serialized siblings under `root`, in document order.
  std::vector<Part> get_parts(const std::string& root, int list_level,
                              bool inline_scope);

  static std::vector<std::string> part_texts(const std::vector<Part>& parts);

  // The content of a group by its label: a list, an inline run, or a
  // transparent block container.
  std::string serialize_group_content(
      const std::string& ref,
      const ai::pipestream::document::v1::GroupItem& group, int list_level,
      bool inline_scope, std::string* first_span);

  // The list marker pieces for one item, following the reference's marker
  // rules: keep an already-valid original marker, keep any alphanumeric one
  // behind a generated "-", and number an enumerated item that carries no
  // marker of its own.
  std::string list_item_prefix(
      const std::string& ref,
      const ai::pipestream::document::v1::BaseTextItem& text);

  // The caption and footnote references some floating item claims. A caption
  // or footnote renders through the item that claims it, never on its own.
  std::set<std::string> caption_refs_;
  std::set<std::string> footnote_refs_;

 private:
  ai::pipestream::document::v1::ContentLayer layer_of(
      const std::string& ref) const;
  // The label the exclusion test reads, or unspecified for a node that is
  // not a document item (groups are never excluded).
  ai::pipestream::document::v1::DocItemLabel label_of(
      const std::string& ref) const;
  bool is_document_item(const std::string& ref) const;

  void collect_reference_sets();
  void walk_for_exclusions(const std::string& ref, std::set<std::string>* seen);

  // Pre-order references under `root`, yielding only body-layer nodes but
  // descending regardless, and stopping at a picture's children unless they
  // are that picture's own captions.
  void collect_walk(const std::string& ref, std::set<std::string>* seen,
                    std::vector<std::string>* out) const;

  static std::string first_part_span(const std::vector<Part>& parts);

  std::string serialize_list(const std::string& ref, int list_level,
                             bool inline_scope, std::string* first_span);
  // Whether a part's first document item declares an inline group as its
  // parent, which is what makes the list append the part to the line before
  // it instead of opening a new one.
  bool span_sits_in_inline_group(const std::string& span) const;
  bool first_item_is_enumerated(const std::string& group_ref) const;

  // The body-tree items the label vocabulary drops.
  std::set<std::string> excluded_refs_;
};

}  // namespace grparse::render

#endif
