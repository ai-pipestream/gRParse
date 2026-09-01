// Ported from grpc-libreoffice src/docling_map.h (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
#ifndef GRPARSE_DOCLING_MAP_H
#define GRPARSE_DOCLING_MAP_H

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/office_fold/anchor_index.h"
#include "grparse/office_fold/annotation_fold.h"
#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/attachments.h"
#include "grparse/office_fold/chart_fold.h"
#include "grparse/office_fold/form_fold.h"
#include "grparse/office_fold/object_fold.h"
#include "grparse/office_fold/page_fold.h"
#include "grparse/office_fold/shape_fold.h"
#include "grparse/office_fold/sheet_fold.h"
#include "grparse/office_fold/writer_fold.h"

namespace grparse {

// DoclingMapper folds a StreamPages response stream into one docling-parity
// ai.pipestream.document.v1.Document. It is the consumer-side counterpart of
// the office worker: it never touches LibreOffice, never reloads anything,
// and holds only the growing Document. Events are consumed in arrival order
// with O(1) work per event plus appends, so the worker stream stays
// single-pass and emit-as-parsed.
//
// The class itself is the event router and the end-of-stream sequence; the
// work belongs to the collaborators under grparse::office_fold. The arena
// holds the growing Document and the primitives every part fold appends
// through; one fold per plane of an office document (pages and metadata,
// Writer body, Impress and Draw shapes, Calc sheets, charts, forms,
// annotations, embedded objects) owns the pending state that plane needs,
// and the anchor index owns the character-space resolution pass.
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
  const std::vector<std::string>& warnings() const {
    return arena_.warnings();
  }

  // The accumulated document. Structurally valid at any point in the
  // stream; complete once finished().
  const ai::pipestream::document::v1::Document& document() const {
    return arena_.document();
  }

  // Moves the accumulated document out; the mapper is spent afterwards.
  ai::pipestream::document::v1::Document take() { return arena_.take(); }

 private:
  // The terminal event: the worker's own warnings, then everything that can
  // only be decided once the whole stream is in.
  void on_status(const ai::pipestream::office::v1::RenderStatus& status);

  // Declaration order is construction order, and every collaborator below
  // binds references to the ones above it.
  office_fold::AnchorIndex anchors_;
  office_fold::DocumentArena arena_;
  office_fold::AttachmentRegistry attachments_{arena_};
  office_fold::SheetFold sheets_{arena_};
  office_fold::ChartFold charts_{arena_, sheets_, attachments_};
  office_fold::ShapeFold shapes_{arena_, anchors_};
  office_fold::WriterFold writer_{arena_, anchors_, shapes_};
  office_fold::PageFold pages_{arena_};
  office_fold::FormFold forms_{arena_, anchors_};
  office_fold::AnnotationFold annotations_{arena_, anchors_};
  office_fold::ObjectFold objects_{arena_, charts_, attachments_};
  bool finished_ = false;
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
