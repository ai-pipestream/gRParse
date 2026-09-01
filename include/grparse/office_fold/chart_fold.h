// The chart composite: a CHART picture, the data table bound under it as
// its child, and a caption from the chart title. A chart reaches the fold
// as up to two events, the embedded object carrying the series and the
// sheet or slide event placing it in the reading order, so the fold holds
// the ones still waiting for their place.
#pragma once

#include <deque>
#include <map>
#include <string>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/attachments.h"
#include "grparse/office_fold/fold_common.h"
#include "grparse/office_fold/sheet_fold.h"

namespace grparse::office_fold {

class ShapeFold;

class ChartFold {
 public:
  ChartFold(DocumentArena& arena, SheetFold& sheets,
            AttachmentRegistry& attachments)
      : arena_(arena), sheets_(sheets), attachments_(attachments) {}

  // Holds a chart until the event that places it in the reading order
  // arrives (a sheet's SheetChart, a slide's OLE2 shape). Writer charts
  // never wait: their caret anchor places them as they arrive.
  void hold(const officev1::EmbeddedObject& object);
  // Takes the first chart waiting on page_index, or the one whose laid-out
  // position matches `at` when given. False when none is waiting.
  bool take_pending(int page_index, const officev1::TwipsPoint* at,
                    officev1::EmbeddedObject* out);

  // Emits one chart composite. `object` is the collector's embedded chart
  // (null when only a SheetChart arrived) and `sheet_chart` the sheet-side
  // event naming its source ranges (null off sheets). The table folds the
  // typed series when the object carries any, the sheet cells its ranges
  // cover otherwise. Geometry is the object's laid-out box on page_index,
  // page-local or document-absolute as the caller says.
  void emit(const officev1::EmbeddedObject* object,
            const officev1::SheetChart* sheet_chart,
            const std::string& parent_ref, docv1::ContentLayer layer,
            bool page_local, int page_index, double l, double t, double r,
            double b);

  // Emits every chart still waiting once the stream ends, under the sheet
  // or slide group of its page when one was mapped, else under the body.
  void flush(const ShapeFold& shapes);

 private:
  // The CHART picture of a composite: its name, its replacement image, its
  // geometry and the source ranges it declares.
  docv1::PictureItem* add_chart_picture(
      const officev1::EmbeddedObject* object,
      const officev1::SheetChart* sheet_chart, const std::string& name,
      const std::string& sheet, const std::string& parent_ref,
      docv1::ContentLayer layer, bool page_local, int page_index, double l,
      double t, double r, double b, std::string* picture_ref);
  // The data table bound under the picture: typed series when the object
  // carries them, the folded tabular projection or the sheet cells the
  // ranges cover otherwise. False when the chart carried no data at all.
  bool bind_chart_data(const officev1::EmbeddedObject* object,
                       const officev1::SheetChart* sheet_chart, bool typed,
                       docv1::TableItem* table);
  // Row provenance back into the sheet grid when the table's rows are the
  // range's rows: a single source range whose header row is the label row.
  void add_row_provenance(const officev1::SheetChart& chart,
                          const std::string& sheet, docv1::TableItem* table);
  // The chart title as the composite's caption, bound to the picture.
  void add_caption(const std::string& title, docv1::PictureItem* picture,
                   const std::string& picture_ref, docv1::ContentLayer layer,
                   bool page_local, int page_index, double l, double t,
                   double r, double b);

  // The typed per-kind annotation plus the tabular projection on the chart
  // picture, for consumers of the upstream annotation vocabulary.
  void add_annotations(const officev1::EmbeddedChart& chart,
                       docv1::PictureItem* picture);
  // Folds an embedded chart's series into a table: one label row on top
  // (column headers), categories down the first column (row headers), one
  // series per further column, numbers typed. Scatter series put their x
  // values in the first column.
  void fold_series(const officev1::EmbeddedChart& chart,
                   docv1::TableData* data);
  // Names a series table's blank corner cell from the sheet header cell at
  // the chart's source range top-left, when the range declares column
  // headers (a pie has no axis title to name it otherwise).
  void name_corner(const officev1::SheetChart& chart, docv1::TableData* data);
  // Folds the sheet cells a SheetChart's ranges cover into a table rebased
  // at the ranges' top-left corner, header flags from the chart's own
  // has_column_headers / has_row_headers. False when the sheet's table has
  // not been mapped.
  bool fold_sheet_range(const officev1::SheetChart& chart,
                        docv1::TableData* data);

  DocumentArena& arena_;
  SheetFold& sheets_;
  AttachmentRegistry& attachments_;
  // Charts waiting for the event that places them, keyed by the sheet or
  // slide they sit on, in arrival order.
  std::map<int, std::deque<officev1::EmbeddedObject>> pending_charts_;
};

}  // namespace grparse::office_fold
