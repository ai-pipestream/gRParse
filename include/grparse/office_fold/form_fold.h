// The form plane: the region every field item hangs from, the form whose
// graph pairs each key with its value, and one field item per office form
// field holding its heading and its value.
#pragma once

#include <string>

#include "grparse/office_fold/fold_base.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class FormFold : public FoldBase {
 public:
  FormFold(DocumentArena& arena, AnchorIndex& anchors)
      : FoldBase(arena, anchors) {}

  void on_form_field(const officev1::FormField& field);

 private:
  // Creates the form region and the form whose graph pairs the fields, once
  // the first form field arrives.
  void ensure_form_arena();
  // The field item itself, hanging from the region.
  docv1::FieldItem* add_field_item(std::string* field_ref);
  // The key-to-value pairing, as the graph the form arena is built around.
  void link_graph(const std::string& heading_ref, const std::string& label,
                  const std::string& value_ref, const std::string& text,
                  bool checkbox);
  // The field's own identity: what the form calls it, what a choice field
  // offers and which entry is chosen, and the parameters a fieldmark
  // stores.
  void set_identity(const officev1::FormField& field, docv1::FieldItem* item);
  // Where the field sits. A draw-page form control is told from an in-text
  // fieldmark by whether it carries a laid-out box.
  void add_field_prov(const officev1::FormField& field, docv1::FieldItem* item,
                      const TextHandle& value);

  // The lazily created form arena: the region every field item hangs from.
  std::string field_region_ref_;
  int graph_cell_id_ = 0;
};

}  // namespace grparse::office_fold
