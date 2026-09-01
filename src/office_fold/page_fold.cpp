#include "grparse/office_fold/page_fold.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// Every coordinate this fold emits is in twips, the office core's own
// unit; the page declares it so a merge with another producer's geometry
// cannot silently mix spaces.
constexpr const char* kCoordinateUnit = "twip";

// Maps the office core's own statistic names onto the typed counters. A
// name with no counter of its own is left out rather than coerced into a
// neighbouring one.
void set_statistics(const google::protobuf::Map<std::string, int64_t>& statistics,
                    docv1::DocumentStatistics* out) {
  for (const auto& [name, count] : statistics) {
    if (name == "PageCount") out->set_pages(count);
    else if (name == "WordCount") out->set_words(count);
    else if (name == "CharacterCount") out->set_characters(count);
    else if (name == "ParagraphCount") out->set_paragraphs(count);
    else if (name == "TableCount") out->set_tables(count);
    else if (name == "ImageCount") out->set_images(count);
    else if (name == "ObjectCount") out->set_objects(count);
    else if (name == "CellCount") out->set_cells(count);
    else if (name == "SheetCount") out->set_sheets(count);
  }
}

// Everything a document records about itself has a typed slot, so nothing
// here goes through a value map. Instants come off the wire as epoch
// milliseconds and are converted, not re-rendered.
void set_source_meta(const officev1::DocumentMetadata& meta,
                     docv1::DocumentMeta* out) {
  if (!meta.title().empty()) out->set_title(meta.title());
  if (!meta.author().empty()) out->add_authors(meta.author());
  if (meta.created_epoch_ms() != 0) {
    set_instant(meta.created_epoch_ms(), out->mutable_created());
  }
  if (meta.modified_epoch_ms() != 0) {
    set_instant(meta.modified_epoch_ms(), out->mutable_modified());
  }
  if (!meta.language().empty()) out->set_language(meta.language());
  if (!meta.generator().empty()) out->set_generator(meta.generator());
  if (!meta.subject().empty()) out->set_subject(meta.subject());
  if (!meta.modified_by().empty()) out->set_modified_by(meta.modified_by());
  if (meta.printed_epoch_ms() != 0) {
    set_instant(meta.printed_epoch_ms(), out->mutable_printed());
  }
  if (!meta.printed_by().empty()) out->set_printer(meta.printed_by());
  if (!meta.template_name().empty()) out->set_template_(meta.template_name());
  if (meta.editing_cycles() != 0) {
    out->set_editing_cycles(meta.editing_cycles());
  }
  if (meta.editing_duration_seconds() != 0) {
    out->set_editing_duration_seconds(meta.editing_duration_seconds());
  }
}

void add_user_properties(const officev1::DocumentMetadata& meta,
                         docv1::DocumentMeta* out) {
  for (const officev1::UserProperty& prop : meta.user_properties()) {
    docv1::UserProperty* property = out->add_user_properties();
    property->set_name(prop.name());
    switch (prop.value_case()) {
      case officev1::UserProperty::kText:
        property->set_text(prop.text());
        break;
      case officev1::UserProperty::kNumber:
        property->set_number(prop.number());
        break;
      case officev1::UserProperty::kFlag:
        property->set_boolean(prop.flag());
        break;
      case officev1::UserProperty::kEpochMs:
        set_instant(prop.epoch_ms(), property->mutable_instant());
        break;
      case officev1::UserProperty::VALUE_NOT_SET:
        // A property the office core stored in a type this wire has no arm
        // for keeps its name and no value.
        break;
    }
  }
}

// The body's language field: the raw tag as it came, plus the enum arm when
// the primary subtag names one.
void set_body_language(const std::string& tag, docv1::BaseMeta* out) {
  docv1::LanguageMetaField* language = out->mutable_language();
  language->set_code_raw(tag);
  std::string subtag = tag.substr(0, tag.find('-'));
  std::ranges::transform(subtag, subtag.begin(),
                         [](unsigned char c) { return std::toupper(c); });
  docv1::HumanLanguageLabel code;
  if (docv1::HumanLanguageLabel_Parse("HUMAN_LANGUAGE_LABEL_" + subtag,
                                      &code)) {
    language->set_code(code);
  }
}

}  // namespace

