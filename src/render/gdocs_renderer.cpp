// The Docs API renderer behind render_gdocs_json; semantics documented on the
// declaration in include/grparse/document_render.h.
//
// The walk is the one the HTML export uses (src/render/html_renderer.cpp): a
// pre-order pass over the body reference tree where a table or a picture
// claims its caption items so a caption linked into the tree twice renders
// once, and where every content layer but the body layer stays out. What
// differs is the target vocabulary. Instead of markup, each body item becomes
// one structural element of the Docs API "document" resource, streamed
// straight into the canonical JSON writer the other JSON export uses (same
// two-space layout, same ASCII-only escaping), so a downstream integration can
// hand the payload to documents.create and follow it with one batchUpdate.
//
// The emitted object is always
//
//   {
//     "title": ...,
//     "body": {"content": [<structural element>, ...]},
//     "lists": {"kix.list0": {"listProperties": {"nestingLevels": [...]}}},
//     "inlineImagePlaceholders": [...]
//   }
//
// with all four members present in that order, so the integration can index
// the payload instead of probing for optional members. Key order is fixed by
// the emit order everywhere, and nothing here iterates an unordered
// container, so two renders of one document are byte-identical.
//
// THE IMAGE CONTRACT. A create body cannot carry image bytes: the API takes an
// inline image only by URI, from a later insert-inline-image request. Every
// picture therefore renders as one NORMAL_TEXT paragraph holding its claimed
// caption text (empty when it has none), and is recorded under
// "inlineImagePlaceholders" as
//
//   {"selfRef", "mimeType", "size": {"width", "height"}, "contentIndex"}
//
// where contentIndex is that paragraph's position in body.content. The
// integration layer owns the second leg: upload the bytes it holds for
// selfRef, then replace the placeholder paragraph at contentIndex with an
// inline image of the uploaded URI. The picture's own bytes never appear in
// this export, whatever the document carries.
#include "gdocs_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grparse/document_render.h"
#include "canonical_json_writer.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {
namespace {

// The API's heading ranks, the range a section header level clamps into.
constexpr int kFirstHeading = 1;
constexpr int kLastHeading = 6;

}  // namespace

std::string gdocs_heading_style(int level) {
  return "HEADING_" + std::to_string(std::clamp(level, kFirstHeading, kLastHeading));
}

std::string gdocs_list_id(std::size_t index) {
  return "kix.list" + std::to_string(index);
}

std::string gdocs_run_content(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 1);
  for (const char c : text) {
    if (c == '\r') continue;
    out.push_back(c == '\n' ? '\v' : c);
  }
  out.push_back('\n');
  return out;
}

}  // namespace grparse::render

namespace grparse {
namespace {

using namespace grparse::render;

constexpr std::string_view kNormalText = "NORMAL_TEXT";
constexpr std::string_view kTitleStyle = "TITLE";
// The monospace face code and formula source ride, spelled the way a
// weightedFontFamily names it, at the regular weight that field wants.
constexpr std::string_view kCodeFontFamily = "Courier New";
constexpr std::int64_t kRegularWeight = 400;
// The glyphs a list level declares: numbering for an enumerated level, the
// filled circle the API's own default bullet uses for the rest.
constexpr std::string_view kDecimalGlyph = "DECIMAL";
constexpr std::string_view kBulletGlyph = "●";
// The deepest nesting level the API honours; a list group below it folds into
// the last level rather than inventing one.
constexpr int kMaxNestingLevel = 8;

// The list membership of a bulleted paragraph: which of the opened lists it
// belongs to, and how deep inside that list it sits.
struct Bullet {
  std::size_t list = 0;
  int nesting_level = 0;
};

// One paragraph structural element, before it is written out. The four
// flavours the walk emits are named rather than built field by field, so a
// call site never has to spell the fields it does not set.
struct Paragraph {
  std::string text;
  std::string named_style;
  bool monospace = false;
  std::optional<Bullet> bullet;

