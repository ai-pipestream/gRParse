#include "grparse/page_projection.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "grparse/document_assembly.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

enum class Arena { kText, kTable, kPicture, kGroup };

struct Slot {
  Arena arena;
  int index;
};

const docv1::TextItemBase* base_of(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading: return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

// The first page-numbered provenance entry, zero when the item names none.
template <typename Prov>
int first_page(const Prov& prov) {
  for (const auto& entry : prov) {
    if (entry.page_no() > 0) return entry.page_no();
  }
  return 0;
}

struct TextView {
  std::string self_ref;
  std::string text;
  int page;
  docv1::ContentLayer layer;
};

TextView text_view(const docv1::BaseTextItem& item) {
  if (item.item_case() == docv1::BaseTextItem::kCode) {
    const auto& code = item.code();
    return {code.self_ref(), code.text(), first_page(code.prov()), code.content_layer()};
  }
  const auto* base = base_of(item);
  if (base == nullptr) return {std::string(), std::string(), 0, docv1::CONTENT_LAYER_BODY};
  return {base->self_ref(), base->text(), first_page(base->prov()), base->content_layer()};
}

class Projector {
 public:
  Projector(const docv1::Document& document, parsev1::TextSource text_source)
      : document_(document), text_source_(text_source) {
    for (int i = 0; i < document.texts_size(); ++i) {
      slots_.emplace(text_view(document.texts(i)).self_ref, Slot{Arena::kText, i});
    }
    for (int i = 0; i < document.tables_size(); ++i) {
      slots_.emplace(document.tables(i).self_ref(), Slot{Arena::kTable, i});
    }
    for (int i = 0; i < document.pictures_size(); ++i) {
      slots_.emplace(document.pictures(i).self_ref(), Slot{Arena::kPicture, i});
    }
    for (int i = 0; i < document.groups_size(); ++i) {
      slots_.emplace(document.groups(i).self_ref(), Slot{Arena::kGroup, i});
    }
  }

  std::vector<parsev1::PageData> run() {
    if (!names_a_page()) return {};
    walk(document_.body().children(), /*body=*/true);
    walk(document_.furniture().children(), /*body=*/false);
    // Arena items no tree reaches still belong to their page; they follow
    // the reachable items so reading order stays the tree's.
    for (int i = 0; i < document_.texts_size(); ++i) {
      const auto view = text_view(document_.texts(i));
      if (!visited_.contains(view.self_ref)) {
        place_text(i, view.layer == docv1::CONTENT_LAYER_BODY);
      }
    }
    for (int i = 0; i < document_.tables_size(); ++i) {
      if (!visited_.contains(document_.tables(i).self_ref())) place_table(i, true);
    }
    for (int i = 0; i < document_.pictures_size(); ++i) {
      if (!visited_.contains(document_.pictures(i).self_ref())) place_picture(i, true);
    }
    int last = pages_.empty() ? 0 : pages_.rbegin()->first;
    for (const auto& [page_no, _] : document_.pages()) last = std::max(last, page_no);
    std::vector<parsev1::PageData> result;
    result.reserve(static_cast<size_t>(last));
    for (int page_no = 1; page_no <= last; ++page_no) {
      parsev1::PageData page;
      if (const auto it = pages_.find(page_no); it != pages_.end()) {
        page = std::move(it->second);
      }
      page.set_page_number(page_no);
      if (const auto meta = document_.pages().find(page_no); meta != document_.pages().end()) {
        *page.mutable_page_meta() = meta->second;
      }
      page.mutable_page_meta()->set_page_no(page_no);
      result.push_back(std::move(page));
    }
    return result;
  }

 private:
  // Whether anything in the document is placed on a page: an item's
  // provenance or the page map. Nothing placed, nothing projected; the
  // page an unplaced item inherits is only meaningful next to placed ones.
  bool names_a_page() const {
    if (!document_.pages().empty()) return true;
    for (const auto& item : document_.texts()) {
      if (text_view(item).page > 0) return true;
    }
    for (const auto& table : document_.tables()) {
      if (first_page(table.prov()) > 0) return true;
    }
    for (const auto& picture : document_.pictures()) {
      if (first_page(picture.prov()) > 0) return true;
    }
    return false;
  }

  void walk(const google::protobuf::RepeatedPtrField<docv1::RefItem>& children, bool body) {
    for (const auto& child : children) {
      const auto slot = slots_.find(child.ref());
      if (slot == slots_.end() || visited_.contains(child.ref())) continue;
      switch (slot->second.arena) {
        case Arena::kText: place_text(slot->second.index, body); break;
        case Arena::kTable: place_table(slot->second.index, body); break;
        case Arena::kPicture: place_picture(slot->second.index, body); break;
        case Arena::kGroup:
          visited_.insert(child.ref());
          walk(document_.groups(slot->second.index).children(), body);
          break;
      }
    }
  }

  // Resolves the page an item lands on and remembers it for the items that
  // follow without a page of their own.
  int settle(int page) {
    if (page > 0) current_page_ = page;
    return current_page_;
  }

  parsev1::PageData& page_for(int page) { return pages_[page]; }

  void place_text(int index, bool body) {
    const auto& item = document_.texts(index);
    const auto view = text_view(item);
    visited_.insert(view.self_ref);
    auto& page = page_for(settle(view.page));
    *page.add_texts() = item;
    auto* offset = page.add_text_offsets();
    offset->set_self_ref(view.self_ref);
    offset->set_utf_start(utf_cursor_);
    utf_cursor_ += utf8_codepoint_count(view.text);
    offset->set_utf_end(utf_cursor_);
    offset->set_source(text_source_);
    if (body) page.add_body_order()->set_ref(view.self_ref);
  }

  void place_table(int index, bool body) {
    const auto& table = document_.tables(index);
    visited_.insert(table.self_ref());
    auto& page = page_for(settle(first_page(table.prov())));
    *page.add_tables() = table;
    if (body) page.add_body_order()->set_ref(table.self_ref());
  }

  void place_picture(int index, bool body) {
    const auto& picture = document_.pictures(index);
    visited_.insert(picture.self_ref());
    auto& page = page_for(settle(first_page(picture.prov())));
    *page.add_pictures() = picture;
    if (body) page.add_body_order()->set_ref(picture.self_ref());
  }

  const docv1::Document& document_;
  const parsev1::TextSource text_source_;
  std::unordered_map<std::string, Slot> slots_;
  std::unordered_set<std::string> visited_;
  std::map<int, parsev1::PageData> pages_;
  int current_page_ = 1;
  uint64_t utf_cursor_ = 0;
};

}  // namespace

std::vector<parsev1::PageData> project_page_data(const docv1::Document& document,
                                                 parsev1::TextSource text_source) {
  return Projector(document, text_source).run();
}

}  // namespace grparse
