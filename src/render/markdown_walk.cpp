#include "markdown_walk.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "markdown_text.h"
#include "renderer_base.h"
#include "text_class.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {
namespace {

// The four-space indent one nesting level of a list adds.
constexpr std::size_t kListIndent = 4;

}  // namespace

const docv1::GroupItem* MarkdownWalk::group_at(const std::string& ref) const {
  if (ref == "#/body") return &document_.body();
  if (ref == "#/furniture") return &document_.furniture();
  const ArenaRef parsed = parse_ref(ref);
  if (parsed.kind == ArenaRef::kGroup && parsed.index < document_.groups_size()) {
    return &document_.groups(parsed.index);
  }
  return nullptr;
}

const docv1::BaseTextItem* MarkdownWalk::text_at(const std::string& ref) const {
  const ArenaRef parsed = parse_ref(ref);
  if (parsed.kind == ArenaRef::kText && parsed.index < document_.texts_size()) {
    return &document_.texts(parsed.index);
  }
  return nullptr;
}

const docv1::TableItem* MarkdownWalk::table_at(const std::string& ref) const {
  const ArenaRef parsed = parse_ref(ref);
  if (parsed.kind == ArenaRef::kTable && parsed.index < document_.tables_size()) {
    return &document_.tables(parsed.index);
  }
  return nullptr;
}

const docv1::PictureItem* MarkdownWalk::picture_at(const std::string& ref) const {
  const ArenaRef parsed = parse_ref(ref);
  if (parsed.kind == ArenaRef::kPicture &&
      parsed.index < document_.pictures_size()) {
    return &document_.pictures(parsed.index);
  }
  return nullptr;
}

std::vector<std::string> MarkdownWalk::children_of(const std::string& ref) const {
  std::vector<std::string> refs;
  const auto collect = [&refs](const auto& children) {
    for (const auto& child : children) refs.push_back(child.ref());
  };
  if (const auto* group = group_at(ref)) {
    collect(group->children());
    return refs;
  }
  if (const auto* text = text_at(ref)) {
    if (text->item_case() == docv1::BaseTextItem::kCode) {
      collect(text->code().children());
    } else if (const auto* base = text_base(*text)) {
      collect(base->children());
    }
    return refs;
  }
  if (const auto* table = table_at(ref)) {
    collect(table->children());
    return refs;
  }
  if (const auto* picture = picture_at(ref)) {
    collect(picture->children());
    return refs;
  }
  const ArenaRef parsed = parse_ref(ref);
  switch (parsed.kind) {
    case ArenaRef::kKeyValue:
      if (parsed.index < document_.key_value_items_size()) {
        collect(document_.key_value_items(parsed.index).children());
      }
      break;
    case ArenaRef::kForm:
      if (parsed.index < document_.form_items_size()) {
        collect(document_.form_items(parsed.index).children());
      }
      break;
    case ArenaRef::kFieldRegion:
      if (parsed.index < document_.field_regions_size()) {
        collect(document_.field_regions(parsed.index).children());
      }
      break;
    case ArenaRef::kFieldItem:
      if (parsed.index < document_.field_items_size()) {
        collect(document_.field_items(parsed.index).children());
      }
      break;
    default: break;
  }
  return refs;
}

docv1::ContentLayer MarkdownWalk::layer_of(const std::string& ref) const {
  if (const auto* group = group_at(ref)) return group->content_layer();
  if (const auto* text = text_at(ref)) {
    if (text->item_case() == docv1::BaseTextItem::kCode) {
      return text->code().content_layer();
    }
    if (const auto* base = text_base(*text)) return base->content_layer();
    return docv1::CONTENT_LAYER_UNSPECIFIED;
  }
  if (const auto* table = table_at(ref)) return table->content_layer();
  if (const auto* picture = picture_at(ref)) return picture->content_layer();
  const ArenaRef parsed = parse_ref(ref);
  switch (parsed.kind) {
    case ArenaRef::kKeyValue:
      if (parsed.index < document_.key_value_items_size()) {
        return document_.key_value_items(parsed.index).content_layer();
      }
      break;
    case ArenaRef::kForm:
      if (parsed.index < document_.form_items_size()) {
        return document_.form_items(parsed.index).content_layer();
      }
      break;
    case ArenaRef::kFieldRegion:
      if (parsed.index < document_.field_regions_size()) {
        return document_.field_regions(parsed.index).content_layer();
      }
      break;
    case ArenaRef::kFieldItem:
      if (parsed.index < document_.field_items_size()) {
        return document_.field_items(parsed.index).content_layer();
      }
      break;
    default: break;
  }
  return docv1::CONTENT_LAYER_UNSPECIFIED;
}