void PageFold::on_document_info(const officev1::DocumentInfo& info) {
  docv1::Document& document = arena_.document();
  arena_.set_document_type(info.document_type());
  arena_.set_page_rects(info.page_rects());
  if (document.name().empty() && !info.document_id().empty()) {
    document.set_name(info.document_id());
  }
  docv1::DocumentOrigin* origin = document.mutable_origin();
  origin->set_mimetype(mime_for_extension(info.source_format()));
  if (!info.document_id().empty()) origin->set_filename(info.document_id());
  for (int index = 0; index < info.page_rects_size(); index++) {
    const officev1::PageRect& rect = info.page_rects(index);
    docv1::PageItem* page = &(*document.mutable_pages())[index + 1];
    page->set_page_no(index + 1);
    page->set_unit(kCoordinateUnit);
    page->mutable_size()->set_width(static_cast<double>(rect.width_twips()));
    page->mutable_size()->set_height(static_cast<double>(rect.height_twips()));
  }
}

void PageFold::on_page_image(const officev1::PageImage& image) {
  docv1::PageItem* page =
      &(*arena_.document().mutable_pages())[image.index() + 1];
  page->set_page_no(image.index() + 1);
  page->set_unit(kCoordinateUnit);
  if (page->size().width() <= 0 && image.dpi() > 0) {
    page->mutable_size()->set_width(
        static_cast<double>(image.width_px()) * 1440.0 / image.dpi());
    page->mutable_size()->set_height(
        static_cast<double>(image.height_px()) * 1440.0 / image.dpi());
  }
  // The page's own style, as the layout put it there. It names one of the
  // PageStyle declarations the fold collects, which arrive later in the
  // stream, so the name is kept here and checked against the catalogue when
  // the stream closes.
  if (!image.page_style().empty()) {
    page->set_style_name(image.page_style());
    styled_pages_.push_back(image.index() + 1);
  }
  // The request selects the page encoding, so the media type has to come
  // from what the response says it produced, not from the default.
  const std::string mime = page_image_mime(image.format());
  docv1::ImageRef* ref = page->mutable_image();
  ref->set_mimetype(mime);
  ref->set_dpi(image.dpi());
  ref->mutable_size()->set_width(image.width_px());
  ref->mutable_size()->set_height(image.height_px());
  ref->set_uri(data_uri(mime, image.png()));
}

void PageFold::on_metadata(const officev1::DocumentMetadata& meta) {
  docv1::Document& document = arena_.document();
  if (!meta.title().empty()) document.set_name(meta.title());
  arena_.set_language(meta.language());
  docv1::DocumentMeta* source_meta = document.mutable_source_meta();
  set_source_meta(meta, source_meta);
  docv1::BaseMeta* body_meta = document.mutable_body()->mutable_meta();
  for (const std::string& keyword : meta.keywords()) {
    source_meta->add_keywords(keyword);
    body_meta->mutable_keywords()->add_values(keyword);
  }
  if (!meta.statistics().empty()) {
    set_statistics(meta.statistics(), source_meta->mutable_statistics());
  }
  add_user_properties(meta, source_meta);
  if (!meta.language().empty()) set_body_language(meta.language(), body_meta);
}

void PageFold::on_page_style(const officev1::PageStyleInfo& style) {
  // Page styles are named declarations of the document, in the same twips
  // every other measurement here uses.
  docv1::PageStyle* out = arena_.document().add_page_styles();
  out->set_name(style.name());
  out->mutable_size()->set_width(static_cast<double>(style.width_twips()));
  out->mutable_size()->set_height(static_cast<double>(style.height_twips()));
  docv1::Margins* margins = out->mutable_margins();
  margins->set_left(static_cast<double>(style.margin_left_twips()));
  margins->set_top(static_cast<double>(style.margin_top_twips()));
  margins->set_right(static_cast<double>(style.margin_right_twips()));
  margins->set_bottom(static_cast<double>(style.margin_bottom_twips()));
  out->set_columns(style.columns());
}

void PageFold::resolve_page_styles() {
  const docv1::Document& document = arena_.document();
  // Nothing to resolve against when the page style catalogue was not part
  // of the request; the names on the pages stay as the layout reported
  // them.
  if (document.page_styles_size() == 0) return;
  for (int page_no : styled_pages_) {
    auto found = document.pages().find(page_no);
    if (found == document.pages().end()) continue;
    const std::string& name = found->second.style_name();
    bool declared = false;
    for (const docv1::PageStyle& style : document.page_styles()) {
      if (style.name() == name) {
        declared = true;
        break;
      }
    }
    if (!declared) {
      arena_.warn("page " + std::to_string(page_no) + " names page style \""
                  + name
                  + "\", which the style catalogue does not declare");
    }
  }
}

}  // namespace grparse::office_fold
