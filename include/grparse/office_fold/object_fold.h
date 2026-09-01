// Embedded objects: the OLE payloads a document carries. A formula becomes
// a formula item, a spreadsheet an inner table, a chart the chart fold's
// composite, and anything else a picture of its replacement image.
#pragma once

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/attachments.h"
#include "grparse/office_fold/chart_fold.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class ObjectFold {
 public:
  ObjectFold(DocumentArena& arena, ChartFold& charts,
             AttachmentRegistry& attachments)
      : arena_(arena), charts_(charts), attachments_(attachments) {}

  void on_embedded_object(const officev1::EmbeddedObject& object);

 private:
  // The laid-out box of an object: Writer text-anchored objects carry a
  // document-absolute caret anchor; draw-page objects carry a page-local
  // position.
  struct ObjectBox {
    bool page_local = false;
    double l = 0;
    double t = 0;
    double r = 0;
    double b = 0;
  };

  void add_formula(const officev1::EmbeddedObject& object,
                   const ObjectBox& box);
  void add_inner_table(const officev1::EmbeddedObject& object,
                       const ObjectBox& box);
  void add_object_picture(const officev1::EmbeddedObject& object,
                          const ObjectBox& box);

  DocumentArena& arena_;
  ChartFold& charts_;
  AttachmentRegistry& attachments_;
};

}  // namespace grparse::office_fold