docv1::DocItemLabel MarkdownWalk::label_of(const std::string& ref) const {
  if (const auto* text = text_at(ref)) {
    switch (classify_text(*text)) {
      case TextClass::kTitle: return docv1::DOC_ITEM_LABEL_TITLE;
      case TextClass::kSectionHeader: return docv1::DOC_ITEM_LABEL_SECTION_HEADER;
      case TextClass::kListItem: return docv1::DOC_ITEM_LABEL_LIST_ITEM;
      case TextClass::kCode: return docv1::DOC_ITEM_LABEL_CODE;
      case TextClass::kFormula: return docv1::DOC_ITEM_LABEL_FORMULA;
      case TextClass::kFieldHeading: return docv1::DOC_ITEM_LABEL_FIELD_HEADING;
      case TextClass::kFieldValue: return docv1::DOC_ITEM_LABEL_FIELD_VALUE;
      case TextClass::kText: break;
    }
    const auto* base = text_base(*text);
    return effective_label(base != nullptr ? base->label()
                                           : docv1::DOC_ITEM_LABEL_TEXT,
                           docv1::DOC_ITEM_LABEL_TEXT);
  }
  if (const auto* table = table_at(ref)) {
    return effective_label(table->label(), docv1::DOC_ITEM_LABEL_TABLE);
  }
  if (const auto* picture = picture_at(ref)) {
    return effective_label(picture->label(), docv1::DOC_ITEM_LABEL_PICTURE);
  }
  const ArenaRef parsed = parse_ref(ref);
  switch (parsed.kind) {
    case ArenaRef::kKeyValue:
      return parsed.index < document_.key_value_items_size()
                 ? effective_label(document_.key_value_items(parsed.index).label(),
                                   docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION)
                 : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
    case ArenaRef::kForm:
      return parsed.index < document_.form_items_size()
                 ? effective_label(document_.form_items(parsed.index).label(),
                                   docv1::DOC_ITEM_LABEL_FORM)
                 : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
    case ArenaRef::kFieldRegion: return docv1::DOC_ITEM_LABEL_FIELD_REGION;
    case ArenaRef::kFieldItem: return docv1::DOC_ITEM_LABEL_FIELD_ITEM;
    default: return docv1::DOC_ITEM_LABEL_UNSPECIFIED;
  }
}

bool MarkdownWalk::is_document_item(const std::string& ref) const {
  return group_at(ref) == nullptr && parse_ref(ref).kind != ArenaRef::kUnknown;
}

void MarkdownWalk::collect_reference_sets() {
  const auto claim = [this](const auto& item) {
    for (const auto& ref : item.captions()) caption_refs_.insert(ref.ref());
    for (const auto& ref : item.footnotes()) footnote_refs_.insert(ref.ref());
  };
  for (const auto& item : document_.tables()) claim(item);
  for (const auto& item : document_.pictures()) claim(item);
  for (const auto& item : document_.key_value_items()) claim(item);
  for (const auto& item : document_.form_items()) claim(item);
  for (const auto& item : document_.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) claim(item.code());
  }

  std::set<std::string> seen;
  walk_for_exclusions("#/body", &seen);
}

