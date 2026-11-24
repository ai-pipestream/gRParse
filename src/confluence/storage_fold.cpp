#include "storage_fold.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "storage_blocks.h"
#include "storage_node.h"
#include "storage_table.h"
#include "storage_text.h"

namespace grparse::confluence {
namespace {

// The attribution every item this handler creates carries. "native" says the
// parse ran in this process, with no model and no remote collector behind it.
constexpr char kCollector[] = "confluence-storage";
constexpr char kModel[] = "native";

// One span for each stretch of text that shares a link target, in code
// points of the text the runs join into. Returns the first target, which
// the caller keeps in the item's own hyperlink slot.
std::string add_link_spans(
    const std::vector<InlineRun>& runs,
    google::protobuf::RepeatedPtrField<docv1::InlineSpan>* spans) {
  std::string first;
  long long local = 0;
  long long start = 0;
  std::string url;
  const auto flush = [&]() {
    if (url.empty()) return;
    if (local > start) {
      docv1::InlineSpan* span = spans->Add();
      span->mutable_range()->set_start(static_cast<int32_t>(start));
      span->mutable_range()->set_end(static_cast<int32_t>(local));
      span->set_hyperlink(url);
      if (first.empty()) first = url;
    }
    url.clear();
  };
  for (const InlineRun& run : runs) {
    if (run.style.hyperlink != url) {
      flush();
      url = run.style.hyperlink;
      start = local;
    }
    local += code_points(run.text);
  }
  flush();
  return first;
}

bool same_formatting(const InlineStyle& left, const InlineStyle& right) {
  return left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline &&
         left.strikethrough == right.strikethrough && left.script == right.script;
}

bool plain_formatting(const InlineStyle& style) {
  return !style.bold && !style.italic && !style.underline &&
         !style.strikethrough && style.script == docv1::SCRIPT_UNSPECIFIED;
}

// Drops empty runs and trims the outer whitespace of the item's text; interior
// spacing between runs is the author's and stays.
void trim_runs(std::vector<InlineRun>* runs) {
  std::erase_if(*runs, [](const InlineRun& run) { return run.text.empty(); });
  if (runs->empty()) return;
  std::string& first = runs->front().text;
  const std::string_view trimmed_first = trim(std::string_view(first));
  first.erase(0, static_cast<size_t>(trimmed_first.data() - first.data()));
  std::string& last = runs->back().text;
  size_t end = last.size();
  while (end > 0 && is_space(last[end - 1])) --end;
  last.resize(end);
  std::erase_if(*runs, [](const InlineRun& run) { return run.text.empty(); });
}

std::string runs_text(const std::vector<InlineRun>& runs) {
  std::string text;
  for (const InlineRun& run : runs) text.append(run.text);
  return text;
}

// Appends a paragraph's runs behind the ones already collected, separated by
// a newline; an empty paragraph contributes nothing.
void append_paragraph_runs(std::vector<InlineRun> paragraph,
                           std::vector<InlineRun>* runs) {
  trim_runs(&paragraph);
  if (paragraph.empty()) return;
  if (!runs->empty()) runs->push_back({"\n", InlineStyle{}});
  runs->insert(runs->end(), std::make_move_iterator(paragraph.begin()),
               std::make_move_iterator(paragraph.end()));
}

}  // namespace

docv1::GroupItem* StorageFold::group_by_ref(const std::string& ref) {
  if (ref == "#/furniture") return document_->mutable_furniture();
  static constexpr std::string_view kPrefix = "#/groups/";
  if (ref.starts_with(kPrefix)) {
    const int index = std::stoi(ref.substr(kPrefix.size()));
    if (index >= 0 && index < document_->groups_size()) {
      return document_->mutable_groups(index);
    }
  }
  return document_->mutable_body();
}

void StorageFold::link_child(const std::string& parent_ref,
                             const std::string& child_ref) {
  group_by_ref(parent_ref)->add_children()->set_ref(child_ref);
}

bool StorageFold::drop_empty_group(const std::string& group_ref,
                                   const std::string& parent_ref) {
  if (group_by_ref(group_ref)->children_size() > 0) return false;
  // Every item a list or a macro body emits hangs under its group, so a
  // group with no children had nothing added after it: it is the last group
  // and its parent's last child. Checked anyway, so a wrong guess keeps the
  // group.
  docv1::GroupItem* parent = group_by_ref(parent_ref);
  const int last_group = document_->groups_size() - 1;
  const int last_child = parent->children_size() - 1;
  if (last_group < 0 || document_->groups(last_group).self_ref() != group_ref ||
      last_child < 0 || parent->children(last_child).ref() != group_ref) {
    return false;
  }
  parent->mutable_children()->RemoveLast();
  document_->mutable_groups()->RemoveLast();
  return true;
}

void StorageFold::drop_empty_list(const std::string& group_ref,
                                  const std::string& parent_ref) {
  if (drop_empty_group(group_ref, parent_ref)) {
    warn("a list carried no items and was dropped");
  }
}

void StorageFold::stamp_source(
    google::protobuf::RepeatedPtrField<docv1::SourceType>* source) {
  docv1::CollectorSource* collector = source->Add()->mutable_collector();
  collector->set_collector(kCollector);
  collector->set_model(kModel);
}

docv1::GroupItem* StorageFold::add_group(const std::string& parent_ref,
                                         docv1::GroupLabel label,
                                         const std::string& name) {
  const int index = document_->groups_size();
  docv1::GroupItem* group = document_->add_groups();
  group->set_self_ref("#/groups/" + std::to_string(index));
  group->mutable_parent()->set_ref(parent_ref);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  if (!name.empty()) group->set_name(name);
  stamp_source(group->mutable_source());
  link_child(parent_ref, group->self_ref());
  return group;
}

StorageFold::TextHandle StorageFold::add_text(TextKind kind,
                                              docv1::DocItemLabel label,
                                              const std::string& parent_ref) {
  TextHandle handle;
  handle.ref = "#/texts/" + std::to_string(document_->texts_size());
  handle.item = document_->add_texts();
  switch (kind) {
    case TextKind::kSectionHeader:
      handle.base = handle.item->mutable_section_header()->mutable_base();
      break;
    case TextKind::kList:
      handle.base = handle.item->mutable_list_item()->mutable_base();
      break;
    case TextKind::kText:
      handle.base = handle.item->mutable_text()->mutable_base();
      break;
  }
  handle.base->set_self_ref(handle.ref);
  handle.base->mutable_parent()->set_ref(parent_ref);
  handle.base->set_label(label);
  handle.base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(handle.base->mutable_source());
  if (!admonitions_.empty()) handle.base->set_admonition_kind(admonitions_.back());
  link_child(parent_ref, handle.ref);
  ++emitted_;
  return handle;
}

void StorageFold::collect_inline_children(const Node& node,
                                          const InlineStyle& style,
                                          std::vector<InlineRun>* runs) {
  for (const Node& child : node.children) collect_inline_node(child, style, runs);
}

void StorageFold::collect_link(const Node& node, InlineStyle style,
                               std::vector<InlineRun>* runs) {
  // The dialect names link targets by resource rather than by URL. What has
  // no URL keeps a pointer in the same shape the attachment pointers use, so
  // a consumer can resolve it later; nothing is invented.
  std::string label;
  if (const Node* page = find_child(node, "ri", "page")) {
    label = attribute_or_empty(*page, "ri", "content-title");
    if (!label.empty()) style.hyperlink = "confluence-page:" + label;
  } else if (const Node* attachment = find_child(node, "ri", "attachment")) {
    label = attribute_or_empty(*attachment, "ri", "filename");
    if (!label.empty()) style.hyperlink = "confluence-attachment:" + label;
  } else if (const Node* url = find_child(node, "ri", "url")) {
    label = attribute_or_empty(*url, "ri", "value");
    style.hyperlink = label;
  } else if (const Node* space = find_child(node, "ri", "space")) {
    label = attribute_or_empty(*space, "ri", "space-key");
    if (!label.empty()) style.hyperlink = "confluence-space:" + label;
  }
  const Node* plain_body = find_child(node, "ac", "plain-text-link-body");
  const Node* rich_body = find_child(node, "ac", "link-body");
  if (plain_body != nullptr) {
    const std::string text = raw_text(*plain_body);
    if (!text.empty()) runs->push_back({text, style});
    return;
  }
  if (rich_body != nullptr) {
    collect_inline_children(*rich_body, style, runs);
    return;
  }
  // A bodiless link renders as its target's own name.
  if (!label.empty()) runs->push_back({label, style});
}

void StorageFold::collect_inline_node(const Node& node, InlineStyle style,
                                      std::vector<InlineRun>* runs) {
  if (node.text_node) {
    if (!node.text.empty()) runs->push_back({node.text, style});
    return;
  }
  if (node.prefix.empty()) {
    if (node.name == "strong" || node.name == "b") {
      style.bold = true;
    } else if (node.name == "em" || node.name == "i") {
      style.italic = true;
    } else if (node.name == "u" || node.name == "ins") {
      style.underline = true;
    } else if (node.name == "s" || node.name == "del" || node.name == "strike") {
      style.strikethrough = true;
    } else if (node.name == "sub") {
      style.script = docv1::SCRIPT_SUB;
    } else if (node.name == "sup") {
      style.script = docv1::SCRIPT_SUPER;
    } else if (node.name == "a") {
      const std::string href = attribute_or_empty(node, "", "href");
      if (!href.empty()) style.hyperlink = href;
    } else if (node.name == "br") {
      runs->push_back({"\n", style});
      return;
    }
    collect_inline_children(node, style, runs);
    return;
  }
  if (node.prefix == "ac" && node.name == "link") {
    collect_link(node, std::move(style), runs);
    return;
  }
  // Every other inline macro (inline comment markers, emoticons, placeholders)
  // contributes whatever text it wraps.
  collect_inline_children(node, style, runs);
}

std::vector<InlineRun> StorageFold::collect_cell_runs(const Node& cell) {
  std::vector<InlineRun> runs;
  for (const Node& part : cell.children) {
    if (html_is(part, "p")) {
      std::vector<InlineRun> paragraph;
      collect_inline_children(part, InlineStyle{}, &paragraph);
      append_paragraph_runs(std::move(paragraph), &runs);
      continue;
    }
    collect_inline_node(part, InlineStyle{}, &runs);
  }
  trim_runs(&runs);
  return runs;
}

void StorageFold::apply_inline(const std::vector<InlineRun>& runs,
                               docv1::TextItemBase* base) {
  const std::string text = runs_text(runs);
  base->set_text(text);
  base->set_orig(text);
  if (runs.empty()) return;

  // Item-level formatting is only honest when every run agrees; mixed text
  // keeps its formatting unset, exactly as the office fold does.
  const InlineStyle& first = runs.front().style;
  const bool uniform = std::ranges::all_of(runs, [&first](const InlineRun& run) {
    return same_formatting(run.style, first);
  });
  if (uniform && !plain_formatting(first)) {
    docv1::Formatting* formatting = base->mutable_formatting();
    formatting->set_bold(first.bold);
    formatting->set_italic(first.italic);
    formatting->set_underline(first.underline);
    formatting->set_strikethrough(first.strikethrough);
    formatting->set_script(first.script);
  }

  // The first link lands in the item's own hyperlink slot, the house
  // convention; every link is a span with its own range.
  const std::string first_link = add_link_spans(runs, base->mutable_spans());
  if (!first_link.empty()) base->set_hyperlink(first_link);
}

void StorageFold::flush_inline(std::vector<InlineRun>* runs,
                               const std::string& parent_ref) {
  trim_runs(runs);
  if (runs->empty()) return;
  const TextHandle handle =
      add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, parent_ref);
  apply_inline(*runs, handle.base);
  runs->clear();
}

