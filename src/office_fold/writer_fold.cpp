#include "grparse/office_fold/writer_fold.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "grparse/data_totals.h"
#include "grparse/document_geometry.h"
#include "grparse/document_reading_order.h"
#include "grparse/office_fold/run_text.h"
#include "grparse/office_fold/shape_meta.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// The path a child of this group carries: the parent's path with the
// group's own paint order appended.
std::string child_group_path(const std::string& group_path, int z_order) {
  return group_path.empty()
             ? std::to_string(z_order)
             : group_path + "/" + std::to_string(z_order);
}

// The chain a text frame or shape belongs to, so reading order across a
// chain resolves by frame name.
void set_chain(const std::string& next, const std::string& prev,
               docv1::ShapeMeta* out) {
  if (!next.empty()) out->set_chain_next(next);
  if (!prev.empty()) out->set_chain_prev(prev);
}

// The identity of an anchored object's event: every per-page copy of a
// header's object carries the same one.
std::string object_key(const char* kind, const std::string& name,
                       const std::string& content, long long width,
                       long long height, int page_index,
                       const officev1::TwipsPoint* anchor) {
  std::string key = std::string(kind) + "\x1f" + name + "\x1f"
                    + std::to_string(width) + "x" + std::to_string(height)
                    + "\x1f" + std::to_string(page_index);
  if (anchor != nullptr) {
    key += "@" + std::to_string(anchor->x()) + "," + std::to_string(anchor->y());
  }
  return key + "\x1f" + content;
}

}  // namespace

bool WriterFold::repeats_placed_object(const std::string& key,
                                       const std::string& ref) {
  auto [found, inserted] = placed_objects_.try_emplace(key, ref);
  if (inserted) return false;
  if (repeated_objects_.insert(found->second).second) {
    arena_.move_to_furniture(found->second);
  }
  return true;
}

bool WriterFold::record_empty_paragraph(const officev1::Paragraph& paragraph,
                                        const std::string& text) {
  if (!blank_text(text)) return false;
  ParagraphSlot slot;
  slot.page_index = paragraph.page_index();
  slot.caret_y = paragraph.start().y();
  const auto& children = arena_.document().body().children();
  if (!children.empty()) slot.after_ref = children[children.size() - 1].ref();
  paragraph_slots_.push_back(std::move(slot));
  return true;
}

TextHandle WriterFold::add_paragraph_item(
    const officev1::Paragraph& paragraph) {
  if (paragraph.style() == "Title") {
    return arena_.add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
                           docv1::CONTENT_LAYER_BODY, "#/body");
  }
  if (paragraph.outline_level() >= 1) {
    TextHandle handle = arena_.add_text(TextKind::kSectionHeader,
                                        docv1::DOC_ITEM_LABEL_SECTION_HEADER,
                                        docv1::CONTENT_LAYER_BODY, "#/body");
    handle.item->mutable_section_header()->set_level(paragraph.outline_level());
    return handle;
  }
  if (paragraph.list_level() >= 0) {
    return arena_.add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                           docv1::CONTENT_LAYER_BODY, "#/body");
  }
  return arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                         docv1::CONTENT_LAYER_BODY, "#/body");
}

void WriterFold::on_paragraph(const officev1::Paragraph& paragraph) {
  const std::string text = concat_runs(paragraph.runs());
  if (record_empty_paragraph(paragraph, text)) return;
  const long long length = runs_length(paragraph.runs());
  // Provenance charspans are 0-indexed within the item's own text; the
  // document-absolute paragraph offset stays on the office wire only.
  TextHandle handle = add_paragraph_item(paragraph);
  paragraph_starts_[{paragraph.page_index(), paragraph.start().x(),
                     paragraph.start().y()}] = handle.ref;
  fill_from_runs(paragraph.runs(), handle);
  if (!paragraph.style().empty()) {
    handle.base->set_style_name(paragraph.style());
  }
  // The paragraph's extent in the document-absolute character space, which
  // is where comments, tracked changes, and bookmarks anchor.
  if (paragraph.char_offset() >= 0) {
    anchors_.add_body_span(paragraph.char_offset(),
                           paragraph.char_offset() + length, handle.ref);
  }
  if (!paragraph.line_rects().empty()) {
    arena_.add_line_prov(handle.base->mutable_prov(), paragraph.line_rects(), 0,
                         length);
  } else {
    arena_.add_caret_prov(handle.base->mutable_prov(), paragraph.page_index(),
                          paragraph.start(), paragraph.end(), 0, length);
  }
}