void MarkdownWalk::walk_for_exclusions(const std::string& ref,
                                       std::set<std::string>* seen) {
  if (!seen->insert(ref).second) return;
  if (layer_of(ref) == docv1::CONTENT_LAYER_BODY ||
      layer_of(ref) == docv1::CONTENT_LAYER_UNSPECIFIED) {
    if (is_document_item(ref) && !exported_label(label_of(ref))) {
      excluded_refs_.insert(ref);
    }
  }
  for (const auto& child : children_of(ref)) walk_for_exclusions(child, seen);
}

void MarkdownWalk::collect_walk(const std::string& ref,
                                std::set<std::string>* seen,
                                std::vector<std::string>* out) const {
  if (!seen->insert(ref).second) return;
  const docv1::ContentLayer layer = layer_of(ref);
  if (layer == docv1::CONTENT_LAYER_BODY ||
      layer == docv1::CONTENT_LAYER_UNSPECIFIED) {
    out->push_back(ref);
  }
  const auto* picture = picture_at(ref);
  std::set<std::string> allowed;
  if (picture != nullptr) {
    for (const auto& caption : picture->captions()) allowed.insert(caption.ref());
  }
  for (const auto& child : children_of(ref)) {
    if (picture != nullptr && !allowed.contains(child)) continue;
    collect_walk(child, seen, out);
  }
}

std::vector<MarkdownWalk::Part> MarkdownWalk::get_parts(const std::string& root,
                                                       int list_level,
                                                       bool inline_scope) {
  std::vector<std::string> refs;
  std::set<std::string> seen;
  collect_walk(root, &seen, &refs);
  std::vector<Part> parts;
  for (const auto& ref : refs) {
    if (!consume(ref)) continue;
    Part part;
    part.text = serialize(ref, list_level, inline_scope, &part.first_span);
    if (!part.text.empty()) parts.push_back(std::move(part));
  }
  return parts;
}

std::string MarkdownWalk::parent_of(const std::string& ref) const {
  const auto parent = [](const auto& item) {
    return item.has_parent() ? item.parent().ref() : std::string();
  };
  if (const auto* group = group_at(ref)) return parent(*group);
  if (const auto* text = text_at(ref)) {
    if (text->item_case() == docv1::BaseTextItem::kCode) {
      return parent(text->code());
    }
    const auto* base = text_base(*text);
    return base != nullptr ? parent(*base) : std::string();
  }
  if (const auto* table = table_at(ref)) return parent(*table);
  if (const auto* picture = picture_at(ref)) return parent(*picture);
  const ArenaRef parsed = parse_ref(ref);
  switch (parsed.kind) {
    case ArenaRef::kKeyValue:
      if (parsed.index < document_.key_value_items_size()) {
        return parent(document_.key_value_items(parsed.index));
      }
      break;
    case ArenaRef::kForm:
      if (parsed.index < document_.form_items_size()) {
        return parent(document_.form_items(parsed.index));
      }
      break;
    case ArenaRef::kFieldRegion:
      if (parsed.index < document_.field_regions_size()) {
        return parent(document_.field_regions(parsed.index));
      }
      break;
    case ArenaRef::kFieldItem:
      if (parsed.index < document_.field_items_size()) {
        return parent(document_.field_items(parsed.index));
      }
      break;
    default: break;
  }
  return std::string();
}

std::vector<std::string> MarkdownWalk::part_texts(const std::vector<Part>& parts) {
  std::vector<std::string> texts;
  texts.reserve(parts.size());
  for (const auto& part : parts) texts.push_back(part.text);
  return texts;
}

bool MarkdownWalk::is_inline_group(const std::string& ref) const {
  const auto* group = group_at(ref);
  return group != nullptr && group->label() == docv1::GROUP_LABEL_INLINE;
}

bool MarkdownWalk::is_list_group(const std::string& ref) const {
  const auto* group = group_at(ref);
  return group != nullptr && (group->label() == docv1::GROUP_LABEL_LIST ||
                              group->label() == docv1::GROUP_LABEL_ORDERED_LIST);
}

std::string MarkdownWalk::first_part_span(const std::vector<Part>& parts) {
  for (const auto& part : parts) {
    if (!part.first_span.empty()) return part.first_span;
  }
  return std::string();
}