void StorageFold::fold_blocks(const Node& container,
                              const std::string& parent_ref) {
  std::vector<InlineRun> pending;
  for (const Node& child : container.children) {
    if (is_block(child)) {
      flush_inline(&pending, parent_ref);
      emit_block(child, parent_ref);
      continue;
    }
    collect_inline_node(child, InlineStyle{}, &pending);
  }
  flush_inline(&pending, parent_ref);
}

void StorageFold::emit_block(const Node& node, const std::string& parent_ref) {
  if (node.prefix == "ac") {
    if (node.name == "structured-macro") {
      emit_macro(node, parent_ref);
      return;
    }
    if (node.name == "task-list") {
      emit_task_list(node, parent_ref);
      return;
    }
    if (node.name == "image") {
      emit_image(node, parent_ref);
      return;
    }
    // Layout wrappers and unmapped container macros are transparent.
    fold_blocks(node, parent_ref);
    return;
  }
  if (!node.prefix.empty()) {
    fold_blocks(node, parent_ref);
    return;
  }
  if (node.name.size() == 2 && node.name[0] == 'h' && node.name[1] >= '1' &&
      node.name[1] <= '6') {
    emit_heading(node, node.name[1] - '0', parent_ref);
    return;
  }
  if (node.name == "p") {
    emit_paragraph(node, parent_ref);
    return;
  }
  if (node.name == "ul") {
    emit_list(node, parent_ref, false);
    return;
  }
  if (node.name == "ol") {
    emit_list(node, parent_ref, true);
    return;
  }
  if (node.name == "table") {
    emit_table(node, parent_ref);
    return;
  }
  // A horizontal rule separates without carrying anything to map.
  if (node.name == "hr") return;
  // Every other XHTML element is a transparent container: its blocks are
  // emitted where they stand and its loose text becomes a text item.
  fold_blocks(node, parent_ref);
}

