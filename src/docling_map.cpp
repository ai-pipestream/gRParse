// Ported from grpc-libreoffice src/docling_map.cpp (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
//
// What is left here is the router: one arm per event of the office stream,
// and the end-of-stream sequence. Every fold it dispatches to lives under
// src/office_fold.
#include "grparse/docling_map.h"

namespace grparse {

namespace officev1 = ai::pipestream::office::v1;

DoclingMapper::DoclingMapper() = default;

void DoclingMapper::consume(const officev1::StreamPagesResponse& event) {
  switch (event.event_case()) {
    case officev1::StreamPagesResponse::kDocumentInfo:
      return pages_.on_document_info(event.document_info());
    case officev1::StreamPagesResponse::kPageImage:
      return pages_.on_page_image(event.page_image());
    case officev1::StreamPagesResponse::kStatus:
      return on_status(event.status());
    case officev1::StreamPagesResponse::kMetadata:
      return pages_.on_metadata(event.metadata());
    case officev1::StreamPagesResponse::kParagraph:
      return writer_.on_paragraph(event.paragraph());
    case officev1::StreamPagesResponse::kTable:
      return writer_.on_table(event.table());
    case officev1::StreamPagesResponse::kEmbeddedImage:
      return writer_.on_embedded_image(event.embedded_image());
    case officev1::StreamPagesResponse::kFootnote:
      return writer_.on_footnote(event.footnote());
    case officev1::StreamPagesResponse::kHeaderFooter:
      return writer_.on_header_footer(event.header_footer());
    case officev1::StreamPagesResponse::kPageStyle:
      return pages_.on_page_style(event.page_style());
    case officev1::StreamPagesResponse::kDocumentIndex:
      return writer_.on_document_index(event.document_index());
    case officev1::StreamPagesResponse::kDrawingShape:
      return shapes_.on_drawing_shape(event.drawing_shape());
    case officev1::StreamPagesResponse::kSlide:
      return shapes_.on_slide(event.slide());
    case officev1::StreamPagesResponse::kSlideShape:
      return shapes_.on_slide_shape(event.slide_shape(), charts_);
    case officev1::StreamPagesResponse::kTextFrame:
      return writer_.on_text_frame(event.text_frame());
    case officev1::StreamPagesResponse::kShape:
      return writer_.on_shape(event.shape());
    case officev1::StreamPagesResponse::kEmbeddedObject:
      return objects_.on_embedded_object(event.embedded_object());
    case officev1::StreamPagesResponse::kSheet:
      return sheets_.on_sheet(event.sheet());
    case officev1::StreamPagesResponse::kSheetRow:
      return sheets_.on_sheet_row(event.sheet_row());
    case officev1::StreamPagesResponse::kSheetNamedRange:
      return sheets_.on_named_range(event.sheet_named_range());
    case officev1::StreamPagesResponse::kSheetDatabaseRange:
      return sheets_.on_database_range(event.sheet_database_range());
    case officev1::StreamPagesResponse::kSheetCellComment:
      return sheets_.on_cell_comment(event.sheet_cell_comment());
    case officev1::StreamPagesResponse::kSheetChart:
      return sheets_.on_chart(event.sheet_chart(), charts_);
    case officev1::StreamPagesResponse::kSheetPivotTable:
      return sheets_.on_pivot_table(event.sheet_pivot_table());
    case officev1::StreamPagesResponse::kComment:
      return annotations_.on_comment(event.comment());
    case officev1::StreamPagesResponse::kTrackedChange:
      return annotations_.on_tracked_change(event.tracked_change());
    case officev1::StreamPagesResponse::kBookmark:
      return annotations_.on_bookmark(event.bookmark());
    case officev1::StreamPagesResponse::kFormField:
      return forms_.on_form_field(event.form_field());
    case officev1::StreamPagesResponse::EVENT_NOT_SET:
      return;
  }
}

void DoclingMapper::on_status(const officev1::RenderStatus& status) {
  for (const std::string& warning : status.warnings()) arena_.warn(warning);
  // Charts whose placing event never came still belong to their sheet or
  // slide; sheet header rows need every row in before the first can be
  // judged against the second.
  charts_.flush(shapes_);
  sheets_.size_empty_tables();
  sheets_.mark_header_rows();
  writer_.anchor_trailing_pictures();
  // Anchors resolve only once the whole body has streamed past: a comment
  // can close before the paragraph it sits in is emitted, and a
  // cross-reference can name an anchor from a later page. The sheet each
  // range names resolves in the same pass, having been pending for the same
  // reason.
  sheets_.resolve_named_range_sheets();
  anchors_.resolve(arena_);
  pages_.resolve_page_styles();
  finished_ = true;
}

}  // namespace grparse