std::string MarkdownWalk::serialize_group_content(const std::string& ref,
                                                  const docv1::GroupItem& group,
                                                  int list_level,
                                                  bool inline_scope,
                                                  std::string* first_span) {
  if (group.label() == docv1::GROUP_LABEL_LIST ||
      group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
    return serialize_list(ref, list_level, inline_scope, first_span);
  }
  if (group.label() == docv1::GROUP_LABEL_INLINE) {
    const auto parts = get_parts(ref, list_level, true);
    if (first_span != nullptr) *first_span = first_part_span(parts);
    return join(part_texts(parts), " ");
  }
  // Every other group label is a transparent block container; its parts
  // start a fresh list scope.
  const auto parts = get_parts(ref, 0, false);
  if (first_span != nullptr) *first_span = first_part_span(parts);
  return join(part_texts(parts), "\n\n");
}

bool MarkdownWalk::span_sits_in_inline_group(const std::string& span) const {
  return !span.empty() && is_inline_group(parent_of(span));
}

std::string MarkdownWalk::serialize_list(const std::string& ref, int list_level,
                                         bool inline_scope,
                                         std::string* first_span) {
  std::vector<Part> parts = get_parts(ref, list_level + 1, inline_scope);
  if (first_span != nullptr) *first_span = first_part_span(parts);
  std::vector<Part> merged;
  for (auto& part : parts) {
    if (!merged.empty() && span_sits_in_inline_group(part.first_span)) {
      merged.back().text.append(part.text);
    } else {
      merged.push_back(std::move(part));
    }
  }
  const std::string indent(static_cast<std::size_t>(list_level) * kListIndent, ' ');
  std::string out;
  for (std::size_t i = 0; i < merged.size(); ++i) {
    if (i != 0) out.push_back('\n');
    // A part that already starts with a space is an evaluated sublist and
    // carries its own indent.
    if (!merged[i].text.empty() && merged[i].text.front() == ' ') {
      out.append(merged[i].text);
    } else {
      out.append(indent);
      out.append(merged[i].text);
    }
  }
  return out;
}

std::string MarkdownWalk::list_item_prefix(const std::string& ref,
                                           const docv1::BaseTextItem& text) {
  // An unset marker takes the model default.
  std::string marker = "-";
  if (text.item_case() == docv1::BaseTextItem::kListItem &&
      text.list_item().has_marker()) {
    marker = text.list_item().marker();
  }

  const bool has_alnum = std::ranges::any_of(marker, [](unsigned char c) {
    return std::isalnum(c) != 0;
  });
  const bool numeric_marker =
      marker.size() > 1 && marker.back() == '.' &&
      std::ranges::all_of(marker.substr(0, marker.size() - 1),
                          [](unsigned char c) { return std::isdigit(c) != 0; });
  const bool already_valid =
      marker == "-" || marker == "*" || marker == "+" || numeric_marker;

  std::vector<std::string> pieces;
  if (!already_valid) {
    std::string generated = "-";
    const auto* base = text_base(text);
    const std::string parent_ref =
        base != nullptr && base->has_parent() ? base->parent().ref() : std::string();
    if (marker.empty() && is_list_group(parent_ref) &&
        first_item_is_enumerated(parent_ref)) {
      int position = -1;
      const auto siblings = children_of(parent_ref);
      for (std::size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == ref) {
          position = static_cast<int>(i);
          break;
        }
      }
      generated = std::to_string(position + 1) + ".";
    }
    pieces.push_back(generated);
  }
  if (!marker.empty() && (has_alnum || already_valid)) pieces.push_back(marker);
  return join(pieces, " ");
}

bool MarkdownWalk::first_item_is_enumerated(const std::string& group_ref) const {
  for (const auto& child : children_of(group_ref)) {
    const auto* text = text_at(child);
    if (text == nullptr) return false;
    return text->item_case() == docv1::BaseTextItem::kListItem &&
           text->list_item().enumerated();
  }
  return false;
}

}  // namespace grparse::render
