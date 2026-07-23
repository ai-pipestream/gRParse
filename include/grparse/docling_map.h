// Ported from grpc-libreoffice src/docling_map.h (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
#ifndef GRPARSE_DOCLING_MAP_H
#define GRPARSE_DOCLING_MAP_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"

namespace grparse {

// DoclingMapper folds a StreamPages response stream into one docling-parity
// ai.pipestream.document.v1.Document. It is the consumer-side counterpart of
// the office worker: it never touches LibreOffice, never reloads anything,
// and holds only the growing Document. Events are consumed in arrival order
// with O(1) work per event plus appends, so the worker stream stays
// single-pass and emit-as-parsed.
//
// The wire is the lossless boundary; this mapper is the lossy one. Fields
// with no docling slot (chart bubble sizes, shape rotation) are dropped;
// fields worth keeping but without a native node (Calc numeric values and
// formulas, named ranges, chain names) ride custom_fields.
//
// Coordinates: office positions are twips. Writer anchors and LineBox
// rectangles are document-absolute; the mapper subtracts the containing
// page's origin (from DocumentInfo.page_rects) so every BoundingBox is
// page-local with COORD_ORIGIN_TOPLEFT. Draw, Impress, and Calc positions
// are already page-local per part. All emitted doubles stay in twips; unit
// policy beyond that is the consumer's.
//
// A partial event stream (StreamOptions part selection) builds a valid
// Document from any subset; only DocumentInfo and RenderStatus are assumed.
class DoclingMapper {
 public:
  DoclingMapper();

  // Consumes one response event. Events must arrive in stream order.
  void consume(const ai::pipestream::office::v1::StreamPagesResponse& event);

  // True once the terminal RenderStatus has been consumed.
  bool finished() const { return finished_; }

  // The warnings carried by the terminal RenderStatus.
  const std::vector<std::string>& warnings() const { return warnings_; }

  // The accumulated document. Structurally valid at any point in the
  // stream; complete once finished().
  const ai::pipestream::document::v1::Document& document() const {
    return document_;
  }

  // Moves the accumulated document out; the mapper is spent afterwards.
  ai::pipestream::document::v1::Document take() { return std::move(document_); }

 private:
  // The classification of a new text item, selecting its BaseTextItem
  // variant.
  enum class TextKind { kTitle, kSectionHeader, kList, kFormula, kText };

  // The handle to a freshly appended text item: the shared base fields plus
  // its arena reference.
  struct TextHandle {
    ai::pipestream::document::v1::BaseTextItem* item = nullptr;
    ai::pipestream::document::v1::TextItemBase* base = nullptr;
    std::string ref;
  };

  ai::pipestream::document::v1::GroupItem* group_by_ref(const std::string& ref);
  void link_child(const std::string& parent_ref, const std::string& child_ref);
  // Stamps the CollectorSource attribution ("libreoffice"/"lok") every text,
  // picture, and table item carries.
  void stamp_collector_source(
      google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::SourceType>*
          source);
  ai::pipestream::document::v1::GroupItem* add_group(
      const std::string& parent_ref,
      ai::pipestream::document::v1::GroupLabel label, const std::string& name,
      ai::pipestream::document::v1::ContentLayer layer);
  TextHandle add_text(TextKind kind,
                      ai::pipestream::document::v1::DocItemLabel label,
                      ai::pipestream::document::v1::ContentLayer layer,
                      const std::string& parent_ref);
  ai::pipestream::document::v1::PictureItem* add_picture(
      ai::pipestream::document::v1::DocItemLabel label,
      ai::pipestream::document::v1::ContentLayer layer,
      const std::string& parent_ref, std::string* ref_out);
  ai::pipestream::document::v1::TableItem* add_table(
      ai::pipestream::document::v1::ContentLayer layer,
      const std::string& parent_ref, std::string* ref_out);

  // Appends one page-local ProvenanceItem. page_index is the wire's
  // zero-based index; -1 appends nothing. page_local says whether l/t/r/b
  // are already page-local; document-absolute boxes have the page origin
  // subtracted when DocumentInfo carried the page rectangle.
  void add_prov(
      google::protobuf::RepeatedPtrField<
          ai::pipestream::document::v1::ProvenanceItem>* prov,
      int page_index, bool page_local, double l, double t, double r, double b,
      long long span_start, long long span_end);
  // Appends one ProvenanceItem per LineBox, each on its line's page. A line
  // with measured character boundaries gets its exact charspan, offset from
  // span_start; unmeasured lines keep the full [span_start, span_end) item
  // span.
  void add_line_prov(
      google::protobuf::RepeatedPtrField<
          ai::pipestream::document::v1::ProvenanceItem>* prov,
      const google::protobuf::RepeatedPtrField<
          ai::pipestream::office::v1::LineBox>& lines,
      long long span_start, long long span_end);
  // Coarse fallback provenance from the caret anchors when no line
  // rectangles were measured.
  void add_caret_prov(
      google::protobuf::RepeatedPtrField<
          ai::pipestream::document::v1::ProvenanceItem>* prov,
      int page_index, const ai::pipestream::office::v1::TwipsPoint& start,
      const ai::pipestream::office::v1::TwipsPoint& end, long long span_start,
      long long span_end);