void WriterFold::on_table(const officev1::TableData& table) {
  std::string table_ref;
  docv1::TableItem* item =
      arena_.add_table(docv1::CONTENT_LAYER_BODY, "#/body", &table_ref);
  // A table streaming after the text frames is one a frame holds (a Word
  // floating table): it is placed by where it sits, like the frames.
  if (frames_seen_) floating_items_.insert(table_ref);
  arena_.fold_table(table, item);
  if (!table.line_rects().empty()) {
    arena_.add_line_prov(item->mutable_prov(), table.line_rects(), 0, 0);
  } else {
    arena_.add_caret_prov(item->mutable_prov(), table.page_index(),
                          table.start(), table.end(), 0, 0);
  }
}

int WriterFold::take_anchor_slot(int page_index, long long anchor_y,
                                 long long height) {
  // The anchor caret sits on the picture's line, at or below its top edge;
  // a line's worth of slack (600 twips) covers the line height itself.
  constexpr long long kLineSlack = 600;
  int best = -1;
  for (int i = 0; i < static_cast<int>(paragraph_slots_.size()); i++) {
    const ParagraphSlot& slot = paragraph_slots_[i];
    if (slot.page_index != page_index) continue;
    if (slot.caret_y < anchor_y || slot.caret_y > anchor_y + height + kLineSlack) {
      continue;
    }
    if (best < 0 || slot.caret_y < paragraph_slots_[best].caret_y) best = i;
  }
  return best;
}

void WriterFold::slot_inline_picture(const officev1::EmbeddedImage& image,
                                     const std::string& picture_ref) {
  // A picture anchored to a paragraph that has text of its own (a logo
  // floating beside a letterhead line) carries that paragraph's start
  // caret as its anchor: it reads right after its paragraph, not in some
  // empty paragraph below it.
  if (auto anchor = paragraph_starts_.find(
          {image.page_index(), image.anchor().x(), image.anchor().y()});
      anchor != paragraph_starts_.end()) {
    arena_.move_child_after("#/body", picture_ref, anchor->second);
    return;
  }
  const int slot = take_anchor_slot(image.page_index(), image.anchor().y(),
                                    image.height_twips());
  if (slot < 0) {
    // No paragraph to take the place of: the picture sits where it
    // arrived, and once the stream is in it is judged against the body
    // around it (anchor_trailing_pictures).
    floating_items_.insert(picture_ref);
    return;
  }
  const std::string after = paragraph_slots_[slot].after_ref;
  arena_.move_child_after("#/body", picture_ref, after);
  // Blank paragraphs in a row all wait after the same item; a later one
  // now waits after this picture, so pictures taking them keep page order.
  for (size_t i = static_cast<size_t>(slot) + 1; i < paragraph_slots_.size(); i++) {
    if (paragraph_slots_[i].after_ref == after) paragraph_slots_[i].after_ref = picture_ref;
  }
  paragraph_slots_.erase(paragraph_slots_.begin() + slot);
}

