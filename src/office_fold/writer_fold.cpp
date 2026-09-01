#include "grparse/office_fold/writer_fold.h"

#include <algorithm>
#include <cctype>
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

bool blank(const std::string& text) {
  return std::ranges::all_of(
      text, [](unsigned char c) { return std::isspace(c) != 0; });
}

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

}  // namespace

bool WriterFold::record_empty_paragraph(const officev1::Paragraph& paragraph,
                                        const std::string& text) {
  if (!blank(text)) return false;
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
  docv1::TableItem* item =
      arena_.add_table(docv1::CONTENT_LAYER_BODY, "#/body", nullptr);
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
  const int slot = take_anchor_slot(image.page_index(), image.anchor().y(),
                                    image.height_twips());
  if (slot < 0) {
    // No paragraph to take the place of: the picture sits where it
    // arrived, and once the stream is in it is judged against the body
    // around it (anchor_trailing_pictures).
    unslotted_pictures_.insert(picture_ref);
    return;
  }
  arena_.move_child_after("#/body", picture_ref,
                          paragraph_slots_[slot].after_ref);
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
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, docv1::CONTENT_LAYER_BODY, parent,
      &picture_ref);
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
  const docv1::DocItemLabel label = block.footer()
      ? docv1::DOC_ITEM_LABEL_PAGE_FOOTER
      : docv1::DOC_ITEM_LABEL_PAGE_HEADER;
  for (const officev1::Paragraph& paragraph : block.paragraphs()) {
    TextHandle handle = arena_.add_text(TextKind::kText, label,
                                        docv1::CONTENT_LAYER_FURNITURE,
                                        "#/furniture");
    fill_from_runs(paragraph.runs(), handle);
    if (!paragraph.style().empty()) {
      handle.base->set_style_name(paragraph.style());
    }
    (*handle.base->mutable_meta()->mutable_custom_fields())["page_style"] =
        str_value(block.page_style());
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
  docv1::GroupItem* group =
      arena_.add_group("#/body", docv1::GROUP_LABEL_UNSPECIFIED, frame.name(),
                       docv1::CONTENT_LAYER_BODY);
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                      docv1::CONTENT_LAYER_BODY, group->self_ref());
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
}

void WriterFold::on_shape(const officev1::Shape& shape) {
  std::string parent = "#/body";
  if (auto container = writer_groups_.find(shape.group_path());
      container != writer_groups_.end()) {
    parent = container->second;
  }

  if (shape.is_group()) {
    // The group's own shape type is always the office core's group shape,
    // which GROUP_LABEL_PICTURE_AREA already says.
    docv1::GroupItem* group =
        arena_.add_group(parent, docv1::GROUP_LABEL_PICTURE_AREA, shape.name(),
                         docv1::CONTENT_LAYER_BODY);
    writer_groups_[child_group_path(shape.group_path(), shape.z_order())] =
        group->self_ref();
    return;
  }

  docv1::GroupItem* group =
      arena_.add_group(parent, docv1::GROUP_LABEL_UNSPECIFIED, shape.name(),
                       docv1::CONTENT_LAYER_BODY);
  TextHandle handle =
      arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                      docv1::CONTENT_LAYER_BODY, group->self_ref());
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
}

void WriterFold::anchor_trailing_pictures() {
  const docv1::Document& document = arena_.document();
  if (arena_.document_type() != "text" || unslotted_pictures_.empty()) return;
  const std::map<int, double> heights = document_page_heights(document);
  std::vector<std::string> trailing;
  std::optional<ItemPlacement> last;
  for (const docv1::RefItem& child : document.body().children()) {
    const std::optional<ItemPlacement> placement =
        item_placement(document, child.ref(), heights);
    if (!placement.has_value()) continue;
    if (unslotted_pictures_.contains(child.ref()) && last.has_value() &&
        std::pair(placement->page, placement->box.top) <
            std::pair(last->page, last->box.top)) {
      trailing.push_back(child.ref());
      continue;
    }
    last = placement;
  }
  if (trailing.empty()) return;
  const PictureAnchorReport report =
      anchor_pictures_by_provenance(&arena_.document(), trailing);
  data_log("office " + document.name() + ": " + std::to_string(report.anchored)
           + " trailing picture(s) placed by provenance");
}

}  // namespace grparse::office_fold
