// The core of the office fold: the growing Document, the arenas every item
// is appended to, the parent-child links between them, and the provenance
// stamping that turns office geometry into page-local boxes. It knows
// nothing about any one part of an office document; the folds above it do.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "grparse/office_fold/fold_common.h"

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
  // Records what the fold had to approximate rather than map, in the order
  // it happened.
  void warn(std::string message) { warnings_.push_back(std::move(message)); }

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

  // Appends one page-local ProvenanceItem. page_index is the wire's
  // zero-based index; -1 appends nothing. page_local says whether l/t/r/b
  // are already page-local; document-absolute boxes have the page origin
  // subtracted when DocumentInfo carried the page rectangle. When it did
  // not, the box is kept as it came and a warning names the page, so a
  // consumer is told the coordinate space rather than left to trust it.
  void add_prov(ProvenanceItems* prov, int page_index, bool page_local,
                double l, double t, double r, double b, long long span_start,
                long long span_end);
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
  // per page when no rectangle for it ever arrived.
  void to_page_local(int page_index, double* l, double* t, double* r,
                     double* b);

  docv1::Document document_;
  std::vector<std::string> warnings_;
  std::string document_type_;
  std::string language_;
  std::vector<officev1::PageRect> page_rects_;
  // Pages a document-absolute box was stamped against without a rectangle
  // to subtract, so the warning that its coordinates stay document-absolute
  // is emitted once per page instead of once per box.
  std::set<int> unresolved_prov_pages_;
};

}  // namespace grparse::office_fold