  // Folds an office TableData cell grid into a docling TableItem: grid
  // dimensions, placed cells, and split or merged cells that do not map to
  // the base grid as custom_fields keyed by their office cell name. Cells
  // carrying per-cell line rectangles get a page-local bbox.
  void fold_table(const ai::pipestream::office::v1::TableData& table,
                  ai::pipestream::document::v1::TableItem* item);

  // The page-local union of a cell's line rectangles on their first page;
  // false when there is nothing to measure.
  bool cell_bbox(const google::protobuf::RepeatedPtrField<
                     ai::pipestream::office::v1::LineBox>& lines,
                 ai::pipestream::document::v1::BoundingBox* box);

  // The zero-based page whose rectangle contains the document-absolute
  // point, resolved from DocumentInfo.page_rects; -1 when no page does.
  int page_for_point(double x, double y) const;

  void on_document_info(const ai::pipestream::office::v1::DocumentInfo& info);
  void on_page_image(const ai::pipestream::office::v1::PageImage& page);
  void on_metadata(const ai::pipestream::office::v1::DocumentMetadata& meta);
  void on_status(const ai::pipestream::office::v1::RenderStatus& status);
  void on_paragraph(const ai::pipestream::office::v1::Paragraph& paragraph);
  void on_table(const ai::pipestream::office::v1::TableData& table);
  void on_embedded_image(const ai::pipestream::office::v1::EmbeddedImage& image);
  void on_footnote(const ai::pipestream::office::v1::Footnote& footnote);
  void on_header_footer(const ai::pipestream::office::v1::HeaderFooter& block);
  void on_page_style(const ai::pipestream::office::v1::PageStyleInfo& style);
  void on_document_index(const ai::pipestream::office::v1::DocumentIndex& index);
  void on_drawing_shape(const ai::pipestream::office::v1::DrawingShape& shape);
  void on_slide(const ai::pipestream::office::v1::Slide& slide);
  void on_slide_shape(const ai::pipestream::office::v1::SlideShape& shape);
  void on_text_frame(const ai::pipestream::office::v1::TextFrame& frame);
  void on_shape(const ai::pipestream::office::v1::Shape& shape);
  void on_embedded_object(
      const ai::pipestream::office::v1::EmbeddedObject& object);
  void on_sheet(const ai::pipestream::office::v1::Sheet& sheet);
  void on_sheet_row(const ai::pipestream::office::v1::SheetRow& row);
  void on_sheet_named_range(
      const ai::pipestream::office::v1::SheetNamedRange& range);
  void on_sheet_database_range(
      const ai::pipestream::office::v1::SheetDatabaseRange& range);
  void on_sheet_cell_comment(
      const ai::pipestream::office::v1::SheetCellComment& comment);
  void on_sheet_chart(const ai::pipestream::office::v1::SheetChart& chart);
  void on_sheet_pivot_table(
      const ai::pipestream::office::v1::SheetPivotTable& pivot);
  void on_comment(const ai::pipestream::office::v1::Comment& comment);
  void on_tracked_change(
      const ai::pipestream::office::v1::TrackedChange& change);
  void on_bookmark(const ai::pipestream::office::v1::Bookmark& bookmark);
  void on_form_field(const ai::pipestream::office::v1::FormField& field);

  ai::pipestream::document::v1::Document document_;
  bool finished_ = false;
  std::vector<std::string> warnings_;
  std::string document_type_;
  std::vector<ai::pipestream::office::v1::PageRect> page_rects_;
  // Per-sheet arena bookkeeping: the sheet's group ref, its folded table's
  // arena index, its lazily created comment-section group ref, and its
  // content layer (hidden sheets map to the invisible layer).
  std::map<int, std::string> sheet_group_;
  std::map<int, int> sheet_table_;
  std::map<int, std::string> sheet_comments_;
  std::map<int, ai::pipestream::document::v1::ContentLayer> sheet_layer_;
  std::map<int, std::string> slide_group_;
  // Draw group nesting: (page index, child group_path) to the group's ref,
  // so a shape attaches under the group its group_path names.
  std::map<std::pair<int, std::string>, std::string> draw_groups_;
  // Writer draw-page group nesting: child group_path to the group's ref.
  // The text document has a single draw page, so the path alone keys it.
  std::map<std::string, std::string> writer_groups_;
  // The lazily created document-level comment section (Writer comments and
  // slide annotations) and form-field area group refs.
  std::string comments_group_ref_;
  std::string form_fields_group_ref_;
};

// Returns structural integrity problems of a mapped document: RefItem
// references that do not resolve to an arena item, and parent links whose
// group does not list the item among its children. Empty means well formed.
std::vector<std::string> docling_integrity_errors(
    const ai::pipestream::document::v1::Document& document);

}  // namespace grparse

#endif