void StorageFold::emit_heading(const Node& node, int level,
                               const std::string& parent_ref) {
  std::vector<InlineRun> runs;
  collect_inline_children(node, InlineStyle{}, &runs);
  trim_runs(&runs);
  if (runs.empty()) return;
  const TextHandle handle = add_text(
      TextKind::kSectionHeader, docv1::DOC_ITEM_LABEL_SECTION_HEADER, parent_ref);
  handle.item->mutable_section_header()->set_level(level);
  apply_inline(runs, handle.base);
}

void StorageFold::emit_paragraph(const Node& node,
                                 const std::string& parent_ref) {
  // A paragraph can wrap a block (an image or a macro is routinely written
  // inside one); those keep their own item and the loose text keeps its own.
  fold_blocks(node, parent_ref);
}

void StorageFold::emit_list(const Node& node, const std::string& parent_ref,
                            bool ordered) {
  docv1::GroupItem* group = add_group(
      parent_ref,
      ordered ? docv1::GROUP_LABEL_ORDERED_LIST : docv1::GROUP_LABEL_LIST,
      "list");
  const std::string group_ref = group->self_ref();
  int position = 0;
  for (const Node& child : node.children) {
    if (html_is(child, "li")) {
      emit_list_item(child, group_ref, ordered, ++position);
      continue;
    }
    if (html_is(child, "ul") || html_is(child, "ol")) {
      // A sublist written beside the items instead of inside one still
      // nests: the group is the list's own child.
      emit_list(child, group_ref, html_is(child, "ol"));
      continue;
    }
    if (child.text_node) {
      if (!blank(child.text)) {
        std::vector<InlineRun> runs{{child.text, InlineStyle{}}};
        flush_inline(&runs, group_ref);
      }
      continue;
    }
    emit_block(child, group_ref);
  }
  drop_empty_list(group_ref, parent_ref);
}

