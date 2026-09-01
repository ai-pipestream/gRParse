// The fold from a parsed storage tree into a Document fragment. Internal to
// the storage handler; include/grparse/confluence_storage.h stays the only
// public surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_FOLD_H
#define GRPARSE_CONFLUENCE_STORAGE_FOLD_H

#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/repeated_ptr_field.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "storage_node.h"
#include "storage_table.h"

namespace grparse::confluence {

namespace docv1 = ai::pipestream::document::v1;

// The inline state one run of characters was written in.
struct InlineStyle {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  docv1::Script script = docv1::SCRIPT_UNSPECIFIED;
  std::string hyperlink;
};

struct InlineRun {
  std::string text;
  InlineStyle style;
};

class StorageFold {
 public:
  StorageFold(docv1::Document* document, std::vector<std::string>* warnings)
      : document_(document), warnings_(warnings) {}

  // Walks a container's children: inline content accumulates into one text
  // item, block content is emitted where it appears.
  void fold_blocks(const Node& container, const std::string& parent_ref);

  int emitted() const { return emitted_; }

 private:
  struct TextHandle {
    docv1::BaseTextItem* item = nullptr;
    docv1::TextItemBase* base = nullptr;
    std::string ref;
  };

  enum class TextKind { kSectionHeader, kList, kText };

  // A meta stamp (panel or macro name) that every item created inside the
  // scope carries, unwound when the scope ends.
  class StampScope {
   public:
    StampScope(StorageFold* fold, std::string key, std::string value)
        : fold_(fold) {
      if (value.empty()) {
        fold_ = nullptr;
        return;
      }
      fold_->stamps_.emplace_back(std::move(key), std::move(value));
    }
    ~StampScope() {
      if (fold_ != nullptr) fold_->stamps_.pop_back();
    }
    StampScope(const StampScope&) = delete;
    StampScope& operator=(const StampScope&) = delete;

   private:
    StorageFold* fold_;
  };

  docv1::GroupItem* group_by_ref(const std::string& ref);
  void link_child(const std::string& parent_ref, const std::string& child_ref);
  void stamp_source(
      google::protobuf::RepeatedPtrField<docv1::SourceType>* source);
  template <typename Meta>
  void stamp_meta(Meta* meta);

  docv1::GroupItem* add_group(const std::string& parent_ref,
                                   docv1::GroupLabel label,
                                   const std::string& name);
  TextHandle add_text(TextKind kind, docv1::DocItemLabel label,
                      const std::string& parent_ref);

  void collect_inline_node(const Node& node, InlineStyle style,
                           std::vector<InlineRun>* runs);
  void collect_inline_children(const Node& node, const InlineStyle& style,
                               std::vector<InlineRun>* runs);
  void collect_link(const Node& node, InlineStyle style,
                    std::vector<InlineRun>* runs);
  // The runs one table cell contributes: paragraphs written directly in it
  // are separated by a newline, everything else folds inline.
  std::vector<InlineRun> collect_cell_runs(const Node& cell);
  void apply_inline(const std::vector<InlineRun>& runs,
                    docv1::TextItemBase* base);
  void flush_inline(std::vector<InlineRun>* runs, const std::string& parent_ref);

  void emit_block(const Node& node, const std::string& parent_ref);
  void emit_heading(const Node& node, int level, const std::string& parent_ref);
  void emit_paragraph(const Node& node, const std::string& parent_ref);
  void emit_list(const Node& node, const std::string& parent_ref, bool ordered);
  void emit_list_item(const Node& node, const std::string& group_ref,
                      bool ordered, int position);
  void emit_task_list(const Node& node, const std::string& parent_ref);
  void emit_table(const Node& node, const std::string& parent_ref);
  // The table's own item, linked and stamped, with its row count set.
  docv1::TableItem* add_table(const std::string& parent_ref, int num_rows);
  // Places every cell of the parsed rows into the data, returning the column
  // count the placement reached and reporting whether a span was clamped.
  int place_table_cells(const std::vector<TableRowNode>& rows,
                        docv1::TableData* data, bool* clamped);
  void place_table_row(const TableRowNode& row, int row_index, int num_rows,
                       std::vector<std::vector<bool>>* occupied,
                       docv1::TableData* data, int* num_cols, bool* clamped);
  void emit_macro(const Node& node, const std::string& parent_ref);
  void emit_code_macro(const Node& node, const std::string& parent_ref);
  void emit_image(const Node& node, const std::string& parent_ref);
  void warn(std::string message) {
    if (warnings_ != nullptr) warnings_->push_back(std::move(message));
  }

  docv1::Document* document_ = nullptr;
  std::vector<std::string>* warnings_ = nullptr;
  std::vector<std::pair<std::string, std::string>> stamps_;
  int emitted_ = 0;
};

}  // namespace grparse::confluence

#endif