  // Text under an explicit named style: the title and the headings.
  static Paragraph styled(std::string text, std::string named_style) {
    return {std::move(text), std::move(named_style), false, std::nullopt};
  }
  // Ordinary prose, and everything the API has no named style for.
  static Paragraph prose(std::string text) {
    return styled(std::move(text), std::string(kNormalText));
  }
  // Source text that has to keep its columns: code and formulas.
  static Paragraph source(std::string text) {
    return {std::move(text), std::string(kNormalText), true, std::nullopt};
  }
  // One item of a list, at the nesting level the bullet names.
  static Paragraph bulleted(std::string text, Bullet bullet) {
    return {std::move(text), std::string(kNormalText), false, bullet};
  }
};

// The caption texts a float claimed, as one paragraph's worth of text.
std::string join_captions(const std::vector<std::string>& captions) {
  std::string text;
  for (const auto& caption : captions) {
    if (!text.empty()) text.push_back(' ');
    text.append(caption);
  }
  return text;
}

// Marks every grid position a cell spanning from (row, col) reaches, clipped
// to the grid it is marked on.
void mark_covered(std::vector<std::vector<bool>>* covered, std::size_t row,
                  std::size_t col, int row_span, int col_span) {
  const std::size_t row_end =
      std::min(covered->size(), row + static_cast<std::size_t>(row_span));
  for (std::size_t r = row; r < row_end; ++r) {
    auto& marks = (*covered)[r];
    const std::size_t col_end =
        std::min(marks.size(), col + static_cast<std::size_t>(col_span));
    for (std::size_t c = col; c < col_end; ++c) marks[c] = true;
  }
}

class GdocsRenderer : RendererBase {
 public:
  explicit GdocsRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    writer_.begin_object();
    writer_.member_string("title", document_title());
    writer_.key("body");
    writer_.begin_object();
    writer_.key("content");
    writer_.begin_array();
    render_children(document_.body());
    writer_.end_array();
    writer_.end_object();
    // Both registries are written from what the walk collected, which is why
    // they sit after the body they describe.
    write_lists();
    write_placeholders();
    writer_.end_object();
    return writer_.take();
  }

 private:
  // A picture the integration layer has to upload and patch in.
  struct Placeholder {
    std::string self_ref;
    std::string mimetype;
    double width = 0.0;
    double height = 0.0;
    std::int64_t content_index = 0;
  };

  JsonWriter writer_;
  // One entry per list the walk opened, in encounter order; each entry
  // records, per nesting level it reached, whether that level enumerates.
  std::vector<std::vector<bool>> lists_;
  std::vector<Placeholder> placeholders_;
  // How many structural elements body.content already holds, which is the
  // index the next one lands on.
  std::int64_t content_index_ = 0;

  // -- title ------------------------------------------------------------

  // The first body-layer title item's text, falling back to the document
  // name. The scan runs ahead of the walk (and consumes nothing, so the same
  // item still renders as a TITLE paragraph) because the API carries the
  // title outside the body content that also holds it.
  std::string document_title() const {
    std::set<std::string> seen;
    std::string title;
    return find_title(document_.body(), &seen, &title) ? title : document_.name();
  }