void StorageFold::emit_list_item(const Node& node, const std::string& group_ref,
                                 bool ordered, int position) {
  std::vector<InlineRun> runs;
  std::vector<const Node*> blocks;
  for (const Node& child : node.children) {
    // The item's own text is the text of the paragraphs written directly in
    // it; a paragraph that wraps a block is a block, or the block it wraps
    // would be flattened into text and lost.
    if (html_is(child, "p") && !contains_block(child)) {
      std::vector<InlineRun> paragraph;
      collect_inline_children(child, InlineStyle{}, &paragraph);
      append_paragraph_runs(std::move(paragraph), &runs);
      continue;
    }
    if (is_block(child)) {
      blocks.push_back(&child);
      continue;
    }
    collect_inline_node(child, InlineStyle{}, &runs);
  }
  trim_runs(&runs);
  const TextHandle handle =
      add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM, group_ref);
  apply_inline(runs, handle.base);
  docv1::ListItem* item = handle.item->mutable_list_item();
  item->set_enumerated(ordered);
  item->set_marker(ordered ? std::to_string(position) + "." : "-");
  // Nested lists and any other block the item carries hang off the list
  // group, so the item stays a leaf and the nesting stays a group tree.
  for (const Node* block : blocks) emit_block(*block, group_ref);
}

void StorageFold::emit_task_list(const Node& node,
                                 const std::string& parent_ref) {
  docv1::GroupItem* group =
      add_group(parent_ref, docv1::GROUP_LABEL_LIST, "task-list");
  const std::string group_ref = group->self_ref();
  for (const Node& child : node.children) {
    if (!element_is(child, "ac", "task")) continue;
    const Node* status = find_child(child, "ac", "task-status");
    const bool complete =
        status != nullptr && lowercase(std::string(trim(raw_text(*status)))) == "complete";
    std::vector<InlineRun> runs;
    if (const Node* body = find_child(child, "ac", "task-body")) {
      collect_inline_children(*body, InlineStyle{}, &runs);
    }
    trim_runs(&runs);
    const TextHandle handle =
        add_text(TextKind::kList,
                 complete ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                          : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED,
                 group_ref);
    apply_inline(runs, handle.base);
    docv1::ListItem* item = handle.item->mutable_list_item();
    item->set_enumerated(false);
    item->set_marker("-");
  }
  drop_empty_list(group_ref, parent_ref);
}

