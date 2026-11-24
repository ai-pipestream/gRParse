// The core of the office fold: the growing Document, the arenas every item
// is appended to, the parent-child links between them, and the provenance
// stamping that turns office geometry into page-local boxes. It knows
// nothing about any one part of an office document; the folds above it do.
#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "grparse/office_fold/fold_common.h"
#include "grparse/warning.h"

namespace grparse::office_fold {

// The classification of a new text item, selecting its BaseTextItem
// variant.
enum class TextKind {
  kTitle,
  kSectionHeader,
  kList,
  kFormula,
  kText,
  kFieldHeading,
  kFieldValue,
};

// The handle to a freshly appended text item: the shared base fields plus
// its arena reference.
struct TextHandle {
  docv1::BaseTextItem* item = nullptr;
  docv1::TextItemBase* base = nullptr;
  std::string ref;
};

class DocumentArena {
 public:
  DocumentArena();

  const docv1::Document& document() const { return document_; }
  docv1::Document& document() { return document_; }
  docv1::Document take() { return std::move(document_); }

  const std::vector<std::string>& warnings() const { return warnings_; }
  // The same warnings in the same order, each with what a consumer acts on:
  // its code, and the page and item it is about where it is about one. A
  // warning the fold or the worker stated in words only keeps
  // WARNING_CODE_UNSPECIFIED.
  const std::vector<Warning>& typed_warnings() const { return typed_warnings_; }
  // Records what the fold had to approximate rather than map, in the order
  // it happened. Both lists grow here and nowhere else, so they cannot
  // disagree.
  void warn(Warning warning) {
    warnings_.push_back(warning.message);
    typed_warnings_.push_back(std::move(warning));
  }

  // Reports an item the office core failed part way through reading, which
  // the worker marks with an ExtractionGap on the item's own event: one
  // typed warning per gap, on the page (the wire's zero-based index; -1
  // names none) and the item the fold made of the event (empty when it made
  // none). `what` names the item in words. picture_without_bytes says the
  // event is a picture that arrived with no image at all.
  void report_gap(const officev1::ExtractionGap& gap, std::string_view what,
                  int page_index, const std::string& item_ref,
                  bool picture_without_bytes = false);
  // The same, for any event that can carry a gap; nothing when it has none.
  template <typename Event>
  void report_gap_of(const Event& event, std::string_view what, int page_index,
                     const std::string& item_ref) {
    if (event.has_gap()) report_gap(event.gap(), what, page_index, item_ref);
  }
  // How many gaps were reported so far, to hold against the worker's own
  // count once the stream is in.
  int gaps_reported() const { return gaps_reported_; }

  const std::string& document_type() const { return document_type_; }
  void set_document_type(std::string type) {
    document_type_ = std::move(type);
  }
  // The document's own language tag, so a run only carries one when it
  // differs.
  const std::string& language() const { return language_; }
  void set_language(std::string language) { language_ = std::move(language); }

  // The page rectangles DocumentInfo declared, the frame document-absolute
  // geometry is reduced against.
  void set_page_rects(const google::protobuf::RepeatedPtrField<
                      officev1::PageRect>& rects);
  // The zero-based page whose rectangle contains the document-absolute
  // point; -1 when no page does.
  int page_for_point(double x, double y) const;
  // The page whose vertical extent holds document-absolute y, or -1. Pages
  // stack vertically in the layout, so y alone names one.
  int page_for_y(double y) const;
  // The page's laid-out size in twips; false when the page is unknown.
  bool page_size(int page_index, double* width, double* height) const;

  docv1::GroupItem* group_by_ref(const std::string& ref);
  // Appends child_ref to the children of parent_ref: a group, the two
  // roots, a form arena item, or a picture or table that owns the child
  // (a chart's data table, a caption).
  void link_child(const std::string& parent_ref, const std::string& child_ref);
  // Stamps the CollectorSource attribution ("libreoffice"/"lok") every text,
  // picture, and table item carries.
  void stamp_collector_source(SourceTypes* source);

