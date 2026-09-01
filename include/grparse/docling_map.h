// Ported from grpc-libreoffice src/docling_map.h (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
#ifndef GRPARSE_DOCLING_MAP_H
#define GRPARSE_DOCLING_MAP_H

#include <deque>
#include <map>
#include <set>
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
// page-local with COORD_ORIGIN_TOPLEFT; a page whose rectangle never
// arrived cannot be reduced that way, and warnings() names it. Draw,
// Impress, and Calc positions are already page-local per part. All emitted
// doubles stay in twips; unit policy beyond that is the consumer's.
//
// The office wire counts comments, tracked changes, bookmarks, and field
// marks in one document-absolute character space. The mapper keeps an index
// of that space while body paragraphs stream past, and resolves every
// anchor against it once the terminal RenderStatus arrives: a comment
// back-links to the item it annotates, a tracked change and a bookmark each
// carry a FineRef into an item's own character range, and a cross-reference
// field points at the anchor it names. An anchor that falls in content the
// fold does not emit is kept without a target rather than dropped.
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

  // The warnings the terminal RenderStatus carried, plus the fold's own:
  // anything the mapper had to approximate rather than map, in the order it
  // happened.
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
    ai::pipestream::document::v1::BaseTextItem* item = nullptr;
    ai::pipestream::document::v1::TextItemBase* base = nullptr;
    std::string ref;
  };

  // One body item's extent in the document-absolute character space, in
  // arrival order (which is ascending offset order).
  struct BodySpan {
    long long start = 0;
    long long end = 0;
    std::string ref;
  };

  // A comment item waiting for the body index to be complete so its
  // anchored item can be found and back-linked.
  struct PendingComment {
    std::string ref;
    long long start = 0;
    long long end = 0;
  };

  // A cross-reference span waiting for the anchor it names to be resolved.
  struct PendingReference {
    std::string item_ref;
    int span_index = 0;
    std::string target_name;
  };

  // A named anchor and the document-absolute range it covers, resolved to
  // an item once the body index is complete.
  struct PendingAnchor {
    std::string name;
    long long start = 0;
    long long end = 0;
  };

  // A form field and the document-absolute range it covers; the index is
  // the field's own arena position.
  struct PendingFieldSpan {
    int index = 0;
    long long start = 0;
    long long end = 0;
  };

  // A tracked change and the document-absolute range it touches; the index
  // is the change's own arena position.
  struct PendingChange {
    int index = 0;
    long long start = 0;
    long long end = 0;
  };

  ai::pipestream::document::v1::GroupItem* group_by_ref(const std::string& ref);
  // Appends child_ref to the children of parent_ref: a group, the two
  // roots, a form arena item, or a picture or table that owns the child
  // (a chart's data table, a caption).
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
  // Creates the form region and the form whose graph pairs the fields, once
  // the first form field arrives.
  void ensure_form_arena();

  ai::pipestream::document::v1::TableItem* add_table(
      ai::pipestream::document::v1::ContentLayer layer,
      const std::string& parent_ref, std::string* ref_out);

  // Appends one page-local ProvenanceItem. page_index is the wire's
  // zero-based index; -1 appends nothing. page_local says whether l/t/r/b
  // are already page-local; document-absolute boxes have the page origin
  // subtracted when DocumentInfo carried the page rectangle. When it did
  // not, the box is kept as it came and a warning names the page, so a
  // consumer is told the coordinate space rather than left to trust it.
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
  // dimensions, placed cells with their merge spans, and the rich runs of
  // each cell as inline spans. A split or merged office cell keeps the
  // base-grid position its name anchors at, so a merged table stays
  // structurally readable; only a cell name the office core never anchored
  // falls back to a custom field. Cells carrying per-cell line rectangles
  // get a page-local bbox.
  void fold_table(const ai::pipestream::office::v1::TableData& table,
                  ai::pipestream::document::v1::TableItem* item);

  // Appends one InlineSpan per coalesced run: adjacent runs agreeing on
  // every character attribute become one span whose range is code points
  // into the item's own text. Runs carrying nothing worth recording add no
  // span. owner_ref, when non-empty, registers each cross-reference span
  // for resolution against the document's named anchors. base_offset is
  // where the first run starts in the item's text, for items assembled from
  // several run sequences.
  void add_run_spans(
      const google::protobuf::RepeatedPtrField<
          ai::pipestream::office::v1::TextRun>& runs,
      google::protobuf::RepeatedPtrField<
          ai::pipestream::document::v1::InlineSpan>* spans,
      const std::string& owner_ref, long long base_offset = 0);

  // Registers one embedded object as an attachment of the document: its
  // container class id, the container's own word for what it is, and the
  // item the payload became.
  void register_embedded_object(
      const ai::pipestream::office::v1::EmbeddedObject& object,
      const std::string& item_ref);

  // Emits one chart composite: a CHART picture, the data table bound under
  // it as its child, and a caption from the chart title. `object` is the
  // collector's embedded chart (null when only a SheetChart arrived) and
  // `sheet_chart` the sheet-side event naming its source ranges (null off
  // sheets). The table folds the typed series when the object carries any,
  // the sheet cells its ranges cover otherwise. Geometry is the object's
  // laid-out box on page_index, page-local or document-absolute as the
  // caller says.
  void emit_chart(const ai::pipestream::office::v1::EmbeddedObject* object,
                  const ai::pipestream::office::v1::SheetChart* sheet_chart,
                  const std::string& parent_ref,
                  ai::pipestream::document::v1::ContentLayer layer,
                  bool page_local, int page_index, double l, double t,
                  double r, double b);
  // Folds an embedded chart's series into a table: one label row on top
  // (column headers), categories down the first column (row headers), one
  // series per further column, numbers typed. Scatter series put their x
  // values in the first column.
  void fold_chart_series(
      const ai::pipestream::office::v1::EmbeddedChart& chart,
      ai::pipestream::document::v1::TableData* data);
  // Names a series table's blank corner cell from the sheet header cell at
  // the chart's source range top-left, when the range declares column
  // headers (a pie has no axis title to name it otherwise).
  void name_chart_corner(const ai::pipestream::office::v1::SheetChart& chart,
                         ai::pipestream::document::v1::TableData* data);
  // Folds the sheet cells a SheetChart's ranges cover into a table rebased
  // at the ranges' top-left corner, header flags from the chart's own
  // has_column_headers / has_row_headers. False when the sheet's table has
  // not been mapped.
  bool fold_sheet_range(const ai::pipestream::office::v1::SheetChart& chart,
                        ai::pipestream::document::v1::TableData* data);
  // The typed per-kind annotation plus the tabular projection on the chart
  // picture, for consumers of the upstream annotation vocabulary.
  void add_chart_annotations(
      const ai::pipestream::office::v1::EmbeddedChart& chart,
      ai::pipestream::document::v1::PictureItem* picture);
  // Takes the first pending chart of page_index, or the one whose laid-out
  // position matches `at` when given. False when none is waiting.
  bool take_pending_chart(int page_index,
                          const ai::pipestream::office::v1::TwipsPoint* at,
                          ai::pipestream::office::v1::EmbeddedObject* out);
  // Emits every chart still waiting once the stream ends, under the sheet
  // or slide group of its page when one was mapped, else under the body.
  void flush_pending_charts();
  // Marks the header row of each sheet table once every row has arrived:
  // a database range that declares one, else the first row with two or
  // more text cells directly above a row carrying typed quantities. A lone
  // merged text cell spanning the width above the header is a section row.
  void mark_sheet_header_rows();

  // Moves child_ref, already among parent_ref's children, to directly after
  // after_ref (to the front when after_ref is empty or absent).
  void move_child_after(const std::string& parent_ref, const std::string& child_ref,
                        const std::string& after_ref);
  // The slot of the empty paragraph an inline picture is anchored in: same
  // page, caret at or below the picture's top and within its height plus a
  // line. Consumed on return; -1 when no slot fits.
  int take_anchor_slot(int page_index, long long anchor_y, long long height);

  // The name of a sheet by its zero-based ordinal; empty when no Sheet
  // header for it has arrived.
  std::string sheet_label(int index) const;

  // The text item behind an arena reference ("#/texts/N"); null when the
  // reference names no text item.
  ai::pipestream::document::v1::TextItemBase* text_by_ref(
      const std::string& ref);

  // Resolves a range of the document-absolute character space to the item
  // that holds it, with the range rebased to that item's own text. False
  // when no emitted item covers the start of the range.
  bool resolve_doc_span(long long start, long long end,
                        ai::pipestream::document::v1::FineRef* out) const;

  // Resolves everything that anchors in the document-absolute character
  // space once the whole body has streamed past: comment back-links,
  // tracked-change targets, named anchors, and the cross-reference spans
  // that point at them.
  void resolve_anchors();

  // Checks every page style name a page carries against the PageStyle
  // catalogue, which streams in after the page images do. A name that
  // matches nothing in a catalogue that was collected is kept, because it
  // is still what the layout reported, and named in a warning so the
  // divergence is visible rather than silent.
  void resolve_page_styles();

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
  // The document's own language tag, so a run only carries one when it
  // differs.
  std::string document_language_;
  std::vector<ai::pipestream::office::v1::PageRect> page_rects_;
  // The 1-based page numbers whose style name came off the wire, so the
  // catalogue check at the end of the stream visits only those.
  std::vector<int> styled_pages_;
  // The document-absolute character space, one entry per emitted body item
  // in ascending offset order.
  std::vector<BodySpan> body_spans_;
  std::vector<PendingComment> pending_comments_;
  // Comment items by the office core's comment name, and the reply links
  // waiting for the comment they name to arrive.
  std::map<std::string, std::string> comment_ref_by_name_;
  std::vector<std::pair<std::string, std::string>> pending_comment_parents_;
  // Embedded objects registered as attachments, numbered in arrival order.
  int attachment_index_ = 0;
  // Where an empty Writer paragraph sat in the body: an inline picture's
  // anchor paragraph is exactly such a paragraph, so the picture takes its
  // place in the reading order instead of trailing the body.
  struct ParagraphSlot {
    int page_index = -1;
    long long caret_y = 0;
    // The body child the slot follows; empty when it led the body.
    std::string after_ref;
  };
  std::vector<ParagraphSlot> paragraph_slots_;
  // True once a slide's title placeholder has become the deck's title;
  // every later slide title is a section heading.
  bool deck_title_emitted_ = false;
  // Embedded charts waiting for the event that places them in the reading
  // order (a sheet's SheetChart, a slide's OLE2 shape), keyed by the sheet
  // or slide they sit on, in arrival order. Writer charts never wait: their
  // caret anchor places them as they arrive.
  std::map<int, std::deque<ai::pipestream::office::v1::EmbeddedObject>>
      pending_charts_;
  // Named ranges arrive before the sheets they sit on, so the sheet each
  // one names is filled in once the sheet headers have streamed past:
  // (index in Document.named_ranges, zero-based sheet ordinal).
  std::vector<std::pair<int, int>> pending_range_sheets_;
  std::vector<PendingReference> pending_references_;
  std::vector<PendingAnchor> pending_anchors_;
  std::vector<PendingChange> pending_changes_;
  std::vector<PendingFieldSpan> pending_field_spans_;
  // Pages a document-absolute box was stamped against without a rectangle
  // to subtract, so the warning that its coordinates stay document-absolute
  // is emitted once per page instead of once per box.
  std::set<int> unresolved_prov_pages_;
  // Per-sheet arena bookkeeping: the sheet's group ref, its folded table's
  // arena index, its lazily created comment-section group ref, and its
  // content layer (hidden sheets map to the invisible layer).
  std::map<int, std::string> sheet_group_;
  std::map<int, int> sheet_table_;
  std::map<int, std::string> sheet_comments_;
  std::map<int, std::string> sheet_name_;
  std::map<int, ai::pipestream::document::v1::ContentLayer> sheet_layer_;
  std::map<int, std::string> slide_group_;
  // Draw group nesting: (page index, child group_path) to the group's ref,
  // so a shape attaches under the group its group_path names.
  std::map<std::pair<int, std::string>, std::string> draw_groups_;
  // Writer draw-page group nesting: child group_path to the group's ref.
  // The text document has a single draw page, so the path alone keys it.
  std::map<std::string, std::string> writer_groups_;
  // The lazily created document-level comment section (Writer comments and
  // slide annotations).
  std::string comments_group_ref_;
  // The lazily created form arena: the region every field item hangs from,
  // and the form whose graph pairs each key with its value.
  std::string field_region_ref_;
  std::string form_item_ref_;
  int graph_cell_id_ = 0;
};

// Returns structural integrity problems of a mapped document: RefItem
// references that do not resolve to an arena item, parent links whose group
// does not list the item among its children, graph-cell item_refs pointing
// at nothing, and provenance on a page number the 1-based dialect has no
// page for. Every linked arena is walked, the four form arenas included.
// Empty means well formed.
std::vector<std::string> docling_integrity_errors(
    const ai::pipestream::document::v1::Document& document);

}  // namespace grparse

#endif