docv1::TableItem* StorageFold::add_table(const std::string& parent_ref,
                                         int num_rows) {
  const std::string ref = "#/tables/" + std::to_string(document_->tables_size());
  docv1::TableItem* table = document_->add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent_ref);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(table->mutable_source());
  link_child(parent_ref, ref);
  ++emitted_;
  table->mutable_data()->set_num_rows(num_rows);
  return table;
}

void StorageFold::place_table_row(const TableRowNode& row, int row_index,
                                  int num_rows,
                                  std::vector<std::vector<bool>>* occupied,
                                  docv1::TableData* data, int* num_cols,
                                  bool* clamped) {
  int column = 0;
  for (const Node& cell : row.row->children) {
    if (!html_is(cell, "td") && !html_is(cell, "th")) continue;
    auto& slots = (*occupied)[static_cast<size_t>(row_index)];
    while (column < static_cast<int>(slots.size()) &&
           slots[static_cast<size_t>(column)]) {
      ++column;
    }
    int row_span = span_attribute(cell, "rowspan", clamped);
    int col_span = span_attribute(cell, "colspan", clamped);
    if (row_index + row_span > num_rows) {
      row_span = num_rows - row_index;
      *clamped = true;
    }
    if (column >= kMaxColumns) {
      // Past the ceiling there is no slot left to place into; the cell's
      // text would need a column that cannot be addressed.
      *clamped = true;
      break;
    }
    if (column + col_span > kMaxColumns) {
      col_span = kMaxColumns - column;
      *clamped = true;
    }
    reserve_slots(occupied, row_index, column, row_span, col_span);

    const std::vector<InlineRun> runs = collect_cell_runs(cell);
    const bool header_cell = html_is(cell, "th");
    docv1::TableCell* out = data->add_table_cells();
    out->set_start_row_offset_idx(row_index);
    out->set_end_row_offset_idx(row_index + row_span);
    out->set_start_col_offset_idx(column);
    out->set_end_col_offset_idx(column + col_span);
    out->set_row_span(row_span);
    out->set_col_span(col_span);
    out->set_text(runs_text(runs));
    add_link_spans(runs, out->mutable_spans());
    out->set_column_header(header_cell && (row.head || row_index == 0));
    out->set_row_header(header_cell && !out->column_header());
    column += col_span;
    *num_cols = std::max(*num_cols, column);
  }
}

int StorageFold::place_table_cells(const std::vector<TableRowNode>& rows,
                                   docv1::TableData* data, bool* clamped) {
  // The placement walk of the HTML table model: a cell takes the first free
  // column of its row, and its spans reserve the slots below and to the
  // right for the rows that follow.
  const int num_rows = static_cast<int>(rows.size());
  std::vector<std::vector<bool>> occupied(static_cast<size_t>(num_rows));
  int num_cols = 0;
  for (int row = 0; row < num_rows; ++row) {
    place_table_row(rows[static_cast<size_t>(row)], row, num_rows, &occupied, data,
                    &num_cols, clamped);
  }
  return num_cols;
}

void StorageFold::emit_table(const Node& node, const std::string& parent_ref) {
  std::vector<TableRowNode> rows;
  collect_rows(node, false, &rows);

  const int num_rows = static_cast<int>(rows.size());
  docv1::TableData* data = add_table(parent_ref, num_rows)->mutable_data();
  if (num_rows == 0) {
    data->set_num_cols(0);
    return;
  }

  bool clamped = false;
  const int num_cols = place_table_cells(rows, data, &clamped);
  data->set_num_cols(num_cols);
  if (clamped) {
    warn("a table cell declared a span beyond the table and was clamped to it");
  }

  if (num_cols <= 0 ||
      static_cast<long long>(num_rows) * num_cols > kMaxGridCells) {
    return;
  }
  materialize_grid(data, num_rows, num_cols);
}