void WriterFold::on_embedded_image(const officev1::EmbeddedImage& image) {
  std::string parent = "#/body";
  // A slide picture belongs under its slide; its geometry is already
  // page-local, unlike a text document's document-absolute anchors.
  bool page_local = false;
  if (arena_.document_type() == "presentation") {
    page_local = true;
    parent = shapes_.slide_group_ref(image.page_index());
  } else if (auto container = writer_groups_.find(image.group_path());
             container != writer_groups_.end()) {
    parent = container->second;
  }
  std::string picture_ref;
  docv1::PictureItem* picture = nullptr;
  // A slide's graphic shape has already placed this picture; the image
  // fills it in rather than adding it a second time.
  if (page_local && image.has_anchor()) {
    picture_ref = shapes_.take_slide_picture(
        image.page_index(), image.anchor().x(), image.anchor().y());
    if (!picture_ref.empty()) {
      picture = arena_.picture_by_ref(picture_ref);
      if (picture == nullptr) picture_ref.clear();
    }
  }
  if (picture != nullptr) {
    if (!image.name().empty()) picture->mutable_shape()->set_name(image.name());
    if (!image.title().empty() || !image.description().empty()) {
      set_alt_text(image.title(), image.description(), picture);
    }
    if (!image.data().empty()) {
      docv1::ImageRef* ref = picture->mutable_image();
      ref->set_mimetype(image.mime_type());
      ref->mutable_size()->set_width(static_cast<double>(image.width_twips()));
      ref->mutable_size()->set_height(
          static_cast<double>(image.height_twips()));
      ref->set_uri(data_uri(image.mime_type(), image.data()));
    }
    return;
  }
  // A header or footer logo repeats on the pages of its page style; it is
  // furniture, kept once, never a body picture at the header's caret.
  const bool header_object = !page_local && image.in_header_footer();
  if (header_object) {
    if (!header_objects_
             .insert(object_key("image", image.name(),
                                std::to_string(std::hash<std::string>{}(image.data())),
                                image.width_twips(), image.height_twips(), -1, nullptr))
             .second) {
      return;
    }
    if (parent == "#/body") parent = "#/furniture";
  } else if (!page_local) {
    // The image bytes are part of the identity: two different pictures can
    // share a name and a size, never the bytes too.
    const std::string key = object_key(
        "image", image.name(),
        std::to_string(std::hash<std::string>{}(image.data())),
        image.width_twips(), image.height_twips(), image.page_index(),
        image.has_anchor() ? &image.anchor() : nullptr);
    const std::string next_ref =
        "#/pictures/" + std::to_string(arena_.document().pictures_size());
    if (repeats_placed_object(key, next_ref)) return;
  }
  const docv1::GroupItem* container = arena_.group_by_ref(parent);
  const bool furniture =
      header_object || container->content_layer() == docv1::CONTENT_LAYER_FURNITURE;
  picture = arena_.add_picture(docv1::DOC_ITEM_LABEL_PICTURE,
                               furniture ? docv1::CONTENT_LAYER_FURNITURE
                                         : docv1::CONTENT_LAYER_BODY,
                               parent, &picture_ref);
  if (!image.name().empty()) picture->mutable_shape()->set_name(image.name());
  set_alt_text(image.title(), image.description(), picture);
  // A Writer picture anchored in an otherwise empty paragraph takes that
  // paragraph's place in the body instead of trailing it.
  if (parent == "#/body" && image.has_anchor() && !page_local) {
    slot_inline_picture(image, picture_ref);
  }
  if (!image.data().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(image.mime_type());
    ref->mutable_size()->set_width(static_cast<double>(image.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(image.height_twips()));
    ref->set_uri(data_uri(image.mime_type(), image.data()));
  }
  if (image.has_anchor()) {
    arena_.add_prov(picture->mutable_prov(), image.page_index(), page_local,
                    static_cast<double>(image.anchor().x()),
                    static_cast<double>(image.anchor().y()),
                    static_cast<double>(image.anchor().x() + image.width_twips()),
                    static_cast<double>(image.anchor().y() + image.height_twips()),
                    0, 0);
  } else if (!image.line_rects().empty()) {
    arena_.add_line_prov(picture->mutable_prov(), image.line_rects(), 0, 0);
  }
}

void WriterFold::on_footnote(const officev1::Footnote& footnote) {
  // A note with no text (its mark kept, its body emptied) has nothing to
  // read.
  if (blank_text(concat_runs(footnote.runs()))) return;
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_FOOTNOTE,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  const std::string text = concat_runs(footnote.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  docv1::FootnoteMeta* note = handle.base->mutable_footnote_meta();
  if (!footnote.label().empty()) note->set_label(footnote.label());
  note->set_endnote(footnote.endnote());
  set_uniform_formatting(footnote.runs(), handle.base);
  add_spans(footnote.runs(), handle, 0);
  apply_run_hyperlinks(footnote.runs(), handle.base);
  arena_.add_caret_prov(handle.base->mutable_prov(), footnote.page_index(),
                        footnote.anchor(), footnote.anchor(), 0,
                        runs_length(footnote.runs()));
}

void WriterFold::on_header_footer(const officev1::HeaderFooter& block) {
  // Which pages a header sits on is known only once every page is in.
  header_footer_blocks_.push_back(block);
}

void WriterFold::add_header_footer(const officev1::HeaderFooter& block,
                                   const std::vector<int>& pages) {
  const docv1::DocItemLabel label = block.footer()
      ? docv1::DOC_ITEM_LABEL_PAGE_FOOTER
      : docv1::DOC_ITEM_LABEL_PAGE_HEADER;
  for (const officev1::Paragraph& paragraph : block.paragraphs()) {
    // A blank header line is spacing, not furniture text.
    if (blank_text(concat_runs(paragraph.runs()))) continue;
    TextHandle handle = arena_.add_text(TextKind::kText, label,
                                        docv1::CONTENT_LAYER_FURNITURE,
                                        "#/furniture");
    fill_from_runs(paragraph.runs(), handle);
    if (!paragraph.style().empty()) {
      handle.base->set_style_name(paragraph.style());
    }
    (*handle.base->mutable_meta()->mutable_custom_fields())["page_style"] =
        str_value(block.page_style());
    // The header repeats on each page, always at the same place; where on
    // the page is not on the wire, so the entries name the page alone.
    const long long length = runs_length(paragraph.runs());
    for (int page_no : pages) {
      docv1::ProvenanceItem* prov = handle.base->add_prov();
      prov->set_page_no(page_no);
      prov->mutable_charspan()->set_start(0);
      prov->mutable_charspan()->set_end(
          static_cast<int32_t>(std::min<long long>(length, INT32_MAX)));
    }
  }
}

void WriterFold::on_document_index(const officev1::DocumentIndex& index) {
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  const std::string text = concat_runs(index.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  docv1::IndexMeta* attribution = handle.base->mutable_index_meta();
  if (!index.type().empty()) attribution->set_service(index.type());
  if (!index.title().empty()) attribution->set_title(index.title());
  set_uniform_formatting(index.runs(), handle.base);
  add_spans(index.runs(), handle, 0);
  apply_run_hyperlinks(index.runs(), handle.base);
  arena_.add_caret_prov(handle.base->mutable_prov(), index.page_index(),
                        index.anchor(), index.anchor(), 0,
                        runs_length(index.runs()));
}

void WriterFold::on_text_frame(const officev1::TextFrame& frame) {
  frames_seen_ = true;
  // A frame with no text (a border, a picture holder whose picture streams
  // on its own) is no text item.
  const std::string text = concat_runs(frame.runs());
  if (blank_text(text)) return;
  const bool header_object = frame.in_header_footer();
  if (header_object) {
    if (!header_objects_
             .insert(object_key("frame", frame.name(), text, frame.width_twips(),
                                frame.height_twips(), -1, nullptr))
             .second) {
      return;
    }
  } else {
    const std::string key = object_key(
        "frame", frame.name(), text, frame.width_twips(), frame.height_twips(),
        frame.page_index(), frame.has_anchor() ? &frame.anchor() : nullptr);
    const std::string group_ref =
        "#/groups/" + std::to_string(arena_.document().groups_size());
    if (repeats_placed_object(key, group_ref)) return;
  }
  const docv1::ContentLayer layer =
      header_object ? docv1::CONTENT_LAYER_FURNITURE : docv1::CONTENT_LAYER_BODY;
  docv1::GroupItem* group =
      arena_.add_group(header_object ? "#/furniture" : "#/body",
                       docv1::GROUP_LABEL_UNSPECIFIED, frame.name(), layer);
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, layer,
                      group->self_ref());
  // The frame's identity, chain included, belongs on the item that carries
  // its text.
  docv1::ShapeMeta* shape_meta = handle.base->mutable_shape();
  set_shape_meta(std::string(), frame.name(), shape_meta);
  set_chain(frame.chain_next(), frame.chain_prev(), shape_meta);
  fill_from_runs(frame.runs(), handle);
  if (frame.has_anchor()) {
    arena_.add_prov(handle.base->mutable_prov(), frame.page_index(), false,
                    static_cast<double>(frame.anchor().x()),
                    static_cast<double>(frame.anchor().y()),
                    static_cast<double>(frame.anchor().x() + frame.width_twips()),
                    static_cast<double>(frame.anchor().y() + frame.height_twips()),
                    0, runs_length(frame.runs()));
  }
  // A frame streams after the body text; like an unslotted picture it is
  // judged against the finished body and placed by where it sits.
  if (!header_object) floating_items_.insert(group->self_ref());
}

void WriterFold::on_shape(const officev1::Shape& shape) {
  std::string parent = "#/body";
  if (auto container = writer_groups_.find(shape.group_path());
      container != writer_groups_.end()) {
    parent = container->second;
  }

  // Only a top-level shape has a caret anchor, so only it can say it sits
  // in a header; its children follow it there through their group.
  const bool header_object = parent == "#/body" && shape.in_header_footer();
  if (header_object) parent = "#/furniture";
  const docv1::GroupItem* container = arena_.group_by_ref(parent);
  const docv1::ContentLayer layer =
      parent == "#/furniture" ||
              container->content_layer() == docv1::CONTENT_LAYER_FURNITURE
          ? docv1::CONTENT_LAYER_FURNITURE
          : docv1::CONTENT_LAYER_BODY;

  if (shape.is_group()) {
    // The group's own shape type is always the office core's group shape,
    // which GROUP_LABEL_PICTURE_AREA already says.
    docv1::GroupItem* group =
        arena_.add_group(parent, docv1::GROUP_LABEL_PICTURE_AREA, shape.name(), layer);
    writer_groups_[child_group_path(shape.group_path(), shape.z_order())] =
        group->self_ref();
    if (parent == "#/body") floating_items_.insert(group->self_ref());
    return;
  }

  // A drawn shape with no text (a line, a box, an arrow) is no text item.
  const std::string text = concat_runs(shape.runs());
  if (blank_text(text)) return;
  if (header_object) {
    if (!header_objects_
             .insert(object_key("shape", shape.name(), text, shape.width_twips(),
                                shape.height_twips(), -1, nullptr))
             .second) {
      return;
    }
  } else {
    const std::string key = object_key(
        "shape", shape.name() + "\x1f" + shape.group_path(), text,
        shape.width_twips(), shape.height_twips(), shape.page_index(),
        shape.has_anchor() ? &shape.anchor()
                           : (shape.has_position() ? &shape.position() : nullptr));
    const std::string group_ref =
        "#/groups/" + std::to_string(arena_.document().groups_size());
    if (repeats_placed_object(key, group_ref)) return;
  }
  docv1::GroupItem* group =
      arena_.add_group(parent, docv1::GROUP_LABEL_UNSPECIFIED, shape.name(), layer);
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, layer,
                      group->self_ref());
  docv1::ShapeMeta* shape_meta = handle.base->mutable_shape();
  set_shape_meta(shape.shape_type(), shape.name(), shape_meta);
  shape_meta->set_z_order(shape.z_order());
  set_chain(shape.chain_next(), shape.chain_prev(), shape_meta);
  fill_from_runs(shape.runs(), handle);
  if (shape.has_anchor()) {
    arena_.add_prov(handle.base->mutable_prov(), shape.page_index(), false,
                    static_cast<double>(shape.anchor().x()),
                    static_cast<double>(shape.anchor().y()),
                    static_cast<double>(shape.anchor().x() + shape.width_twips()),
                    static_cast<double>(shape.anchor().y() + shape.height_twips()),
                    0, runs_length(shape.runs()));
  } else if (shape.has_position()) {
    // Group children carry a model position instead of a caret anchor; the
    // page resolves from the position, which shares the document-absolute
    // space of the page rectangles.
    const double l = static_cast<double>(shape.position().x());
    const double t = static_cast<double>(shape.position().y());
    const double r = l + static_cast<double>(shape.width_twips());
    const double b = t + static_cast<double>(shape.height_twips());
    arena_.add_prov(handle.base->mutable_prov(),
                    arena_.page_for_point((l + r) / 2, (t + b) / 2), false, l,
                    t, r, b, 0, runs_length(shape.runs()));
  }
  // Shapes stream after the body text, like frames.
  if (parent == "#/body") floating_items_.insert(group->self_ref());
}

void WriterFold::place_headers_footers() {
  if (header_footer_blocks_.empty()) return;
  const docv1::Document& document = arena_.document();
  // Pages by the page style the layout put them in; a page the stream named
  // no style for (no page image for it) may be in any style.
  std::map<std::string, std::vector<int>> pages_by_style;
  std::vector<int> unstyled;
  for (const auto& [page_no, page] : document.pages()) {
    if (page.style_name().empty()) {
      unstyled.push_back(page_no);
    } else {
      pages_by_style[page.style_name()].push_back(page_no);
    }
  }
  int unused = 0;
  for (const officev1::HeaderFooter& block : header_footer_blocks_) {
    std::vector<int> pages = unstyled;
    if (auto found = pages_by_style.find(block.page_style());
        found != pages_by_style.end()) {
      pages.insert(pages.end(), found->second.begin(), found->second.end());
    }
    // A header of a page style no page is laid out in never shows.
    if (pages.empty() && !document.pages().empty()) {
      unused++;
      continue;
    }
    std::ranges::sort(pages);
    add_header_footer(block, pages);
  }
  header_footer_blocks_.clear();
  if (unused > 0) {
    data_log("office " + document.name() + ": " + std::to_string(unused)
             + " header/footer block(s) of page styles no page uses left out");
  }
}

void WriterFold::anchor_trailing_pictures() {
  const docv1::Document& document = arena_.document();
  if (arena_.document_type() != "text" || floating_items_.empty()) return;
  const std::map<int, double> heights = document_page_heights(document);
  std::vector<std::string> trailing;
  std::optional<ItemPlacement> last;
  for (const docv1::RefItem& child : document.body().children()) {
    const std::optional<ItemPlacement> placement =
        item_placement(document, child.ref(), heights);
    if (!placement.has_value()) continue;
    if (floating_items_.contains(child.ref()) && last.has_value() &&
        std::pair(placement->page, placement->box.top) <
            std::pair(last->page, last->box.top)) {
      trailing.push_back(child.ref());
      continue;
    }
    last = placement;
  }
  if (!trailing.empty()) {
    const PictureAnchorReport report =
        anchor_pictures_by_provenance(&arena_.document(), trailing);
    data_log("office " + document.name() + ": "
             + std::to_string(report.anchored)
             + " trailing picture(s), frame(s) and shape(s) placed by provenance");
  }
  place_page_only_pictures();
}

void WriterFold::place_page_only_pictures() {
  docv1::Document& document = arena_.document();
  const std::map<int, double> heights = document_page_heights(document);
  // A floating picture whose caret box was dropped (its anchor caret stood
  // on another page) still knows its page: it goes before the first body
  // item of that page, the place a picture pushed to the top of a page has.
  std::vector<std::pair<std::string, int>> pictures;
  for (const docv1::RefItem& child : document.body().children()) {
    if (!floating_items_.contains(child.ref())) continue;
    if (item_placement(document, child.ref(), heights).has_value()) continue;
    const docv1::PictureItem* picture = arena_.picture_by_ref(child.ref());
    if (picture == nullptr) continue;
    const int page = first_page_of(picture->prov());
    if (page >= 1) pictures.emplace_back(child.ref(), page);
  }
  // Pictures already moved count by their page too, so a run of page-only
  // pictures on consecutive pages keeps its page order.
  std::map<std::string, int> page_only(pictures.begin(), pictures.end());
  for (const auto& [ref, page] : pictures) {
    std::string after;
    bool placed = false;
    for (const docv1::RefItem& child : document.body().children()) {
      if (child.ref() == ref) continue;
      std::optional<int> child_page;
      if (const auto known = page_only.find(child.ref()); known != page_only.end()) {
        child_page = known->second;
      } else if (const std::optional<ItemPlacement> placement =
                     item_placement(document, child.ref(), heights)) {
        child_page = placement->page;
      } else if (const docv1::PictureItem* other = arena_.picture_by_ref(child.ref());
                 other != nullptr && first_page_of(other->prov()) >= 1) {
        // A picture placed by its anchor paragraph, page known but no box.
        child_page = first_page_of(other->prov());
      }
      if (child_page.has_value() && *child_page > page) {
        placed = true;
        break;
      }
      if (child_page.has_value() && *child_page == page &&
          !page_only.contains(child.ref()) && arena_.picture_by_ref(child.ref()) == nullptr) {
        placed = true;
        break;
      }
      after = child.ref();
    }
    if (placed) arena_.move_child_after("#/body", ref, after);
  }
}

}  // namespace grparse::office_fold
