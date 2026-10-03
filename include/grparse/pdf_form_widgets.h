#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grparse {

// PDF AcroForm widgets folded into the Document's form subtree.
//
// The upstream model keeps a PDF widget's raw state (rect, /V, /TU, the
// field name and type, the inherited /Ff mask and the widget's /AS) on the
// parsed page's PdfWidget list, outside the document. Here every widget a
// PdfBackendService reports becomes a FieldItem in Document.field_items,
// carrying that state in typed fields, under one FieldRegionItem per page
// in the body: the field-region shape upstream's own form model builds.
// The fold runs after the body is final, so it works the same whichever
// path produced the body (the inspector's fast path, the CV path, a
// collector merge): only the widgets come from the backend.

// The widgets of one page as the backend reported them, with the page's
// PageInfo so the fold can map the contract's page space (in whichever
// frame PageInfo.page_space names, before /Rotate) onto the document's own
// page space, the way the page source maps text cells.
struct PdfPageWidgets {
  // One-based, the numbering Document.pages uses.
  int page_number = 0;
  ai::protomolt::parse::pdf::v1::PageInfo page_info;
  std::vector<ai::protomolt::parse::pdf::v1::FormField> fields;
};

// Folds the widgets into `document`: a FieldRegionItem per page that has
// any, inserted into the body after the last body item on that page or an
// earlier one, holding one FieldItem per widget in reading order (top to
// bottom, then left to right; ties keep the backend's /Annots order). Each
// FieldItem carries a field_key text (the tooltip, else the field name)
// and a value child: a field_value text for text and choice fields (kind
// "fillable", or "read_only" when /Ff says so), a checkbox_selected or
// checkbox_unselected text for check boxes and radio buttons (selected
// when the widget's /AS names a state other than /Off), none for push
// buttons and signatures.
//
// Boxes land in the page's space, measured on the page as rendered (the
// CropBox with /Rotate applied, see PdfPageFrame): a page whose
// PageItem.unit is "pt", or that has no size, keeps points; otherwise boxes
// scale by the page's size over its rendered size in points (the CV path's
// raster pixels). The origin
// follows the boxes already on that page, top-left when there are none and
// the page is sized, bottom-left for points. Items are attributed to the
// collector "grparse" with `engine` as the model. Deterministic: the same
// widgets always yield the same items in the same order.
void fold_pdf_form_widgets(const std::vector<PdfPageWidgets>& pages, const std::string& engine,
                           ai::pipestream::document::v1::Document* document);

// What one backend answered for the form-field family.
struct PdfWidgetFetch {
  bool ok = false;
  // The transport or load failure when !ok.
  std::string error;
  // The backend answered but does not read form fields (or predates the
  // family); `pages` is empty. A document without widgets is not this: it
  // comes back ok with no pages.
  bool unsupported = false;
  // The engine name the backend reported in its Parse header.
  std::string engine;
  // Pages that carry at least one widget, in page order.
  std::vector<PdfPageWidgets> pages;
};

// One Parse call for PDF_FAMILY_FORM_FIELDS over every page, with the
// same content-addressed handshake the page source uses (hash only, then
// the bytes once on LOAD_STATUS_BYTES_REQUIRED; GRPARSE_PDF_BACKEND_
// HANDSHAKE=off sends the bytes up front). Never throws.
PdfWidgetFetch fetch_pdf_form_widgets(const std::string& bytes, const std::string& target,
                                      std::chrono::system_clock::time_point deadline);

// The post-parse step source_parse runs on a PDF: when GRPARSE_PDF_BACKEND
// names backends, asks them in configured order for the widgets (the
// first that answers wins; consensus mode needs no vote, the widgets are
// stored state, not a reading) and folds them into `document`. Returns
// nothing when there was nothing to do or the fold ran, and a warning
// naming the failure when every backend failed; the document is then left
// as it was. With GRPARSE_PDF_BACKEND unset no PDF is read at all, so
// there are no widgets to fold.
std::optional<std::string> fold_pdf_form_widgets_from_backend(
    const std::string& bytes, std::chrono::system_clock::time_point deadline,
    ai::pipestream::document::v1::Document* document);

}  // namespace grparse
