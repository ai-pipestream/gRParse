#include "text_class.h"

#include "ai/pipestream/document/v1/document.pb.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {

TextClass classify_text(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return TextClass::kTitle;
    case docv1::BaseTextItem::kSectionHeader: return TextClass::kSectionHeader;
    case docv1::BaseTextItem::kListItem: return TextClass::kListItem;
    case docv1::BaseTextItem::kCode: return TextClass::kCode;
    case docv1::BaseTextItem::kFormula: return TextClass::kFormula;
    case docv1::BaseTextItem::kFieldHeading: return TextClass::kFieldHeading;
    case docv1::BaseTextItem::kFieldValue: return TextClass::kFieldValue;
    case docv1::BaseTextItem::kText: break;
    case docv1::BaseTextItem::ITEM_NOT_SET: return TextClass::kText;
  }
  switch (item.text().base().label()) {
    case docv1::DOC_ITEM_LABEL_TITLE: return TextClass::kTitle;
    case docv1::DOC_ITEM_LABEL_SECTION_HEADER: return TextClass::kSectionHeader;
    case docv1::DOC_ITEM_LABEL_LIST_ITEM: return TextClass::kListItem;
    case docv1::DOC_ITEM_LABEL_FORMULA: return TextClass::kFormula;
    case docv1::DOC_ITEM_LABEL_FIELD_HEADING: return TextClass::kFieldHeading;
    case docv1::DOC_ITEM_LABEL_FIELD_VALUE: return TextClass::kFieldValue;
    case docv1::DOC_ITEM_LABEL_CODE: return TextClass::kCode;
    default: return TextClass::kText;
  }
}

// The label vocabulary the export includes. Everything outside it is dropped
// (its captions still render, exactly as in the reference).
bool exported_label(docv1::DocItemLabel label) {
  switch (label) {
    case docv1::DOC_ITEM_LABEL_CHART:
    case docv1::DOC_ITEM_LABEL_GRADING_SCALE:
    case docv1::DOC_ITEM_LABEL_FIELD_REGION:
    case docv1::DOC_ITEM_LABEL_FIELD_ITEM: return false;
    default: return true;
  }
}

// The label the model reconstructs: an unset tag falls back to the class
// default, which the export vocabulary always contains.
docv1::DocItemLabel effective_label(docv1::DocItemLabel label,
                                    docv1::DocItemLabel fallback) {
  return label == docv1::DOC_ITEM_LABEL_UNSPECIFIED ? fallback : label;
}

}  // namespace grparse::render