void StorageFold::emit_macro(const Node& node, const std::string& parent_ref) {
  const std::string name = lowercase(attribute_or_empty(node, "ac", "name"));
  if (name == "code") {
    emit_code_macro(node, parent_ref);
    return;
  }
  const std::string shown = name.empty() ? std::string("unnamed") : name;
  const bool has_body = std::ranges::any_of(node.children, [](const Node& child) {
    return element_is(child, "ac", "rich-text-body") ||
           element_is(child, "ac", "plain-text-body");
  });
  if (!has_body) {
    // A bodiless macro (a table of contents, a status lozenge) carries only
    // its parameters, which are configuration rather than page content.
    warn("macro '" + shown + "' carried no body; its parameters were not mapped");
    return;
  }

  // Every other macro is a container with a body, not a construct of its
  // own: the body maps to whatever it contains, inside a group that carries
  // the macro's name, so a table or a picture in it belongs to it as much
  // as a paragraph does and nothing has to invent a label for it. The four
  // callouts are also what the schema calls an admonition, and their text
  // says which.
  static constexpr std::string_view kCallouts[] = {"info", "note", "warning", "tip"};
  const bool callout = std::ranges::find(kCallouts, name) != std::end(kCallouts);
  const std::string group_ref =
      add_group(parent_ref, docv1::GROUP_LABEL_UNSPECIFIED, name)->self_ref();
  {
    AdmonitionScope scope(this, callout ? name : std::string());
    for (const Node& child : node.children) {
      if (element_is(child, "ac", "rich-text-body")) {
        fold_blocks(child, group_ref);
        continue;
      }
      if (element_is(child, "ac", "plain-text-body")) {
        const std::string text = raw_text(child);
        if (!blank(text)) {
          std::vector<InlineRun> runs{{text, InlineStyle{}}};
          flush_inline(&runs, group_ref);
        }
      }
    }
  }
  if (drop_empty_group(group_ref, parent_ref)) {
    // docling-core's EMPTY_GROUP rule rejects a group with no children.
    warn("macro '" + shown + "' had an empty body and was dropped");
  }
}

void StorageFold::emit_code_macro(const Node& node,
                                  const std::string& parent_ref) {
  std::string language;
  for (const Node& child : node.children) {
    if (element_is(child, "ac", "parameter") &&
        lowercase(attribute_or_empty(child, "ac", "name")) == "language") {
      language = std::string(trim(raw_text(child)));
    }
  }
  std::string text;
  if (const Node* body = find_child(node, "ac", "plain-text-body")) {
    text = raw_text(*body);
  }

  const std::string ref = "#/texts/" + std::to_string(document_->texts_size());
  docv1::CodeItem* code = document_->add_texts()->mutable_code();
  code->set_self_ref(ref);
  code->mutable_parent()->set_ref(parent_ref);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_text(text);
  code->set_orig(text);
  stamp_source(code->mutable_source());
  link_child(parent_ref, ref);
  ++emitted_;

  if (language.empty()) return;
  // The declared language maps by enum name, case-insensitively and with "-"
  // read as "_". Anything the schema has no value for keeps its raw string;
  // the mapping never guesses a neighbour.
  docv1::CodeLanguageLabel label = docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED;
  std::string wanted = "CODE_LANGUAGE_LABEL_" + uppercase(language);
  std::ranges::replace(wanted, '-', '_');
  if (docv1::CodeLanguageLabel_Parse(wanted, &label) &&
      label != docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED) {
    code->set_code_language(label);
  } else {
    code->set_code_language_raw(language);
  }
}

void StorageFold::emit_image(const Node& node, const std::string& parent_ref) {
  // An attachment is a pointer, never bytes: the handler has the page body
  // and nothing else, so it names the resource and stops there.
  std::string uri;
  if (const Node* attachment = find_child(node, "ri", "attachment")) {
    const std::string filename = attribute_or_empty(*attachment, "ri", "filename");
    if (!filename.empty()) uri = "confluence-attachment:" + filename;
  } else if (const Node* url = find_child(node, "ri", "url")) {
    uri = attribute_or_empty(*url, "ri", "value");
  }

  const std::string ref =
      "#/pictures/" + std::to_string(document_->pictures_size());
  docv1::PictureItem* picture = document_->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent_ref);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(picture->mutable_source());
  link_child(parent_ref, ref);
  ++emitted_;
  if (!uri.empty()) picture->mutable_image()->set_uri(uri);
  const std::string alt = attribute_or_empty(node, "ac", "alt");
  if (!alt.empty()) picture->mutable_meta()->set_alt_text(alt);
}

}  // namespace grparse::confluence
