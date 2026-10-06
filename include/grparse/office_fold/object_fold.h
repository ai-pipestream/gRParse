// Embedded objects: the OLE payloads a document carries. A formula becomes
// a formula item, a spreadsheet an inner table, a chart the chart fold's
// composite, and anything else a picture of its replacement image.
#pragma once

#include <deque>
#include <map>
#include <string>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/attachments.h"
#include "grparse/office_fold/chart_fold.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class ShapeFold;

class ObjectFold {
 public:
  ObjectFold(DocumentArena& arena, ChartFold& charts,
             AttachmentRegistry& attachments)
      : arena_(arena), charts_(charts), attachments_(attachments) {}

  void on_embedded_object(const officev1::EmbeddedObject& object);

  // A deck's object waiting for its slide: the OLE2 shape at this position
  // on this slide places it under the slide. False when none waits there.
  bool take_pending(int page_index, const officev1::TwipsPoint& at,
                    const std::string& parent, docv1::ContentLayer layer);
  // Objects no shape claimed go under their slide once the deck is in.
  void flush(const ShapeFold& shapes);

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

  void emit(const officev1::EmbeddedObject& object, const std::string& parent,
            docv1::ContentLayer layer);
  void add_formula(const officev1::EmbeddedObject& object,
                   const ObjectBox& box, const std::string& parent,
                   docv1::ContentLayer layer);
  void add_inner_table(const officev1::EmbeddedObject& object,
                       const ObjectBox& box, const std::string& parent,
                       docv1::ContentLayer layer);
  void add_object_picture(const officev1::EmbeddedObject& object,
                          const ObjectBox& box, const std::string& parent,
                          docv1::ContentLayer layer);

  DocumentArena& arena_;
  ChartFold& charts_;
  AttachmentRegistry& attachments_;
  // A deck's objects by slide, until their OLE2 shape arrives.
  std::map<int, std::deque<officev1::EmbeddedObject>> pending_;
};

}  // namespace grparse::office_fold