  // Depth-first search for that title item. `seen` is what keeps a reference
  // graph that loops back on itself from recursing forever.
  bool find_title(const docv1::GroupItem& group, std::set<std::string>* seen,
                  std::string* out) const {
    for (const auto& child : group.children()) {
      if (!seen->insert(child.ref()).second) continue;
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kText && ref.index < document_.texts_size()) {
        const auto& item = document_.texts(ref.index);
        if (item.item_case() != docv1::BaseTextItem::kTitle) continue;
        const auto* base = text_base(item);
        if (base == nullptr || excluded_layer(base->content_layer())) continue;
        *out = base->text();
        return true;
      }
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const auto& nested = document_.groups(ref.index);
        if (excluded_layer(nested.content_layer())) continue;
        if (find_title(nested, seen, out)) return true;
      }
    }
    return false;
  }

  // -- writers ----------------------------------------------------------

  // Writes one paragraph structural element into whichever array is open.
  // Table cells hold paragraphs of their own, so this never touches
  // content_index_; only the body-level callers count.
  void write_paragraph(const Paragraph& paragraph) {
    writer_.begin_object();
    writer_.key("paragraph");
    writer_.begin_object();
    writer_.key("elements");
    writer_.begin_array();
    writer_.begin_object();
    writer_.key("textRun");
    writer_.begin_object();
    writer_.member_string("content", gdocs_run_content(paragraph.text));
    writer_.key("textStyle");
    write_text_style(paragraph.monospace);
    writer_.end_object();
    writer_.end_object();
    writer_.end_array();
    writer_.key("paragraphStyle");
    writer_.begin_object();
    writer_.member_string("namedStyleType", paragraph.named_style);
    writer_.end_object();
    if (paragraph.bullet.has_value()) {
      writer_.key("bullet");
      writer_.begin_object();
      writer_.member_string("listId", gdocs_list_id(paragraph.bullet->list));
      writer_.member_int("nestingLevel", paragraph.bullet->nesting_level);
      writer_.end_object();
    }
    writer_.end_object();
    writer_.end_object();
  }

  // A run's style: empty for prose, a monospace weightedFontFamily for the
  // source text that has to keep its columns.
  void write_text_style(bool monospace) {
    writer_.begin_object();
    if (monospace) {
      writer_.key("weightedFontFamily");
      writer_.begin_object();
      writer_.member_string("fontFamily", kCodeFontFamily);
      writer_.member_int("weight", kRegularWeight);
      writer_.end_object();
    }
    writer_.end_object();
  }

  // Writes a paragraph as the next element of body.content.
  void add_paragraph(const Paragraph& paragraph) {
    write_paragraph(paragraph);
    ++content_index_;
  }

  // -- tree walk --------------------------------------------------------

  void render_children(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) render_ref(child.ref());
  }

  void render_ref(const std::string& raw) {
    if (!consume(raw)) return;
    const ArenaRef ref = parse_ref(raw);
    switch (ref.kind) {
      case ArenaRef::kText:
        if (ref.index < document_.texts_size()) render_text(document_.texts(ref.index));
        break;
      case ArenaRef::kTable:
        if (ref.index < document_.tables_size()) render_table(document_.tables(ref.index));
        break;
      case ArenaRef::kPicture:
        if (ref.index < document_.pictures_size()) {
          render_picture(document_.pictures(ref.index));
        }
        break;
      case ArenaRef::kGroup:
        if (ref.index < document_.groups_size()) render_group(document_.groups(ref.index));
        break;
      case ArenaRef::kFieldRegion:
        if (ref.index < document_.field_regions_size()) {
          for (const auto& child : document_.field_regions(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      case ArenaRef::kFieldItem:
        if (ref.index < document_.field_items_size()) {
          for (const auto& child : document_.field_items(ref.index).children()) {
            render_ref(child.ref());
          }
        }
        break;
      // The API has no element for a key-value or form graph and no comment
      // syntax to leave a marker in, so those arenas are skipped outright
      // rather than degraded the way the markup exports degrade them.
      case ArenaRef::kKeyValue:
      case ArenaRef::kForm:
      case ArenaRef::kUnknown:
        break;
    }
  }

  void render_text(const docv1::BaseTextItem& item) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      const auto& code = item.code();
      if (excluded_layer(code.content_layer())) return;
      // Code is a normal paragraph in a monospace run: the API has no code
      // named style, and a style the create body cannot name is worse than
      // one it can.
      add_paragraph(Paragraph::source(code.text()));
      return;
    }
    const auto* base = text_base(item);
    if (base == nullptr || excluded_layer(base->content_layer())) return;
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        add_paragraph(Paragraph::styled(base->text(), std::string(kTitleStyle)));
        return;
      case docv1::BaseTextItem::kSectionHeader:
      case docv1::BaseTextItem::kFieldHeading: {
        const int level = item.item_case() == docv1::BaseTextItem::kSectionHeader
                              ? item.section_header().level()
                              : item.field_heading().level();
        add_paragraph(Paragraph::styled(base->text(), gdocs_heading_style(level)));
        return;
      }
      case docv1::BaseTextItem::kFormula:
        // Formula source keeps its columns, so it rides the same monospace
        // run code does; an undecoded formula has nothing to insert at all.
        if (!base->text().empty()) {
          add_paragraph(Paragraph::source(base->text()));
        }
        return;
      case docv1::BaseTextItem::kListItem:
        // A list item outside a list group still reads as a one-item list,
        // exactly as it does in the HTML export.
        add_paragraph(Paragraph::bulleted(
            base->text(), Bullet{open_list(item.list_item().enumerated()), 0}));
        return;
      default:
        break;
    }
    if (!base->text().empty()) add_paragraph(Paragraph::prose(base->text()));
  }

  void render_group(const docv1::GroupItem& group) {
    if (excluded_layer(group.content_layer())) return;
    if (list_group(group)) {
      render_list(group, open_list(ordered_list(group)), 0);
      return;
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      add_inline_paragraph(group);
      return;
    }
    render_children(group);
  }

  // An inline group folds into one paragraph, its texts joined with a space,
  // the way the HTML export folds it.
  void add_inline_paragraph(const docv1::GroupItem& group) {
    std::string text;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      const auto* base = text_base(document_.texts(ref.index));
      if (base == nullptr || base->text().empty()) continue;
      if (!text.empty()) text.push_back(' ');
      text.append(base->text());
    }
    if (!text.empty()) add_paragraph(Paragraph::prose(text));
  }

  // -- lists ------------------------------------------------------------

  static bool list_group(const docv1::GroupItem& group) {
    return group.label() == docv1::GROUP_LABEL_LIST ||
           group.label() == docv1::GROUP_LABEL_ORDERED_LIST;
  }

  // Whether a list group numbers its items: the group label decides when it
  // is explicit, otherwise the first list item in it does.
  bool ordered_list(const docv1::GroupItem& group) const {
    if (group.label() == docv1::GROUP_LABEL_ORDERED_LIST) return true;
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      const auto& item = document_.texts(ref.index);
      if (item.item_case() != docv1::BaseTextItem::kListItem) continue;
      return item.list_item().enumerated();
    }
    return false;
  }

  // Opens a list and returns its index. Only a top-level list group (or a
  // stray list item) opens one; a sublist shares its parent's identifier,
  // which is how the API models nesting.
  std::size_t open_list(bool enumerated) {
    lists_.push_back(std::vector<bool>{enumerated});
    return lists_.size() - 1;
  }

  // Grows a list's level table far enough to declare `level`, filling every
  // level it passes with the glyph the reaching group asked for. An existing
  // level keeps the glyph it was opened with.
  void record_level(std::size_t list, int level, bool enumerated) {
    auto& levels = lists_[list];
    while (static_cast<int>(levels.size()) <= level) levels.push_back(enumerated);
  }

  // Emits one bulleted paragraph per item of a list group. A nested list
  // group keeps the identifier and steps the nesting level.
  void render_list(const docv1::GroupItem& group, std::size_t list, int level) {
    record_level(list, level, ordered_list(group));
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        const auto& nested = document_.groups(ref.index);
        if (list_group(nested) && !excluded_layer(nested.content_layer()) &&
            consume(child.ref())) {
          render_list(nested, list, std::min(level + 1, kMaxNestingLevel));
        }
        continue;
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      const auto* base = text_base(document_.texts(ref.index));
      if (base == nullptr || excluded_layer(base->content_layer())) continue;
      add_paragraph(Paragraph::bulleted(base->text(), Bullet{list, level}));
    }
  }

  // -- floats -----------------------------------------------------------

  void render_table(const docv1::TableItem& table) {
    if (excluded_layer(table.content_layer())) return;
    // The caption is claimed first so a caption item the tree links twice
    // stays consumed even when the table itself renders nothing.
    const std::vector<std::string> captions = caption_texts(table.captions());
    const auto grid = table_grid(table.data());
    if (grid.empty() && captions.empty()) return;
    // The API has no caption element on a table, so a claimed caption rides
    // as the paragraph immediately ahead of its table.
    const std::string caption = join_captions(captions);
    if (!caption.empty()) add_paragraph(Paragraph::prose(caption));
    if (grid.empty()) return;
    write_table(grid);
    ++content_index_;
  }

  void write_table(const std::vector<std::vector<const docv1::TableCell*>>& grid) {
    std::size_t columns = 0;
    for (const auto& row : grid) columns = std::max(columns, row.size());
    // A spanned cell is written once, at the first position it covers; the
    // coverage map suppresses its mirrored continuations whether or not the
    // producer stamped span offsets on every mirror.
    std::vector<std::vector<bool>> covered(grid.size(),
                                           std::vector<bool>(columns, false));
    writer_.begin_object();
    writer_.key("table");
    writer_.begin_object();
    writer_.member_int("rows", static_cast<std::int64_t>(grid.size()));
    writer_.member_int("columns", static_cast<std::int64_t>(columns));
    writer_.key("tableRows");
    writer_.begin_array();
    for (std::size_t row = 0; row < grid.size(); ++row) {
      writer_.begin_object();
      writer_.key("tableCells");
      writer_.begin_array();
      for (std::size_t col = 0; col < grid[row].size(); ++col) {
        if (covered[row][col]) continue;
        const docv1::TableCell* cell = grid[row][col];
        const int row_span = cell != nullptr ? std::max(cell->row_span(), 1) : 1;
        const int col_span = cell != nullptr ? std::max(cell->col_span(), 1) : 1;
        mark_covered(&covered, row, col, row_span, col_span);
        write_table_cell(
            cell != nullptr ? std::string_view(cell->text()) : std::string_view(),
            row_span, col_span);
      }
      writer_.end_array();
      writer_.end_object();
    }
    writer_.end_array();
    writer_.end_object();
    writer_.end_object();
  }

  // One table cell. The API keeps a cell's text in structural elements of its
  // own, so the cell text becomes a single NORMAL_TEXT paragraph, and the
  // spans a covered position was skipped for live on the cell style.
  void write_table_cell(std::string_view text, int row_span, int col_span) {
    writer_.begin_object();
    writer_.key("content");
    writer_.begin_array();
    write_paragraph(Paragraph::prose(std::string(text)));
    writer_.end_array();
    writer_.key("tableCellStyle");
    writer_.begin_object();
    writer_.member_int("rowSpan", row_span);
    writer_.member_int("columnSpan", col_span);
    writer_.end_object();
    writer_.end_object();
  }

  // A picture leaves a paragraph behind and an entry in the placeholder
  // registry; see the image contract in this file's header.
  void render_picture(const docv1::PictureItem& picture) {
    if (excluded_layer(picture.content_layer())) return;
    const std::vector<std::string> captions = caption_texts(picture.captions());
    Placeholder placeholder;
    placeholder.self_ref = picture.self_ref();
    if (picture.has_image()) {
      placeholder.mimetype = picture.image().mimetype();
      placeholder.width = picture.image().size().width();
      placeholder.height = picture.image().size().height();
    }
    placeholder.content_index = content_index_;
    placeholders_.push_back(std::move(placeholder));
    add_paragraph(Paragraph::prose(join_captions(captions)));
  }

  // -- registries -------------------------------------------------------

  // The list registry, one entry per opened list, each declaring the glyph of
  // every nesting level it reached. Only the glyph fields are emitted: the
  // rest of a nesting level's shape has defaults the API fills in.
  void write_lists() {
    writer_.key("lists");
    writer_.begin_object();
    for (std::size_t index = 0; index < lists_.size(); ++index) {
      writer_.key(gdocs_list_id(index));
      writer_.begin_object();
      writer_.key("listProperties");
      writer_.begin_object();
      writer_.key("nestingLevels");
      writer_.begin_array();
      for (const bool enumerated : lists_[index]) {
        writer_.begin_object();
        if (enumerated) {
          writer_.member_string("glyphType", kDecimalGlyph);
        } else {
          writer_.member_string("glyphSymbol", kBulletGlyph);
        }
        writer_.end_object();
      }
      writer_.end_array();
      writer_.end_object();
      writer_.end_object();
    }
    writer_.end_object();
  }

  void write_placeholders() {
    writer_.key("inlineImagePlaceholders");
    writer_.begin_array();
    for (const auto& placeholder : placeholders_) {
      writer_.begin_object();
      writer_.member_string("selfRef", placeholder.self_ref);
      writer_.member_string("mimeType", placeholder.mimetype);
      // Always present, zeroed when the picture declares no image, so the
      // integration reads one shape whatever the producer supplied.
      writer_.key("size");
      writer_.begin_object();
      writer_.member_double("width", placeholder.width);
      writer_.member_double("height", placeholder.height);
      writer_.end_object();
      writer_.member_int("contentIndex", placeholder.content_index);
      writer_.end_object();
    }
    writer_.end_array();
  }
};

}  // namespace

std::string render_gdocs_json(const docv1::Document& document) {
  return GdocsRenderer(document).render();
}

}  // namespace grparse