  docv1::GroupItem* add_group(const std::string& parent_ref,
                              docv1::GroupLabel label, const std::string& name,
                              docv1::ContentLayer layer);
  TextHandle add_text(TextKind kind, docv1::DocItemLabel label,
                      docv1::ContentLayer layer, const std::string& parent_ref);
  docv1::PictureItem* add_picture(docv1::DocItemLabel label,
                                  docv1::ContentLayer layer,
                                  const std::string& parent_ref,
                                  std::string* ref_out);
  docv1::TableItem* add_table(docv1::ContentLayer layer,
                              const std::string& parent_ref,
                              std::string* ref_out);

  // Moves child_ref, already among parent_ref's children, to directly after
  // after_ref (to the front when after_ref is empty or absent).
  void move_child_after(const std::string& parent_ref,
                        const std::string& child_ref,
                        const std::string& after_ref);

  // The text item behind an arena reference ("#/texts/N"); null when the
  // reference names no text item.
  docv1::TextItemBase* text_by_ref(const std::string& ref);
  // Moves a placed picture, text or group (with everything under it) out
  // of the body into the furniture layer and drops its boxes, keeping its
  // pages: for content the layout repeats on every page, whose single
  // stream position says nothing about where any one copy sits.
  void move_to_furniture(const std::string& ref);

  // The picture behind "#/pictures/N"; null when the reference names none.
  docv1::PictureItem* picture_by_ref(const std::string& ref);

  // Appends one page-local ProvenanceItem. page_index is the wire's
  // zero-based index; -1 appends nothing. page_local says whether l/t/r/b
  // are already page-local; document-absolute boxes have the page origin
  // subtracted when DocumentInfo carried the page rectangle. When it did
  // not, the box keeps its document-absolute numbers but no coordinate
  // origin is claimed, and a warning names the page once, so a consumer is
  // told the coordinate space rather than left to trust it. has_geometry
  // false stamps page and charspan only: sources with no rectangle of their
  // own (a sheet grid, an unplaced chart) must not present a zero-area box
  // as real geometry.
  void add_prov(ProvenanceItems* prov, int page_index, bool page_local,
                double l, double t, double r, double b, long long span_start,
                long long span_end, bool has_geometry = true);
  // Appends one ProvenanceItem per LineBox, each on its line's page. A line
  // with measured character boundaries gets its exact charspan, offset from
  // span_start; unmeasured lines keep the full [span_start, span_end) item
  // span.
  void add_line_prov(ProvenanceItems* prov, const LineBoxes& lines,
                     long long span_start, long long span_end);
  // Coarse fallback provenance from the caret anchors when no line
  // rectangles were measured.
  void add_caret_prov(ProvenanceItems* prov, int page_index,
                      const officev1::TwipsPoint& start,
                      const officev1::TwipsPoint& end, long long span_start,
                      long long span_end);

  // The page-local union of a cell's line rectangles on their first page;
  // false when there is nothing to measure.
  bool cell_bbox(const LineBoxes& lines, docv1::BoundingBox* box);

  // Folds an office TableData cell grid into a schema TableItem: grid
  // dimensions, placed cells with their merge spans, and the rich runs of
  // each cell as inline spans. A split or merged office cell keeps the
  // base-grid position its name anchors at, so a merged table stays
  // structurally readable; only a cell name the office core never anchored
  // falls back to a custom field. Cells carrying per-cell line rectangles
  // get a page-local bbox.
  void fold_table(const officev1::TableData& table, docv1::TableItem* item);

 private:
  // The arena a "#/<arena>/N" reference names, when that arena owns
  // children of its own. False when the reference names no such item.
  bool link_into_item_arena(const std::string& parent_ref,
                            const std::string& child_ref);
  // Subtracts the page origin from a document-absolute box, or warns once
  // per page when no rectangle for it ever arrived. False when the box
  // could not be reduced and no page-local origin may be claimed for it.
  bool to_page_local(int page_index, double* l, double* t, double* r,
                     double* b);

  docv1::Document document_;
  std::vector<std::string> warnings_;
  std::vector<Warning> typed_warnings_;
  int gaps_reported_ = 0;
  std::string document_type_;
  std::string language_;
  std::vector<officev1::PageRect> page_rects_;
  // Pages a document-absolute box was stamped against without a rectangle
  // to subtract, so the warning that its coordinates stay document-absolute
  // is emitted once per page instead of once per box.
  std::set<int> unresolved_prov_pages_;
};

}  // namespace grparse::office_fold
