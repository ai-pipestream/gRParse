// Ported from grpc-libreoffice src/docling_map.cpp (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
#include "grparse/docling_map.h"
#include "grparse/schema_version.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <set>
#include <sstream>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

// The schema identity carried on the document root: the wire schema name
// and the schema minor this repo currently mirrors. Every producer in the
// fleet stamps the same pair.
constexpr const char* kSchemaName = kWireSchemaName;
constexpr const char* kSchemaVersion = kUpstreamSchemaVersion;

// Every coordinate this mapper emits is in twips, the office core's own
// unit; the page declares it so a merge with another producer's geometry
// cannot silently mix spaces.
constexpr const char* kCoordinateUnit = "twip";

// Grids above this cell count keep table_cells only; a fully materialized
// grid over a sparse used range would dwarf the data it carries.
constexpr int kMaxGridCells = 4096;

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size()
      && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int32_t clamp32(long long value) {
  if (value < 0) return 0;
  if (value > INT32_MAX) return INT32_MAX;
  return static_cast<int32_t>(value);
}

std::string base64(const std::string& data) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8)
        | static_cast<unsigned char>(data[i + 2]);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back(alphabet[chunk & 63]);
    i += 3;
  }
  if (i + 1 == data.size()) {
    uint32_t chunk = static_cast<unsigned char>(data[i]) << 16;
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.append("==");
  } else if (i + 2 == data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string data_uri(const std::string& mime, const std::string& bytes) {
  return "data:" + mime + ";base64," + base64(bytes);
}

// The media type of a rendered page image. The request selects the encoding
// and the response names it; a producer that predates format selection sends
// PNG and no name.
std::string page_image_mime(officev1::PageImageFormat format) {
  switch (format) {
    case officev1::PAGE_IMAGE_FORMAT_JPEG: return "image/jpeg";
    case officev1::PAGE_IMAGE_FORMAT_WEBP: return "image/webp";
    case officev1::PAGE_IMAGE_FORMAT_SVG: return "image/svg+xml";
    default: return "image/png";
  }
}

// The office wire counts instants in epoch milliseconds; the schema wants
// them typed. Negative remainders borrow a second so the nanos stay in
// range for instants before 1970.
void set_instant(int64_t epoch_ms, google::protobuf::Timestamp* out) {
  int64_t seconds = epoch_ms / 1000;
  int64_t millis = epoch_ms % 1000;
  if (millis < 0) {
    millis += 1000;
    seconds -= 1;
  }
  out->set_seconds(seconds);
  out->set_nanos(static_cast<int32_t>(millis * 1000000));
}

// A color as #rrggbb.
std::string hex_color_always(uint32_t color_rgb) {
  char text[8];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x", (color_rgb >> 16) & 0xff,
                (color_rgb >> 8) & 0xff, color_rgb & 0xff);
  return text;
}

// A run's text color. The office core reports automatic color as 0, so an
// explicit black is indistinguishable from no color at all and both stay
// unset rather than claiming a color the document never declared. A
// highlight has its own sentinel and does not go through here.
std::string hex_color(uint32_t color_rgb) {
  if (color_rgb == 0) return std::string();
  return hex_color_always(color_rgb);
}

// The vertical position of a run from its escapement percentage: the office
// core raises superscript above the baseline and lowers subscript below it.
docv1::Script script_for(int escapement) {
  if (escapement > 0) return docv1::SCRIPT_SUPER;
  if (escapement < 0) return docv1::SCRIPT_SUB;
  return docv1::SCRIPT_UNSPECIFIED;
}

// Every character attribute of a run. Adjacent runs agreeing on all of them
// are one inline span; a portion boundary the reader cannot see is not a
// formatting boundary.
struct RunKey {
  std::string font;
  float size_pt = 0;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  bool monospace = false;
  bool small_caps = false;
  bool overline = false;
  std::string char_style;
  int32_t highlight_rgb = -1;
  uint32_t color_rgb = 0;
  int escapement = 0;
  std::string language;
  std::string hyperlink;
  std::string field_code;
  std::string field_target;

  bool operator==(const RunKey& other) const = default;
};

RunKey run_key(const officev1::TextRun& run) {
  RunKey key;
  key.font = run.font();
  key.size_pt = run.size_pt();
  key.bold = run.weight() >= 150.0f;
  key.italic = run.italic();
  key.underline = run.underline();
  key.strikethrough = run.strikethrough();
  key.monospace = run.monospace();
  key.small_caps = run.small_caps();
  key.overline = run.overline();
  key.char_style = run.char_style();
  key.highlight_rgb = run.highlight_rgb();
  key.color_rgb = run.color_rgb();
  key.escapement = run.escapement();
  key.language = run.language();
  key.hyperlink = run.hyperlink_url();
  key.field_code = run.field_code();
  key.field_target = run.field_target();
  return key;
}

std::string mime_for_extension(const std::string& extension) {
  static const std::map<std::string, std::string> kMimes = {
      {"doc", "application/msword"},
      {"docx", "application/vnd.openxmlformats-officedocument"
               ".wordprocessingml.document"},
      {"xls", "application/vnd.ms-excel"},
      {"xlsx", "application/vnd.openxmlformats-officedocument"
               ".spreadsheetml.sheet"},
      {"ppt", "application/vnd.ms-powerpoint"},
      {"pptx", "application/vnd.openxmlformats-officedocument"
               ".presentationml.presentation"},
      {"odt", "application/vnd.oasis.opendocument.text"},
      {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"odp", "application/vnd.oasis.opendocument.presentation"},
      {"odg", "application/vnd.oasis.opendocument.graphics"},
      {"fodt", "application/vnd.oasis.opendocument.text"},
      {"fods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"fodp", "application/vnd.oasis.opendocument.presentation"},
      {"fodg", "application/vnd.oasis.opendocument.graphics"},
      {"rtf", "application/rtf"},
      {"csv", "text/csv"},
      {"txt", "text/plain"},
      {"html", "text/html"},
      {"pdf", "application/pdf"},
  };
  auto found = kMimes.find(extension);
  return found != kMimes.end() ? found->second : "application/octet-stream";
}

google::protobuf::Value str_value(const std::string& text) {
  google::protobuf::Value value;
  value.set_string_value(text);
  return value;
}

google::protobuf::Value num_value(double number) {
  google::protobuf::Value value;
  value.set_number_value(number);
  return value;
}

std::string concat_runs(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs) {
  std::string text;
  for (const officev1::TextRun& run : runs) text += run.text();
  return text;
}

long long runs_length(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs) {
  long long total = 0;
  for (const officev1::TextRun& run : runs) total += run.char_length();
  return total;
}

// Sets item-level Formatting when every run agrees on the flags the item
// level models; mixed-format text keeps item formatting unset and is
// described run by run in the item's inline spans instead.
void set_uniform_formatting(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs,
    docv1::TextItemBase* base) {
  if (runs.empty()) return;
  bool bold = runs[0].weight() >= 150.0f;
  docv1::Script script = script_for(runs[0].escapement());
  for (const officev1::TextRun& run : runs) {
    if ((run.weight() >= 150.0f) != bold || run.italic() != runs[0].italic()
        || run.underline() != runs[0].underline()
        || run.strikethrough() != runs[0].strikethrough()
        || run.monospace() != runs[0].monospace()
        || run.small_caps() != runs[0].small_caps()
        || run.overline() != runs[0].overline()
        || script_for(run.escapement()) != script) {
      return;
    }
  }
  if (!bold && !runs[0].italic() && !runs[0].underline()
      && !runs[0].strikethrough() && !runs[0].monospace()
      && !runs[0].small_caps() && !runs[0].overline()
      && script == docv1::SCRIPT_UNSPECIFIED) {
    return;
  }
  docv1::Formatting* formatting = base->mutable_formatting();
  formatting->set_bold(bold);
  formatting->set_italic(runs[0].italic());
  formatting->set_underline(runs[0].underline());
  formatting->set_strikethrough(runs[0].strikethrough());
  formatting->set_monospace(runs[0].monospace());
  formatting->set_small_caps(runs[0].small_caps());
  formatting->set_overline(runs[0].overline());
  formatting->set_script(script);
}

// Sets the item-level hyperlink from the first link among the runs. Every
// link, this one included, also reaches the item as an InlineSpan carrying
// its own range, so nothing here has to record the rest.
void apply_run_hyperlinks(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs,
    docv1::TextItemBase* base) {
  for (const officev1::TextRun& run : runs) {
    if (run.hyperlink_url().empty()) continue;
    base->set_hyperlink(run.hyperlink_url());
    return;
  }
}

// Maps the office core's own statistic names onto the typed counters. A
// name with no counter of its own is left out rather than coerced into a
// neighbouring one.
void set_statistics(
    const google::protobuf::Map<std::string, int64_t>& statistics,
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

// A shape's identity, on whichever item the shape became. Every field is
// optional, so a shape that names nothing leaves the message empty rather
// than claiming defaults.
void set_shape_meta(const std::string& shape_type, const std::string& name,
                    docv1::ShapeMeta* out) {
  if (!shape_type.empty()) out->set_shape_type(shape_type);
  if (!name.empty()) out->set_name(name);
}

// A drawing shape's identity, including the paint order and the rotation
// the office core reports in hundredths of a degree.
void set_drawing_shape_meta(const officev1::DrawingShape& shape,
                            docv1::ShapeMeta* out) {
  set_shape_meta(shape.shape_type(), shape.name(), out);
  out->set_z_order(shape.z_order());
  if (shape.rotation() != 0) {
    out->set_rotation_degrees(static_cast<double>(shape.rotation()) / 100.0);
  }
}

// Attaches a shape's alt text to a picture. Title and description are two
// source strings and now have a slot each, so neither has to stand in for
// the other.
void set_alt_text(const std::string& title, const std::string& description,
                  docv1::PictureItem* picture) {
  if (!description.empty()) {
    picture->mutable_meta()->mutable_description()->set_text(description);
  }
  if (!title.empty()) {
    picture->mutable_meta()->set_accessibility_title(title);
  }
}

std::string column_name(int column) {
  std::string name;
  for (int c = column; c >= 0; c = c / 26 - 1) {
    name.insert(name.begin(), static_cast<char>('A' + c % 26));
  }
  return name;
}

// "B7" and "B7.1.2" both anchor at row 6, column 1: an office cell name
// starts with the base-grid cell it was split from, so a split cell still
// has a place in the grid. False when the name anchors nowhere.
bool anchor_of_cell_name(const std::string& name, int* row, int* column) {
  size_t pos = 0;
  long col = 0;
  while (pos < name.size()
         && std::isupper(static_cast<unsigned char>(name[pos]))) {
    col = col * 26 + (name[pos] - 'A' + 1);
    pos++;
  }
  if (pos == 0 || pos >= name.size()) return false;
  long row_number = 0;
  size_t digit = pos;
  for (; digit < name.size()
         && std::isdigit(static_cast<unsigned char>(name[digit]));
       digit++) {
    row_number = row_number * 10 + (name[digit] - '0');
  }
  if (digit == pos || row_number <= 0) return false;
  *row = static_cast<int>(row_number - 1);
  *column = static_cast<int>(col - 1);
  return true;
}

// A wire cell range as a grid span, naming its sheet on both corners so a
// span stays readable without the surrounding context.
void set_grid_span(const officev1::SheetRangeRef& range,
                   const std::string& sheet, docv1::GridSpan* out) {
  docv1::GridCell* start = out->mutable_start();
  start->set_row(range.start_row());
  start->set_col(range.start_column());
  docv1::GridCell* end = out->mutable_end();
  end->set_row(range.end_row());
  end->set_col(range.end_column());
  if (sheet.empty()) return;
  start->set_sheet(sheet);
  end->set_sheet(sheet);
}

}  // namespace

DoclingMapper::DoclingMapper() {
  document_.set_schema_name(kSchemaName);
  document_.set_version(kSchemaVersion);
  docv1::GroupItem* body = document_.mutable_body();
  body->set_self_ref("#/body");
  body->set_content_layer(docv1::CONTENT_LAYER_BODY);
  docv1::GroupItem* furniture = document_.mutable_furniture();
  furniture->set_self_ref("#/furniture");
  furniture->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
}

docv1::GroupItem* DoclingMapper::group_by_ref(const std::string& ref) {
  if (ref == "#/body") return document_.mutable_body();
  if (ref == "#/furniture") return document_.mutable_furniture();
  const std::string prefix = "#/groups/";
  if (ref.starts_with(prefix)) {
    int index = std::atoi(ref.c_str() + prefix.size());
    if (index >= 0 && index < document_.groups_size()) {
      return document_.mutable_groups(index);
    }
  }
  return document_.mutable_body();
}

void DoclingMapper::link_child(const std::string& parent_ref,
                               const std::string& child_ref) {
  // group_by_ref falls back to the body, so the form arenas are matched
  // here first; otherwise a field's children would silently land in the
  // body instead of under their field.
  if (!field_region_ref_.empty() && parent_ref == field_region_ref_
      && document_.field_regions_size() > 0) {
    document_.mutable_field_regions(0)->add_children()->set_ref(child_ref);
    return;
  }
  const std::string field_prefix = "#/field_items/";
  if (parent_ref.starts_with(field_prefix)) {
    int index = std::atoi(parent_ref.c_str() + field_prefix.size());
    if (index >= 0 && index < document_.field_items_size()) {
      document_.mutable_field_items(index)->add_children()->set_ref(child_ref);
      return;
    }
  }
  group_by_ref(parent_ref)->add_children()->set_ref(child_ref);
}

void DoclingMapper::ensure_form_arena() {
  if (!field_region_ref_.empty()) return;
  docv1::FieldRegionItem* region = document_.add_field_regions();
  region->set_self_ref("#/field_regions/0");
  region->mutable_parent()->set_ref("#/body");
  region->set_label(docv1::DOC_ITEM_LABEL_FIELD_REGION);
  region->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_collector_source(region->mutable_source());
  field_region_ref_ = region->self_ref();
  link_child("#/body", field_region_ref_);

  docv1::FormItem* form = document_.add_form_items();
  form->set_self_ref("#/form_items/0");
  form->mutable_parent()->set_ref("#/body");
  form->set_label(docv1::DOC_ITEM_LABEL_FORM);
  form->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_collector_source(form->mutable_source());
  form_item_ref_ = form->self_ref();
  link_child("#/body", form_item_ref_);
}

void DoclingMapper::stamp_collector_source(
    google::protobuf::RepeatedPtrField<docv1::SourceType>* source) {
  // Every item this mapper creates is attributable: additive merges with
  // other collectors' output rely on the tag to never collide silently.
  docv1::CollectorSource* collector = source->Add()->mutable_collector();
  collector->set_collector("libreoffice");
  collector->set_model("lok");
}

docv1::GroupItem* DoclingMapper::add_group(const std::string& parent_ref,
                                           docv1::GroupLabel label,
                                           const std::string& name,
                                           docv1::ContentLayer layer) {
  int index = document_.groups_size();
  docv1::GroupItem* group = document_.add_groups();
  group->set_self_ref("#/groups/" + std::to_string(index));
  group->mutable_parent()->set_ref(parent_ref);
  group->set_label(label);
  group->set_content_layer(layer);
  if (!name.empty()) group->set_name(name);
  link_child(parent_ref, group->self_ref());
  return group;
}

DoclingMapper::TextHandle DoclingMapper::add_text(TextKind kind,
                                                  docv1::DocItemLabel label,
                                                  docv1::ContentLayer layer,
                                                  const std::string& parent_ref) {
  TextHandle handle;
  handle.ref = "#/texts/" + std::to_string(document_.texts_size());
  handle.item = document_.add_texts();
  switch (kind) {
    case TextKind::kTitle:
      handle.base = handle.item->mutable_title()->mutable_base();
      break;
    case TextKind::kSectionHeader:
      handle.base = handle.item->mutable_section_header()->mutable_base();
      break;
    case TextKind::kList:
      handle.base = handle.item->mutable_list_item()->mutable_base();
      break;
    case TextKind::kFormula:
      handle.base = handle.item->mutable_formula()->mutable_base();
      break;
    case TextKind::kText:
      handle.base = handle.item->mutable_text()->mutable_base();
      break;
    case TextKind::kFieldHeading:
      handle.base = handle.item->mutable_field_heading()->mutable_base();
      break;
    case TextKind::kFieldValue:
      handle.base = handle.item->mutable_field_value()->mutable_base();
      break;
  }
  handle.base->set_self_ref(handle.ref);
  handle.base->mutable_parent()->set_ref(parent_ref);
  handle.base->set_label(label);
  handle.base->set_content_layer(layer);
  stamp_collector_source(handle.base->mutable_source());
  link_child(parent_ref, handle.ref);
  return handle;
}

docv1::PictureItem* DoclingMapper::add_picture(docv1::DocItemLabel label,
                                               docv1::ContentLayer layer,
                                               const std::string& parent_ref,
                                               std::string* ref_out) {
  std::string ref = "#/pictures/" + std::to_string(document_.pictures_size());
  docv1::PictureItem* picture = document_.add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent_ref);
  picture->set_label(label);
  picture->set_content_layer(layer);
  stamp_collector_source(picture->mutable_source());
  link_child(parent_ref, ref);
  if (ref_out != nullptr) *ref_out = ref;
  return picture;
}

docv1::TableItem* DoclingMapper::add_table(docv1::ContentLayer layer,
                                           const std::string& parent_ref,
                                           std::string* ref_out) {
  std::string ref = "#/tables/" + std::to_string(document_.tables_size());
  docv1::TableItem* table = document_.add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent_ref);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(layer);
  stamp_collector_source(table->mutable_source());
  link_child(parent_ref, ref);
  if (ref_out != nullptr) *ref_out = ref;
  return table;
}

void DoclingMapper::add_run_spans(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs,
    google::protobuf::RepeatedPtrField<docv1::InlineSpan>* spans,
    const std::string& owner_ref, long long base_offset) {
  long long local = base_offset;
  for (int index = 0; index < runs.size();) {
    const RunKey key = run_key(runs[index]);
    long long start = local;
    int end_index = index;
    // Adjacent runs agreeing on every attribute are one span: the office
    // core splits portions for reasons a reader never sees.
    while (end_index < runs.size() && run_key(runs[end_index]) == key) {
      local += runs[end_index].char_length();
      end_index++;
    }
    index = end_index;
    if (local <= start) continue;
    const std::string color = hex_color(key.color_rgb);
    const docv1::Script script = script_for(key.escapement);
    const bool language_differs =
        !key.language.empty() && key.language != document_language_;
    const bool formatted = key.bold || key.italic || key.underline
        || key.strikethrough || key.monospace || key.small_caps || key.overline
        || script != docv1::SCRIPT_UNSPECIFIED;
    // -1 is the office core's transparent value: a run with no highlight.
    const std::string highlight = key.highlight_rgb >= 0
        ? hex_color_always(static_cast<uint32_t>(key.highlight_rgb))
        : std::string();
    if (!formatted && key.font.empty() && key.size_pt <= 0 && color.empty()
        && !language_differs && key.hyperlink.empty()
        && key.field_code.empty() && key.char_style.empty()
        && highlight.empty()) {
      // Nothing the item does not already say; a span here would be noise.
      continue;
    }
    docv1::InlineSpan* span = spans->Add();
    span->mutable_range()->set_start(clamp32(start));
    span->mutable_range()->set_end(clamp32(local));
    if (formatted) {
      docv1::Formatting* formatting = span->mutable_formatting();
      formatting->set_bold(key.bold);
      formatting->set_italic(key.italic);
      formatting->set_underline(key.underline);
      formatting->set_strikethrough(key.strikethrough);
      formatting->set_monospace(key.monospace);
      formatting->set_small_caps(key.small_caps);
      formatting->set_overline(key.overline);
      formatting->set_script(script);
    }
    if (!key.char_style.empty()) span->set_style_name(key.char_style);
    if (!highlight.empty()) span->set_highlight_color(highlight);
    if (!key.font.empty()) span->set_font_family(key.font);
    if (key.size_pt > 0) span->set_font_size_pt(key.size_pt);
    if (!color.empty()) span->set_color(color);
    if (language_differs) span->set_language(key.language);
    if (!key.hyperlink.empty()) span->set_hyperlink(key.hyperlink);
    if (!key.field_code.empty()) span->set_field_code(key.field_code);
    if (!key.field_target.empty() && !owner_ref.empty()) {
      // The anchor the reference names may not have streamed yet, so the
      // target is filled in once the whole document has.
      pending_references_.push_back(
          {owner_ref, spans->size() - 1, key.field_target});
    }
  }
}

void DoclingMapper::register_embedded_object(
    const officev1::EmbeddedObject& object, const std::string& item_ref) {
  // An OLE payload is a nested document the fold does not open. Registering
  // it makes it addressable for a later pass instead of leaving its class
  // id as a bare string on a picture.
  docv1::SubDocumentRef* attachment = document_.add_attachments();
  attachment->set_id("object:" + std::to_string(attachment_index_++));
  attachment->set_name(object.name());
  attachment->set_media_type(object.replacement_mime_type());
  attachment->set_size_bytes(object.replacement_image().size());
  if (!item_ref.empty()) attachment->set_item_ref(item_ref);
  if (!object.clsid().empty()) attachment->set_class_id(object.clsid());
  std::string kind = officev1::EmbeddedObjectKind_Name(object.kind());
  const std::string prefix = "EMBEDDED_OBJECT_KIND_";
  if (kind.starts_with(prefix)) kind = kind.substr(prefix.size());
  std::ranges::transform(kind, kind.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  if (kind != "unspecified") attachment->set_kind(kind);
}

std::string DoclingMapper::sheet_label(int index) const {
  auto found = sheet_name_.find(index);
  return found != sheet_name_.end() ? found->second : std::string();
}

docv1::TextItemBase* DoclingMapper::text_by_ref(const std::string& ref) {
  const std::string prefix = "#/texts/";
  if (!ref.starts_with(prefix)) return nullptr;
  int index = std::atoi(ref.c_str() + prefix.size());
  if (index < 0 || index >= document_.texts_size()) return nullptr;
  docv1::BaseTextItem* item = document_.mutable_texts(index);
  switch (item->item_case()) {
    case docv1::BaseTextItem::kTitle:
      return item->mutable_title()->mutable_base();
    case docv1::BaseTextItem::kSectionHeader:
      return item->mutable_section_header()->mutable_base();
    case docv1::BaseTextItem::kListItem:
      return item->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kFormula:
      return item->mutable_formula()->mutable_base();
    case docv1::BaseTextItem::kText:
      return item->mutable_text()->mutable_base();
    case docv1::BaseTextItem::kFieldHeading:
      return item->mutable_field_heading()->mutable_base();
    case docv1::BaseTextItem::kFieldValue:
      return item->mutable_field_value()->mutable_base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

bool DoclingMapper::resolve_doc_span(long long start, long long end,
                                     docv1::FineRef* out) const {
  if (start < 0) return false;
  // The index is built in arrival order, which is ascending offset order,
  // so the item holding an offset is the last one starting at or before it.
  auto after = std::upper_bound(
      body_spans_.begin(), body_spans_.end(), start,
      [](long long value, const BodySpan& span) { return value < span.start; });
  if (after == body_spans_.begin()) return false;
  const BodySpan& span = *std::prev(after);
  if (start >= span.end && start != span.start) return false;
  out->set_ref(span.ref);
  long long local_start = start - span.start;
  long long local_end = (end > start ? end : start) - span.start;
  long long length = span.end - span.start;
  out->mutable_range()->set_start(clamp32(std::min(local_start, length)));
  out->mutable_range()->set_end(clamp32(std::min(local_end, length)));
  return true;
}

void DoclingMapper::resolve_anchors() {
  // Sheet names for the ranges that were declared before their sheet.
  for (const auto& [range_index, sheet_index] : pending_range_sheets_) {
    const std::string sheet = sheet_label(sheet_index);
    if (sheet.empty() || range_index >= document_.named_ranges_size()) continue;
    docv1::GridSpan* span =
        document_.mutable_named_ranges(range_index)->mutable_range();
    span->mutable_start()->set_sheet(sheet);
    span->mutable_end()->set_sheet(sheet);
  }
  // A reply points at the comment it answers, once that comment exists.
  for (const auto& [reply_ref, parent_name] : pending_comment_parents_) {
    auto parent = comment_ref_by_name_.find(parent_name);
    if (parent == comment_ref_by_name_.end()) continue;
    docv1::TextItemBase* base = text_by_ref(reply_ref);
    if (base == nullptr) continue;
    base->mutable_comment_meta()->mutable_parent()->set_ref(parent->second);
  }
  // Comments first: the item they annotate gains a back-link carrying the
  // annotated range in that item's own characters.
  for (const PendingComment& pending : pending_comments_) {
    docv1::FineRef anchor;
    if (!resolve_doc_span(pending.start, pending.end, &anchor)) continue;
    docv1::TextItemBase* base = text_by_ref(anchor.ref());
    if (base == nullptr) continue;
    docv1::FineRef* link = base->add_comments();
    link->set_ref(pending.ref);
    *link->mutable_range() = anchor.range();
  }
  for (const PendingFieldSpan& pending : pending_field_spans_) {
    if (pending.index >= document_.field_items_size()) continue;
    docv1::FineRef span;
    if (!resolve_doc_span(pending.start, pending.end, &span)) continue;
    *document_.mutable_field_items(pending.index)->mutable_span() = span;
  }
  for (const PendingChange& pending : pending_changes_) {
    if (pending.index >= document_.changes_size()) continue;
    docv1::ChangeRecord* change = document_.mutable_changes(pending.index);
    docv1::FineRef target;
    // An anchor in content the fold does not emit keeps its record and
    // loses only the target: an unanchored change is still evidence.
    if (resolve_doc_span(pending.start, pending.end, &target)) {
      *change->mutable_target() = target;
    }
  }
  std::map<std::string, docv1::FineRef> resolved_anchors;
  for (const PendingAnchor& pending : pending_anchors_) {
    docv1::NamedAnchor* anchor = document_.add_anchors();
    anchor->set_name(pending.name);
    docv1::FineRef target;
    if (resolve_doc_span(pending.start, pending.end, &target)) {
      *anchor->mutable_target() = target;
      resolved_anchors[pending.name] = target;
    }
  }
  // A cross-reference field points at a named anchor; now that every anchor
  // is placed, the field's span can point at the same item.
  for (const PendingReference& pending : pending_references_) {
    auto found = resolved_anchors.find(pending.target_name);
    if (found == resolved_anchors.end()) continue;
    docv1::TextItemBase* base = text_by_ref(pending.item_ref);
    if (base == nullptr || pending.span_index >= base->spans_size()) continue;
    *base->mutable_spans(pending.span_index)->mutable_target() = found->second;
  }
}

void DoclingMapper::add_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    int page_index, bool page_local, double l, double t, double r, double b,
    long long span_start, long long span_end) {
  if (page_index < 0) return;
  if (!page_local) {
    if (page_index < static_cast<int>(page_rects_.size())) {
      const officev1::PageRect& page = page_rects_[page_index];
      l -= static_cast<double>(page.x_twips());
      r -= static_cast<double>(page.x_twips());
      t -= static_cast<double>(page.y_twips());
      b -= static_cast<double>(page.y_twips());
    } else if (unresolved_prov_pages_.insert(page_index).second) {
      // No page rectangle to subtract, so the box stays document-absolute
      // while every emitted box claims to be page-local. The item keeps its
      // provenance (dropping it loses the page number too), but the fold
      // says so once per page rather than letting the consumer trust a
      // coordinate space that does not hold.
      warnings_.push_back(
          "page " + std::to_string(page_index + 1)
          + " has no known rectangle, so its provenance boxes stay "
            "document-absolute despite the page-local coordinate origin");
    }
  }
  docv1::ProvenanceItem* item = prov->Add();
  item->set_page_no(page_index + 1);
  docv1::BoundingBox* box = item->mutable_bbox();
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  item->mutable_charspan()->set_start(clamp32(span_start));
  item->mutable_charspan()->set_end(clamp32(span_end));
}

void DoclingMapper::add_line_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    const google::protobuf::RepeatedPtrField<officev1::LineBox>& lines,
    long long span_start, long long span_end) {
  for (const officev1::LineBox& line : lines) {
    // A line carrying measured character boundaries narrows its charspan to
    // its own characters, offset into the item's span space; unmeasured
    // lines keep the full item span. char_end must exceed char_start so the
    // -1 sentinel and a defaulted [0, 0) both fall back.
    long long start = span_start;
    long long end = span_end;
    if (line.char_start() >= 0 && line.char_end() > line.char_start()) {
      start = span_start + line.char_start();
      end = span_start + line.char_end();
      if (span_end > span_start) {
        start = std::min(start, span_end);
        end = std::min(end, span_end);
      }
    }
    add_prov(prov, line.page_index(), false,
             static_cast<double>(line.x_twips()),
             static_cast<double>(line.y_twips()),
             static_cast<double>(line.x_twips() + line.width_twips()),
             static_cast<double>(line.y_twips() + line.height_twips()),
             start, end);
  }
}

void DoclingMapper::add_caret_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    int page_index, const officev1::TwipsPoint& start,
    const officev1::TwipsPoint& end, long long span_start,
    long long span_end) {
  add_prov(prov, page_index, false,
           static_cast<double>(std::min(start.x(), end.x())),
           static_cast<double>(std::min(start.y(), end.y())),
           static_cast<double>(std::max(start.x(), end.x())),
           static_cast<double>(std::max(start.y(), end.y())),
           span_start, span_end);
}

bool DoclingMapper::cell_bbox(
    const google::protobuf::RepeatedPtrField<officev1::LineBox>& lines,
    docv1::BoundingBox* box) {
  // The union covers only the lines on the cell's first page: TableCell has
  // no page slot, so a cell straddling a page break keeps its first page's
  // extent.
  int page_index = -1;
  double l = 0, t = 0, r = 0, b = 0;
  bool first = true;
  for (const officev1::LineBox& line : lines) {
    if (line.page_index() < 0) continue;
    if (page_index < 0) page_index = line.page_index();
    if (line.page_index() != page_index) continue;
    double ll = static_cast<double>(line.x_twips());
    double lt = static_cast<double>(line.y_twips());
    double lr = ll + static_cast<double>(line.width_twips());
    double lb = lt + static_cast<double>(line.height_twips());
    if (first) {
      l = ll; t = lt; r = lr; b = lb;
      first = false;
    } else {
      l = std::min(l, ll);
      t = std::min(t, lt);
      r = std::max(r, lr);
      b = std::max(b, lb);
    }
  }
  if (first) return false;
  // Line rectangles are document-absolute like every LineBox; page-local
  // like add_prov.
  if (page_index < static_cast<int>(page_rects_.size())) {
    const officev1::PageRect& page = page_rects_[page_index];
    l -= static_cast<double>(page.x_twips());
    r -= static_cast<double>(page.x_twips());
    t -= static_cast<double>(page.y_twips());
    b -= static_cast<double>(page.y_twips());
  }
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  return true;
}

void DoclingMapper::fold_table(const officev1::TableData& table,
                               docv1::TableItem* item) {
  docv1::TableData* data = item->mutable_data();
  data->set_num_rows(table.rows());
  data->set_num_cols(table.columns());
  for (const officev1::TableCellData& cell : table.cells()) {
    // A split or merged office cell has no base-grid position of its own,
    // but its name anchors at one; placing it there with its merge spans is
    // what makes a merged table structurally readable. Only a name that
    // anchors nowhere has nowhere to go.
    int row = cell.row();
    int column = cell.column();
    if ((row < 0 || column < 0)
        && !anchor_of_cell_name(cell.name(), &row, &column)) {
      row = -1;
      column = -1;
    }
    if (row < 0 || column < 0) {
      (*item->mutable_meta()->mutable_custom_fields())["cell:" + cell.name()] =
          str_value(cell.text());
      continue;
    }
    int row_span = std::max(1, cell.row_span());
    int col_span = std::max(1, cell.column_span());
    docv1::TableCell* out = data->add_table_cells();
    out->set_start_row_offset_idx(row);
    out->set_end_row_offset_idx(row + row_span);
    out->set_start_col_offset_idx(column);
    out->set_end_col_offset_idx(column + col_span);
    out->set_row_span(row_span);
    out->set_col_span(col_span);
    out->set_text(cell.text());
    if (!cell.line_rects().empty()) {
      docv1::BoundingBox box;
      if (cell_bbox(cell.line_rects(), &box)) *out->mutable_bbox() = box;
    }
  }
  // 64-bit product: adversarial row and column counts must saturate the
  // guard, not overflow it into acceptance.
  if (table.rows() > 0 && table.columns() > 0 &&
      static_cast<int64_t>(table.rows()) * table.columns() <= kMaxGridCells) {
    for (int row = 0; row < table.rows(); row++) {
      docv1::TableRow* out_row = data->add_grid();
      for (int column = 0; column < table.columns(); column++) {
        docv1::TableCell* out = out_row->add_cells();
        out->set_start_row_offset_idx(row);
        out->set_end_row_offset_idx(row + 1);
        out->set_start_col_offset_idx(column);
        out->set_end_col_offset_idx(column + 1);
        out->set_row_span(1);
        out->set_col_span(1);
      }
    }
    // Several cells can anchor at one slot when a base cell was split, so
    // the first cell placed there keeps it: the base cell is emitted before
    // the pieces split out of it.
    std::set<std::pair<int, int>> filled;
    for (const docv1::TableCell& cell : data->table_cells()) {
      // Merged or irregular office tables can report cells beyond the
      // declared grid; those stay in table_cells but have no grid slot.
      if (cell.start_row_offset_idx() >= data->grid_size()) continue;
      docv1::TableRow* out_row = data->mutable_grid(cell.start_row_offset_idx());
      if (cell.start_col_offset_idx() >= out_row->cells_size()) continue;
      if (!filled.insert({cell.start_row_offset_idx(),
                          cell.start_col_offset_idx()}).second) {
        continue;
      }
      docv1::TableCell* slot =
          out_row->mutable_cells(cell.start_col_offset_idx());
      slot->set_text(cell.text());
      slot->set_row_span(cell.row_span());
      slot->set_col_span(cell.col_span());
      slot->set_end_row_offset_idx(cell.end_row_offset_idx());
      slot->set_end_col_offset_idx(cell.end_col_offset_idx());
      if (cell.has_value()) *slot->mutable_value() = cell.value();
      if (cell.has_bbox()) *slot->mutable_bbox() = cell.bbox();
    }
  }
}

void DoclingMapper::consume(const officev1::StreamPagesResponse& event) {
  switch (event.event_case()) {
    case officev1::StreamPagesResponse::kDocumentInfo:
      return on_document_info(event.document_info());
    case officev1::StreamPagesResponse::kPageImage:
      return on_page_image(event.page_image());
    case officev1::StreamPagesResponse::kStatus:
      return on_status(event.status());
    case officev1::StreamPagesResponse::kMetadata:
      return on_metadata(event.metadata());
    case officev1::StreamPagesResponse::kParagraph:
      return on_paragraph(event.paragraph());
    case officev1::StreamPagesResponse::kTable:
      return on_table(event.table());
    case officev1::StreamPagesResponse::kEmbeddedImage:
      return on_embedded_image(event.embedded_image());
    case officev1::StreamPagesResponse::kFootnote:
      return on_footnote(event.footnote());
    case officev1::StreamPagesResponse::kHeaderFooter:
      return on_header_footer(event.header_footer());
    case officev1::StreamPagesResponse::kPageStyle:
      return on_page_style(event.page_style());
    case officev1::StreamPagesResponse::kDocumentIndex:
      return on_document_index(event.document_index());
    case officev1::StreamPagesResponse::kDrawingShape:
      return on_drawing_shape(event.drawing_shape());
    case officev1::StreamPagesResponse::kSlide:
      return on_slide(event.slide());
    case officev1::StreamPagesResponse::kSlideShape:
      return on_slide_shape(event.slide_shape());
    case officev1::StreamPagesResponse::kTextFrame:
      return on_text_frame(event.text_frame());
    case officev1::StreamPagesResponse::kShape:
      return on_shape(event.shape());
    case officev1::StreamPagesResponse::kEmbeddedObject:
      return on_embedded_object(event.embedded_object());
    case officev1::StreamPagesResponse::kSheet:
      return on_sheet(event.sheet());
    case officev1::StreamPagesResponse::kSheetRow:
      return on_sheet_row(event.sheet_row());
    case officev1::StreamPagesResponse::kSheetNamedRange:
      return on_sheet_named_range(event.sheet_named_range());
    case officev1::StreamPagesResponse::kSheetDatabaseRange:
      return on_sheet_database_range(event.sheet_database_range());
    case officev1::StreamPagesResponse::kSheetCellComment:
      return on_sheet_cell_comment(event.sheet_cell_comment());
    case officev1::StreamPagesResponse::kSheetChart:
      return on_sheet_chart(event.sheet_chart());
    case officev1::StreamPagesResponse::kSheetPivotTable:
      return on_sheet_pivot_table(event.sheet_pivot_table());
    case officev1::StreamPagesResponse::kComment:
      return on_comment(event.comment());
    case officev1::StreamPagesResponse::kTrackedChange:
      return on_tracked_change(event.tracked_change());
    case officev1::StreamPagesResponse::kBookmark:
      return on_bookmark(event.bookmark());
    case officev1::StreamPagesResponse::kFormField:
      return on_form_field(event.form_field());
    case officev1::StreamPagesResponse::EVENT_NOT_SET:
      return;
  }
}

void DoclingMapper::on_document_info(const officev1::DocumentInfo& info) {
  document_type_ = info.document_type();
  page_rects_.assign(info.page_rects().begin(), info.page_rects().end());
  if (document_.name().empty() && !info.document_id().empty()) {
    document_.set_name(info.document_id());
  }
  docv1::DocumentOrigin* origin = document_.mutable_origin();
  origin->set_mimetype(mime_for_extension(info.source_format()));
  if (!info.document_id().empty()) origin->set_filename(info.document_id());
  for (int index = 0; index < info.page_rects_size(); index++) {
    const officev1::PageRect& rect = info.page_rects(index);
    docv1::PageItem* page = &(*document_.mutable_pages())[index + 1];
    page->set_page_no(index + 1);
    page->set_unit(kCoordinateUnit);
    page->mutable_size()->set_width(static_cast<double>(rect.width_twips()));
    page->mutable_size()->set_height(static_cast<double>(rect.height_twips()));
  }
}

void DoclingMapper::on_page_image(const officev1::PageImage& image) {
  docv1::PageItem* page = &(*document_.mutable_pages())[image.index() + 1];
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

void DoclingMapper::on_metadata(const officev1::DocumentMetadata& meta) {
  if (!meta.title().empty()) document_.set_name(meta.title());
  document_language_ = meta.language();
  // Everything a document records about itself has a typed slot, so nothing
  // here goes through a value map. Instants come off the wire as epoch
  // milliseconds and are converted, not re-rendered.
  docv1::DocumentMeta* source_meta = document_.mutable_source_meta();
  if (!meta.title().empty()) source_meta->set_title(meta.title());
  if (!meta.author().empty()) source_meta->add_authors(meta.author());
  if (meta.created_epoch_ms() != 0) {
    set_instant(meta.created_epoch_ms(), source_meta->mutable_created());
  }
  if (meta.modified_epoch_ms() != 0) {
    set_instant(meta.modified_epoch_ms(), source_meta->mutable_modified());
  }
  if (!meta.language().empty()) source_meta->set_language(meta.language());
  if (!meta.generator().empty()) source_meta->set_generator(meta.generator());
  if (!meta.subject().empty()) source_meta->set_subject(meta.subject());
  if (!meta.modified_by().empty()) {
    source_meta->set_modified_by(meta.modified_by());
  }
  if (meta.printed_epoch_ms() != 0) {
    set_instant(meta.printed_epoch_ms(), source_meta->mutable_printed());
  }
  if (!meta.printed_by().empty()) source_meta->set_printer(meta.printed_by());
  if (!meta.template_name().empty()) {
    source_meta->set_template_(meta.template_name());
  }
  if (meta.editing_cycles() != 0) {
    source_meta->set_editing_cycles(meta.editing_cycles());
  }
  if (meta.editing_duration_seconds() != 0) {
    source_meta->set_editing_duration_seconds(meta.editing_duration_seconds());
  }
  docv1::BaseMeta* body_meta = document_.mutable_body()->mutable_meta();
  for (const std::string& keyword : meta.keywords()) {
    source_meta->add_keywords(keyword);
    body_meta->mutable_keywords()->add_values(keyword);
  }
  if (!meta.statistics().empty()) {
    set_statistics(meta.statistics(), source_meta->mutable_statistics());
  }
  for (const officev1::UserProperty& prop : meta.user_properties()) {
    docv1::UserProperty* out = source_meta->add_user_properties();
    out->set_name(prop.name());
    switch (prop.value_case()) {
      case officev1::UserProperty::kText:
        out->set_text(prop.text());
        break;
      case officev1::UserProperty::kNumber:
        out->set_number(prop.number());
        break;
      case officev1::UserProperty::kFlag:
        out->set_boolean(prop.flag());
        break;
      case officev1::UserProperty::kEpochMs:
        set_instant(prop.epoch_ms(), out->mutable_instant());
        break;
      case officev1::UserProperty::VALUE_NOT_SET:
        // A property the office core stored in a type this wire has no arm
        // for keeps its name and no value.
        break;
    }
  }
  if (!meta.language().empty()) {
    docv1::LanguageMetaField* language = body_meta->mutable_language();
    language->set_code_raw(meta.language());
    std::string subtag = meta.language().substr(0, meta.language().find('-'));
    std::ranges::transform(subtag, subtag.begin(),
                           [](unsigned char c) { return std::toupper(c); });
    docv1::HumanLanguageLabel code;
    if (docv1::HumanLanguageLabel_Parse("HUMAN_LANGUAGE_LABEL_" + subtag,
                                        &code)) {
      language->set_code(code);
    }
  }
}

void DoclingMapper::on_status(const officev1::RenderStatus& status) {
  for (const std::string& warning : status.warnings()) {
    warnings_.push_back(warning);
  }
  // Anchors resolve only once the whole body has streamed past: a comment
  // can close before the paragraph it sits in is emitted, and a
  // cross-reference can name an anchor from a later page.
  resolve_anchors();
  resolve_page_styles();
  finished_ = true;
}

void DoclingMapper::resolve_page_styles() {
  // Nothing to resolve against when the page style catalogue was not part
  // of the request; the names on the pages stay as the layout reported
  // them.
  if (document_.page_styles_size() == 0) return;
  for (int page_no : styled_pages_) {
    auto found = document_.pages().find(page_no);
    if (found == document_.pages().end()) continue;
    const std::string& name = found->second.style_name();
    bool declared = false;
    for (const docv1::PageStyle& style : document_.page_styles()) {
      if (style.name() == name) {
        declared = true;
        break;
      }
    }
    if (!declared) {
      warnings_.push_back("page " + std::to_string(page_no)
                          + " names page style \"" + name
                          + "\", which the style catalogue does not declare");
    }
  }
}

void DoclingMapper::on_paragraph(const officev1::Paragraph& paragraph) {
  std::string text = concat_runs(paragraph.runs());
  long long length = runs_length(paragraph.runs());
  // Provenance charspans are 0-indexed within the item's own text; the
  // document-absolute paragraph offset stays on the office wire only.
  const long long span_start = 0;
  const long long span_end = length;
  TextHandle handle;
  if (paragraph.style() == "Title") {
    handle = add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  } else if (paragraph.outline_level() >= 1) {
    handle = add_text(TextKind::kSectionHeader,
                      docv1::DOC_ITEM_LABEL_SECTION_HEADER,
                      docv1::CONTENT_LAYER_BODY, "#/body");
    handle.item->mutable_section_header()->set_level(paragraph.outline_level());
  } else if (paragraph.list_level() >= 0) {
    handle = add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  } else {
    handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  }
  handle.base->set_text(text);
  handle.base->set_orig(text);
  if (!paragraph.style().empty()) {
    handle.base->set_style_name(paragraph.style());
  }
  set_uniform_formatting(paragraph.runs(), handle.base);
  add_run_spans(paragraph.runs(), handle.base->mutable_spans(), handle.ref);
  apply_run_hyperlinks(paragraph.runs(), handle.base);
  // The paragraph's extent in the document-absolute character space, which
  // is where comments, tracked changes, and bookmarks anchor.
  if (paragraph.char_offset() >= 0) {
    body_spans_.push_back(
        {paragraph.char_offset(), paragraph.char_offset() + length,
         handle.ref});
  }
  if (!paragraph.line_rects().empty()) {
    add_line_prov(handle.base->mutable_prov(), paragraph.line_rects(),
                  span_start, span_end);
  } else {
    add_caret_prov(handle.base->mutable_prov(), paragraph.page_index(),
                   paragraph.start(), paragraph.end(), span_start, span_end);
  }
}

void DoclingMapper::on_table(const officev1::TableData& table) {
  docv1::TableItem* item = add_table(docv1::CONTENT_LAYER_BODY, "#/body",
                                     nullptr);
  fold_table(table, item);
  if (!table.line_rects().empty()) {
    add_line_prov(item->mutable_prov(), table.line_rects(), 0, 0);
  } else {
    add_caret_prov(item->mutable_prov(), table.page_index(), table.start(),
                   table.end(), 0, 0);
  }
}

void DoclingMapper::on_embedded_image(const officev1::EmbeddedImage& image) {
  std::string parent = "#/body";
  // A slide picture belongs under its slide; its geometry is already
  // page-local, unlike a text document's document-absolute anchors.
  bool page_local = false;
  if (document_type_ == "presentation") {
    page_local = true;
    if (auto slide = slide_group_.find(image.page_index());
        slide != slide_group_.end()) {
      parent = slide->second;
    }
  } else if (auto container = writer_groups_.find(image.group_path());
             container != writer_groups_.end()) {
    parent = container->second;
  }
  docv1::PictureItem* picture = add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, docv1::CONTENT_LAYER_BODY, parent,
      nullptr);
  if (!image.name().empty()) picture->mutable_shape()->set_name(image.name());
  set_alt_text(image.title(), image.description(), picture);
  if (!image.data().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(image.mime_type());
    ref->mutable_size()->set_width(static_cast<double>(image.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(image.height_twips()));
    ref->set_uri(data_uri(image.mime_type(), image.data()));
  }
  if (image.has_anchor()) {
    add_prov(picture->mutable_prov(), image.page_index(), page_local,
             static_cast<double>(image.anchor().x()),
             static_cast<double>(image.anchor().y()),
             static_cast<double>(image.anchor().x() + image.width_twips()),
             static_cast<double>(image.anchor().y() + image.height_twips()),
             0, 0);
  } else if (!image.line_rects().empty()) {
    add_line_prov(picture->mutable_prov(), image.line_rects(), 0, 0);
  }
}

void DoclingMapper::on_footnote(const officev1::Footnote& footnote) {
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_FOOTNOTE,
                               docv1::CONTENT_LAYER_BODY, "#/body");
  std::string text = concat_runs(footnote.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  docv1::FootnoteMeta* note = handle.base->mutable_footnote_meta();
  if (!footnote.label().empty()) note->set_label(footnote.label());
  note->set_endnote(footnote.endnote());
  set_uniform_formatting(footnote.runs(), handle.base);
  add_run_spans(footnote.runs(), handle.base->mutable_spans(), handle.ref);
  apply_run_hyperlinks(footnote.runs(), handle.base);
  add_caret_prov(handle.base->mutable_prov(), footnote.page_index(),
                 footnote.anchor(), footnote.anchor(), 0,
                 runs_length(footnote.runs()));
}

void DoclingMapper::on_header_footer(const officev1::HeaderFooter& block) {
  docv1::DocItemLabel label = block.footer()
      ? docv1::DOC_ITEM_LABEL_PAGE_FOOTER
      : docv1::DOC_ITEM_LABEL_PAGE_HEADER;
  for (const officev1::Paragraph& paragraph : block.paragraphs()) {
    TextHandle handle = add_text(TextKind::kText, label,
                                 docv1::CONTENT_LAYER_FURNITURE, "#/furniture");
    std::string text = concat_runs(paragraph.runs());
    handle.base->set_text(text);
    handle.base->set_orig(text);
    if (!paragraph.style().empty()) {
      handle.base->set_style_name(paragraph.style());
    }
    set_uniform_formatting(paragraph.runs(), handle.base);
    add_run_spans(paragraph.runs(), handle.base->mutable_spans(), handle.ref);
    apply_run_hyperlinks(paragraph.runs(), handle.base);
    (*handle.base->mutable_meta()->mutable_custom_fields())["page_style"] =
        str_value(block.page_style());
  }
}

void DoclingMapper::on_page_style(const officev1::PageStyleInfo& style) {
  // Page styles are named declarations of the document, in the same twips
  // every other measurement here uses.
  docv1::PageStyle* out = document_.add_page_styles();
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

void DoclingMapper::on_document_index(const officev1::DocumentIndex& index) {
  TextHandle handle = add_text(TextKind::kText,
                               docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX,
                               docv1::CONTENT_LAYER_BODY, "#/body");
  std::string text = concat_runs(index.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  docv1::IndexMeta* attribution = handle.base->mutable_index_meta();
  if (!index.type().empty()) attribution->set_service(index.type());
  if (!index.title().empty()) attribution->set_title(index.title());
  set_uniform_formatting(index.runs(), handle.base);
  add_run_spans(index.runs(), handle.base->mutable_spans(), handle.ref);
  apply_run_hyperlinks(index.runs(), handle.base);
  add_caret_prov(handle.base->mutable_prov(), index.page_index(),
                 index.anchor(), index.anchor(), 0, runs_length(index.runs()));
}

void DoclingMapper::on_drawing_shape(const officev1::DrawingShape& shape) {
  std::string parent = "#/body";
  if (auto container = draw_groups_.find({shape.page_index(), shape.group_path()});
      container != draw_groups_.end()) parent = container->second;
  double l = static_cast<double>(shape.position().x());
  double t = static_cast<double>(shape.position().y());
  double r = l + static_cast<double>(shape.width_twips());
  double b = t + static_cast<double>(shape.height_twips());
  if (shape.is_group()) {
    docv1::GroupItem* group = add_group(parent,
                                        docv1::GROUP_LABEL_PICTURE_AREA,
                                        shape.name(),
                                        docv1::CONTENT_LAYER_BODY);
    std::string child_path = shape.group_path().empty()
        ? std::to_string(shape.z_order())
        : shape.group_path() + "/" + std::to_string(shape.z_order());
    draw_groups_[{shape.page_index(), child_path}] = group->self_ref();
    return;
  }
  if (shape.has_text()) {
    TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                                 docv1::CONTENT_LAYER_BODY, parent);
    std::string text = concat_runs(shape.runs());
    handle.base->set_text(text);
    handle.base->set_orig(text);
    set_uniform_formatting(shape.runs(), handle.base);
    add_run_spans(shape.runs(), handle.base->mutable_spans(), handle.ref);
    apply_run_hyperlinks(shape.runs(), handle.base);
    set_drawing_shape_meta(shape, handle.base->mutable_shape());
    // Draw positions are page-local per part.
    add_prov(handle.base->mutable_prov(), shape.page_index(), true, l, t, r, b,
             0, runs_length(shape.runs()));
    return;
  }
  docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_PICTURE,
                                            docv1::CONTENT_LAYER_BODY, parent,
                                            nullptr);
  set_drawing_shape_meta(shape, picture->mutable_shape());
  set_alt_text(shape.title(), shape.description(), picture);
  add_prov(picture->mutable_prov(), shape.page_index(), true, l, t, r, b, 0, 0);
}

void DoclingMapper::on_slide(const officev1::Slide& slide) {
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SLIDE,
                                      slide.name(), docv1::CONTENT_LAYER_BODY);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  (*fields)["layout"] = num_value(slide.layout());
  if (!slide.master_page_name().empty()) {
    (*fields)["master_page_name"] = str_value(slide.master_page_name());
  }
  slide_group_[slide.index()] = group->self_ref();
}

void DoclingMapper::on_slide_shape(const officev1::SlideShape& shape) {
  if (shape.is_empty_placeholder()) return;
  std::string parent = "#/body";
  if (auto group = slide_group_.find(shape.slide_index());
      group != slide_group_.end()) parent = group->second;
  docv1::ContentLayer layer = shape.notes() ? docv1::CONTENT_LAYER_NOTES
                                            : docv1::CONTENT_LAYER_BODY;
  // Notes shapes carry no slide-page provenance: their geometry is in
  // notes-page space, which has no PageImage.
  int prov_page = shape.notes() ? -1 : shape.slide_index();
  double l = static_cast<double>(shape.position().x());
  double t = static_cast<double>(shape.position().y());
  double r = l + static_cast<double>(shape.width_twips());
  double b = t + static_cast<double>(shape.height_twips());

  // A table shape carries its content in a cell grid, not in shape text, so
  // it folds into a real table under the slide rather than a placeholder.
  if (shape.has_table()) {
    docv1::TableItem* item = add_table(layer, parent, nullptr);
    fold_table(shape.table(), item);
    add_prov(item->mutable_prov(), prov_page, true, l, t, r, b, 0, 0);
    return;
  }

  bool has_text = false;
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (!paragraph.runs().empty()) has_text = true;
  }
  if (!has_text) {
    if (ends_with(shape.shape_type(), "GraphicObjectShape")
        || ends_with(shape.shape_type(), "OLE2Shape")
        || ends_with(shape.shape_type(), "TableShape")
        || ends_with(shape.shape_type(), "MediaShape")) {
      docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_PICTURE,
                                                layer, parent, nullptr);
      docv1::ShapeMeta* shape_meta = picture->mutable_shape();
      set_shape_meta(shape.shape_type(), std::string(), shape_meta);
      shape_meta->set_z_order(shape.z_order());
      set_alt_text(shape.title(), shape.description(), picture);
      add_prov(picture->mutable_prov(), prov_page, true, l, t, r, b, 0, 0);
    }
    return;
  }

  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_OUTLINE) {
    // Outline placeholders keep their per-paragraph depth: top-level lines
    // become section headers, deeper lines list items.
    for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
      if (paragraph.runs().empty()) continue;
      TextHandle handle;
      if (paragraph.outline_depth() == 0) {
        handle = add_text(TextKind::kSectionHeader,
                          docv1::DOC_ITEM_LABEL_SECTION_HEADER, layer, parent);
        handle.item->mutable_section_header()->set_level(1);
      } else {
        handle = add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                          layer, parent);
      }
      std::string text = concat_runs(paragraph.runs());
      handle.base->set_text(text);
      handle.base->set_orig(text);
      set_uniform_formatting(paragraph.runs(), handle.base);
      add_run_spans(paragraph.runs(), handle.base->mutable_spans(), handle.ref);
      apply_run_hyperlinks(paragraph.runs(), handle.base);
      set_shape_meta(shape.shape_type(), std::string(),
                     handle.base->mutable_shape());
      handle.base->mutable_shape()->set_z_order(shape.z_order());
      add_prov(handle.base->mutable_prov(), prov_page, true, l, t, r, b, 0,
               runs_length(paragraph.runs()));
    }
    return;
  }

  TextHandle handle;
  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_TITLE) {
    handle = add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE, layer,
                      parent);
  } else {
    handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, layer,
                      parent);
  }
  std::string text;
  long long length = 0;
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (!text.empty()) {
      text += "\n";
      length += 1;
    }
    text += concat_runs(paragraph.runs());
    // Spans stay aligned with the joined text, newline separators included.
    add_run_spans(paragraph.runs(), handle.base->mutable_spans(), handle.ref,
                  length);
    length += runs_length(paragraph.runs());
  }
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_shape_meta(shape.shape_type(), std::string(),
                 handle.base->mutable_shape());
  handle.base->mutable_shape()->set_z_order(shape.z_order());
  add_prov(handle.base->mutable_prov(), prov_page, true, l, t, r, b, 0, length);
}

void DoclingMapper::on_text_frame(const officev1::TextFrame& frame) {
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_UNSPECIFIED,
                                      frame.name(), docv1::CONTENT_LAYER_BODY);
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_BODY, group->self_ref());
  // The frame's identity, chain included, belongs on the item that carries
  // its text: reading order across a chain resolves by frame name.
  docv1::ShapeMeta* shape_meta = handle.base->mutable_shape();
  set_shape_meta(std::string(), frame.name(), shape_meta);
  if (!frame.chain_next().empty()) {
    shape_meta->set_chain_next(frame.chain_next());
  }
  if (!frame.chain_prev().empty()) {
    shape_meta->set_chain_prev(frame.chain_prev());
  }
  std::string text = concat_runs(frame.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(frame.runs(), handle.base);
  add_run_spans(frame.runs(), handle.base->mutable_spans(), handle.ref);
  apply_run_hyperlinks(frame.runs(), handle.base);
  if (frame.has_anchor()) {
    add_prov(handle.base->mutable_prov(), frame.page_index(), false,
             static_cast<double>(frame.anchor().x()),
             static_cast<double>(frame.anchor().y()),
             static_cast<double>(frame.anchor().x() + frame.width_twips()),
             static_cast<double>(frame.anchor().y() + frame.height_twips()),
             0, runs_length(frame.runs()));
  }
}

int DoclingMapper::page_for_point(double x, double y) const {
  for (int index = 0; index < static_cast<int>(page_rects_.size()); index++) {
    const officev1::PageRect& page = page_rects_[index];
    if (x >= static_cast<double>(page.x_twips())
        && x < static_cast<double>(page.x_twips() + page.width_twips())
        && y >= static_cast<double>(page.y_twips())
        && y < static_cast<double>(page.y_twips() + page.height_twips())) {
      return index;
    }
  }
  return -1;
}

void DoclingMapper::on_shape(const officev1::Shape& shape) {
  std::string parent = "#/body";
  if (auto container = writer_groups_.find(shape.group_path());
      container != writer_groups_.end()) parent = container->second;

  if (shape.is_group()) {
    // The group's own shape type is always the office core's group shape,
    // which GROUP_LABEL_PICTURE_AREA already says.
    docv1::GroupItem* group = add_group(parent,
                                        docv1::GROUP_LABEL_PICTURE_AREA,
                                        shape.name(),
                                        docv1::CONTENT_LAYER_BODY);
    std::string child_path = shape.group_path().empty()
        ? std::to_string(shape.z_order())
        : shape.group_path() + "/" + std::to_string(shape.z_order());
    writer_groups_[child_path] = group->self_ref();
    return;
  }

  docv1::GroupItem* group = add_group(parent, docv1::GROUP_LABEL_UNSPECIFIED,
                                      shape.name(), docv1::CONTENT_LAYER_BODY);
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_BODY, group->self_ref());
  docv1::ShapeMeta* shape_meta = handle.base->mutable_shape();
  set_shape_meta(shape.shape_type(), shape.name(), shape_meta);
  shape_meta->set_z_order(shape.z_order());
  if (!shape.chain_next().empty()) {
    shape_meta->set_chain_next(shape.chain_next());
  }
  if (!shape.chain_prev().empty()) {
    shape_meta->set_chain_prev(shape.chain_prev());
  }
  std::string text = concat_runs(shape.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(shape.runs(), handle.base);
  add_run_spans(shape.runs(), handle.base->mutable_spans(), handle.ref);
  apply_run_hyperlinks(shape.runs(), handle.base);
  if (shape.has_anchor()) {
    add_prov(handle.base->mutable_prov(), shape.page_index(), false,
             static_cast<double>(shape.anchor().x()),
             static_cast<double>(shape.anchor().y()),
             static_cast<double>(shape.anchor().x() + shape.width_twips()),
             static_cast<double>(shape.anchor().y() + shape.height_twips()),
             0, runs_length(shape.runs()));
  } else if (shape.has_position()) {
    // Group children carry a model position instead of a caret anchor; the
    // page resolves from the position, which shares the document-absolute
    // space of the page rectangles.
    double l = static_cast<double>(shape.position().x());
    double t = static_cast<double>(shape.position().y());
    double r = l + static_cast<double>(shape.width_twips());
    double b = t + static_cast<double>(shape.height_twips());
    add_prov(handle.base->mutable_prov(),
             page_for_point((l + r) / 2, (t + b) / 2), false, l, t, r, b,
             0, runs_length(shape.runs()));
  }
}

void DoclingMapper::on_embedded_object(const officev1::EmbeddedObject& object) {
  // Geometry: Writer text-anchored objects carry a document-absolute caret
  // anchor; draw-page objects carry a page-local position.
  bool page_local = !object.has_anchor();
  double l;
  double t;
  if (object.has_anchor()) {
    l = static_cast<double>(object.anchor().x());
    t = static_cast<double>(object.anchor().y());
  } else {
    l = static_cast<double>(object.position().x());
    t = static_cast<double>(object.position().y());
  }
  double r = l + static_cast<double>(object.width_twips());
  double b = t + static_cast<double>(object.height_twips());

  if (object.kind() == officev1::EMBEDDED_OBJECT_KIND_FORMULA) {
    TextHandle handle = add_text(TextKind::kFormula,
                                 docv1::DOC_ITEM_LABEL_FORMULA,
                                 docv1::CONTENT_LAYER_BODY, "#/body");
    handle.base->set_text(object.formula());
    handle.base->set_orig(object.formula());
    register_embedded_object(object, handle.ref);
    add_prov(handle.base->mutable_prov(), object.page_index(), page_local, l, t,
             r, b, 0, static_cast<long long>(object.formula().size()));
    return;
  }

  if (object.kind() == officev1::EMBEDDED_OBJECT_KIND_SPREADSHEET) {
    std::string table_ref;
    docv1::TableItem* item = add_table(docv1::CONTENT_LAYER_BODY, "#/body",
                                       &table_ref);
    fold_table(object.inner_table(), item);
    register_embedded_object(object, table_ref);
    add_prov(item->mutable_prov(), object.page_index(), page_local, l, t, r, b,
             0, 0);
    return;
  }

  bool is_chart = object.kind() == officev1::EMBEDDED_OBJECT_KIND_CHART;
  std::string picture_ref;
  docv1::PictureItem* picture = add_picture(
      is_chart ? docv1::DOC_ITEM_LABEL_CHART : docv1::DOC_ITEM_LABEL_PICTURE,
      docv1::CONTENT_LAYER_BODY, "#/body", &picture_ref);
  if (!object.name().empty()) picture->mutable_shape()->set_name(object.name());
  register_embedded_object(object, picture_ref);
  if (!object.replacement_image().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(object.replacement_mime_type());
    ref->mutable_size()->set_width(static_cast<double>(object.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(object.height_twips()));
    ref->set_uri(data_uri(object.replacement_mime_type(),
                          object.replacement_image()));
  }
  add_prov(picture->mutable_prov(), object.page_index(), page_local, l, t, r, b,
           0, 0);
  if (!is_chart) return;

  const officev1::EmbeddedChart& chart = object.chart();
  const auto& series = chart.series();
  switch (chart.kind()) {
    case officev1::EMBEDDED_CHART_KIND_BAR:
    case officev1::EMBEDDED_CHART_KIND_COLUMN: {
      if (series.empty()) break;
      docv1::PictureBarChartData* bars =
          picture->add_annotations()->mutable_bar_chart();
      bars->set_kind("bar_chart_data");
      bars->set_title(chart.title());
      bars->set_x_axis_label(chart.x_axis_title());
      bars->set_y_axis_label(chart.y_axis_title());
      for (int i = 0; i < series[0].values_y_size(); i++) {
        docv1::ChartBar* bar = bars->add_bars();
        bar->set_label(i < chart.categories_size() ? chart.categories(i)
                                                   : std::to_string(i + 1));
        bar->set_values(series[0].values_y(i));
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_LINE:
    case officev1::EMBEDDED_CHART_KIND_AREA: {
      docv1::PictureLineChartData* lines =
          picture->add_annotations()->mutable_line_chart();
      lines->set_kind("line_chart_data");
      lines->set_title(chart.title());
      lines->set_x_axis_label(chart.x_axis_title());
      lines->set_y_axis_label(chart.y_axis_title());
      for (const officev1::EmbeddedChartSeries& one : series) {
        docv1::ChartLine* line = lines->add_lines();
        line->set_label(one.label());
        for (int i = 0; i < one.values_y_size(); i++) {
          docv1::FloatPair* pair = line->add_values();
          pair->set_first(i < one.values_x_size() ? one.values_x(i)
                                                  : static_cast<double>(i));
          pair->set_second(one.values_y(i));
        }
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_PIE: {
      if (series.empty()) break;
      docv1::PicturePieChartData* pie =
          picture->add_annotations()->mutable_pie_chart();
      pie->set_kind("pie_chart_data");
      pie->set_title(chart.title());
      for (int i = 0; i < series[0].values_y_size(); i++) {
        docv1::ChartSlice* slice = pie->add_slices();
        slice->set_label(i < chart.categories_size() ? chart.categories(i)
                                                     : std::to_string(i + 1));
        slice->set_value(series[0].values_y(i));
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_SCATTER:
    case officev1::EMBEDDED_CHART_KIND_BUBBLE: {
      docv1::PictureScatterChartData* scatter =
          picture->add_annotations()->mutable_scatter_chart();
      scatter->set_kind("scatter_chart_data");
      scatter->set_title(chart.title());
      scatter->set_x_axis_label(chart.x_axis_title());
      scatter->set_y_axis_label(chart.y_axis_title());
      for (const officev1::EmbeddedChartSeries& one : series) {
        int points = std::min(one.values_x_size(), one.values_y_size());
        for (int i = 0; i < points; i++) {
          docv1::FloatPair* pair =
              scatter->add_points()->mutable_value();
          pair->set_first(one.values_x(i));
          pair->set_second(one.values_y(i));
        }
      }
      break;
    }
    default:
      break;
  }
  // The tabular projection is always attached: any chart family stays
  // representable, including kinds with no typed variant above.
  docv1::PictureTabularChartData* tabular =
      picture->add_annotations()->mutable_tabular_chart();
  tabular->set_kind("tabular_chart_data");
  tabular->set_title(chart.title());
  docv1::TableItem scratch;
  fold_table(chart.tabular(), &scratch);
  *tabular->mutable_chart_data() = scratch.data();
}

void DoclingMapper::on_sheet(const officev1::Sheet& sheet) {
  docv1::ContentLayer layer = sheet.visible()
      ? docv1::CONTENT_LAYER_BODY
      : docv1::CONTENT_LAYER_INVISIBLE;
  sheet_layer_[sheet.index()] = layer;
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SHEET,
                                      sheet.name(), layer);
  docv1::SheetMeta* attributes = group->mutable_sheet();
  attributes->set_index(sheet.index());
  attributes->set_visible(sheet.visible());
  if (sheet.tab_color_rgb() >= 0) {
    attributes->set_tab_color(
        hex_color(static_cast<uint32_t>(sheet.tab_color_rgb())));
  }
  for (const officev1::SheetRangeRef& area : sheet.print_areas()) {
    set_grid_span(area, sheet.name(), attributes->add_print_areas());
  }
  sheet_group_[sheet.index()] = group->self_ref();
  sheet_name_[sheet.index()] = sheet.name();

  // The sheet's cell grid folds into one TableItem in absolute row and
  // column offsets, so cell addresses survive the mapping.
  sheet_table_[sheet.index()] = document_.tables_size();
  docv1::TableItem* table = add_table(layer, group->self_ref(), nullptr);
  docv1::TableData* data = table->mutable_data();
  data->set_num_rows(sheet.used_end_row() + 1);
  data->set_num_cols(sheet.used_end_column() + 1);
  // The sheet's columns are its schema: each one names its spreadsheet
  // column and carries the declared width in the page unit.
  for (int column = 0; column < sheet.column_widths_twips_size(); column++) {
    docv1::TableColumnSchema* schema = data->add_columns();
    schema->set_name(column_name(column));
    schema->set_width(
        static_cast<double>(sheet.column_widths_twips(column)));
  }
  add_prov(table->mutable_prov(), sheet.index(), true, 0, 0, 0, 0, 0, 0);
  if (table->prov_size() > 0 && !sheet.name().empty()) {
    table->mutable_prov(0)->mutable_grid()->set_sheet(sheet.name());
  }
}

void DoclingMapper::on_sheet_row(const officev1::SheetRow& row) {
  auto found = sheet_table_.find(row.sheet_index());
  if (found == sheet_table_.end()) return;
  docv1::TableItem* table = document_.mutable_tables(found->second);
  docv1::TableData* data = table->mutable_data();
  // One provenance entry per used row, locating it in the sheet grid; the
  // cells themselves carry no provenance slot.
  if (!row.cells().empty()) {
    docv1::ProvenanceItem* row_prov = data->add_row_prov();
    row_prov->set_page_no(row.sheet_index() + 1);
    docv1::GridCell* grid = row_prov->mutable_grid();
    grid->set_row(row.row());
    grid->set_col(row.cells(0).column());
    if (auto name = sheet_name_.find(row.sheet_index());
        name != sheet_name_.end()) {
      grid->set_sheet(name->second);
    }
  }
  for (const officev1::SheetCell& cell : row.cells()) {
    docv1::TableCell* out = data->add_table_cells();
    out->set_start_row_offset_idx(row.row());
    out->set_end_row_offset_idx(row.row() + std::max(1, cell.merged_rows()));
    out->set_start_col_offset_idx(cell.column());
    out->set_end_col_offset_idx(cell.column()
                                + std::max(1, cell.merged_columns()));
    out->set_row_span(std::max(1, cell.merged_rows()));
    out->set_col_span(std::max(1, cell.merged_columns()));
    out->set_text(cell.display());
    // The cell's typed value: text stays the display string, and the value
    // says what the sheet actually holds. An error beats a formula, because
    // a formula that failed has no value to report.
    docv1::CellValue value;
    bool typed = true;
    if (cell.error_code() != 0) {
      value.set_error(cell.display().empty()
                          ? "Err:" + std::to_string(cell.error_code())
                          : cell.display());
    } else if (cell.type() == officev1::SHEET_CELL_TYPE_FORMULA) {
      value.set_formula(cell.formula());
    } else if (cell.is_boolean()) {
      value.set_boolean(cell.number() != 0);
    } else if (cell.is_datetime()) {
      // A spreadsheet date is a wall-clock value; it stays one.
      docv1::CivilDateTime* when = value.mutable_datetime();
      when->set_year(cell.datetime().year());
      when->set_month(cell.datetime().month());
      when->set_day(cell.datetime().day());
      when->set_hour(cell.datetime().hour());
      when->set_minute(cell.datetime().minute());
      when->set_second(cell.datetime().second());
    } else if (cell.type() == officev1::SHEET_CELL_TYPE_VALUE) {
      value.set_number(cell.number());
    } else {
      typed = false;
    }
    if (!cell.number_format_string().empty()) {
      value.set_number_format(cell.number_format_string());
      typed = true;
    }
    if (typed) *out->mutable_value() = value;
  }
}

void DoclingMapper::on_sheet_named_range(
    const officev1::SheetNamedRange& range) {
  docv1::NamedRange* out = document_.add_named_ranges();
  out->set_name(range.name());
  out->set_kind("named");
  // A name pointing at cells resolves to a span; a name holding an
  // expression has no rectangle and keeps only its name. Workbook-scoped
  // names arrive before any sheet header, so the sheet is named later.
  if (range.has_range()) {
    set_grid_span(range.range(), std::string(), out->mutable_range());
    if (range.sheet_index() >= 0) {
      pending_range_sheets_.emplace_back(document_.named_ranges_size() - 1,
                                         range.sheet_index());
    }
  } else if (!range.content().empty()) {
    out->set_expression(range.content());
  }
}

void DoclingMapper::on_sheet_database_range(
    const officev1::SheetDatabaseRange& range) {
  docv1::NamedRange* out = document_.add_named_ranges();
  out->set_name(range.name());
  out->set_kind("database");
  set_grid_span(range.range(), std::string(), out->mutable_range());
  if (range.sheet_index() >= 0) {
    pending_range_sheets_.emplace_back(document_.named_ranges_size() - 1,
                                       range.sheet_index());
  }
  out->set_has_headers(range.contains_header());
  out->set_has_totals(range.totals_row());
}

void DoclingMapper::on_sheet_cell_comment(
    const officev1::SheetCellComment& comment) {
  std::string sheet_ref = "#/body";
  if (auto group = sheet_group_.find(comment.sheet_index());
      group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  if (auto sheet_layer = sheet_layer_.find(comment.sheet_index());
      sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  auto comments = sheet_comments_.find(comment.sheet_index());
  if (comments == sheet_comments_.end()) {
    docv1::GroupItem* section = add_group(
        sheet_ref, docv1::GROUP_LABEL_COMMENT_SECTION, "comments", layer);
    comments = sheet_comments_
        .emplace(comment.sheet_index(), section->self_ref()).first;
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               layer, comments->second);
  handle.base->set_text(comment.text());
  handle.base->set_orig(comment.text());
  docv1::CommentMeta* identity = handle.base->mutable_comment_meta();
  if (!comment.author().empty()) identity->set_author(comment.author());
  // The office core hands a sheet annotation's date over as its own string
  // and does not say in what format, so it stays the raw spelling.
  if (!comment.date().empty()) identity->set_timestamp_raw(comment.date());
  // The cell a note is attached to is a position in the sheet grid.
  docv1::ProvenanceItem* where = handle.base->add_prov();
  where->set_page_no(comment.sheet_index() + 1);
  docv1::GridCell* grid = where->mutable_grid();
  grid->set_row(comment.row());
  grid->set_col(comment.column());
  const std::string sheet = sheet_label(comment.sheet_index());
  if (!sheet.empty()) grid->set_sheet(sheet);
  identity->set_shown(comment.visible());
  auto table = sheet_table_.find(comment.sheet_index());
  if (table != sheet_table_.end()) {
    document_.mutable_tables(table->second)->add_comments()->set_ref(
        handle.ref);
  }
}

void DoclingMapper::on_sheet_chart(const officev1::SheetChart& chart) {
  std::string sheet_ref = "#/body";
  auto group = sheet_group_.find(chart.sheet_index());
  if (group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  auto sheet_layer = sheet_layer_.find(chart.sheet_index());
  if (sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_CHART, layer,
                                            sheet_ref, nullptr);
  if (!chart.name().empty()) picture->mutable_shape()->set_name(chart.name());
  // Where the chart's data came from, as grid spans on the sheet it sits on.
  docv1::ChartMeta* provenance = picture->mutable_chart();
  const std::string sheet = sheet_label(chart.sheet_index());
  for (const officev1::SheetRangeRef& range : chart.ranges()) {
    set_grid_span(range, sheet, provenance->add_sources());
  }
  provenance->set_has_column_headers(chart.has_column_headers());
  provenance->set_has_row_headers(chart.has_row_headers());
  add_prov(picture->mutable_prov(), chart.sheet_index(), true, 0, 0, 0, 0, 0,
           0);
}

void DoclingMapper::on_sheet_pivot_table(
    const officev1::SheetPivotTable& pivot) {
  std::string sheet_ref = "#/body";
  if (auto group = sheet_group_.find(pivot.sheet_index());
      group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  if (auto sheet_layer = sheet_layer_.find(pivot.sheet_index());
      sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  docv1::TableItem* table = add_table(layer, sheet_ref, nullptr);
  const officev1::SheetRangeRef& output = pivot.output_range();
  table->mutable_data()->set_num_rows(output.end_row() - output.start_row()
                                      + 1);
  table->mutable_data()->set_num_cols(output.end_column()
                                      - output.start_column() + 1);
  // The definition is a declaration of the workbook, not of the output
  // table, so it lives beside the document with its ranges as grid spans.
  const std::string sheet = sheet_label(pivot.sheet_index());
  docv1::PivotSpec* spec = document_.add_pivots();
  spec->set_name(pivot.name());
  set_grid_span(pivot.source_range(), sheet, spec->mutable_source());
  set_grid_span(output, sheet, spec->mutable_output());
  for (const std::string& name : pivot.row_fields()) {
    spec->add_row_fields(name);
  }
  for (const std::string& name : pivot.column_fields()) {
    spec->add_column_fields(name);
  }
  for (const std::string& name : pivot.data_fields()) {
    spec->add_data_fields(name);
  }
  for (const std::string& name : pivot.page_fields()) {
    spec->add_page_fields(name);
  }
  add_prov(table->mutable_prov(), pivot.sheet_index(), true, 0, 0, 0, 0, 0, 0);
}

void DoclingMapper::on_comment(const officev1::Comment& comment) {
  if (comments_group_ref_.empty()) {
    comments_group_ref_ =
        add_group("#/furniture", docv1::GROUP_LABEL_COMMENT_SECTION,
                  "comments", docv1::CONTENT_LAYER_FURNITURE)
            ->self_ref();
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_FURNITURE,
                               comments_group_ref_);
  std::string text =
      !comment.text().empty() ? comment.text() : concat_runs(comment.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  docv1::CommentMeta* identity = handle.base->mutable_comment_meta();
  if (!comment.author().empty()) identity->set_author(comment.author());
  if (!comment.initials().empty()) identity->set_initials(comment.initials());
  if (comment.epoch_ms() != 0) {
    set_instant(comment.epoch_ms(), identity->mutable_timestamp());
  }
  identity->set_resolved(comment.resolved());
  if (!comment.anchored_text().empty()) {
    identity->set_anchored_text(comment.anchored_text());
  }
  // The office core's comment name is the threading key and nothing else,
  // so it becomes a reference to the parent comment rather than a string a
  // consumer would have to match itself.
  if (!comment.name().empty()) {
    comment_ref_by_name_[comment.name()] = handle.ref;
  }
  if (!comment.parent_name().empty()) {
    pending_comment_parents_.emplace_back(handle.ref, comment.parent_name());
  }
  if (comment.char_start() >= 0) {
    // The item this comment annotates is not known yet: a comment can close
    // before the paragraph holding it is emitted. The back-link is made
    // once the whole body has streamed past.
    pending_comments_.push_back(
        {handle.ref, comment.char_start(), comment.char_end()});
  }
  add_caret_prov(handle.base->mutable_prov(), comment.page_index(),
                 comment.anchor(), comment.anchor(), 0,
                 runs_length(comment.runs()));
}

void DoclingMapper::on_tracked_change(const officev1::TrackedChange& change) {
  // Tracked changes annotate spans of the body text rather than adding
  // display text of their own, so they are records of the document, each
  // pointing at the item and range it touches.
  int index = document_.changes_size();
  docv1::ChangeRecord* record = document_.add_changes();
  record->set_id(change.identifier().empty()
                     ? std::to_string(change.index())
                     : change.identifier());
  std::string kind = change.kind_name();
  std::ranges::transform(kind, kind.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  record->set_kind(kind);
  record->set_author(change.author());
  if (change.epoch_ms() != 0) {
    set_instant(change.epoch_ms(), record->mutable_timestamp());
  }
  if (!change.changed_text().empty()) {
    record->set_content(change.changed_text());
  }
  pending_changes_.push_back({index, change.char_start(), change.char_end()});
}

void DoclingMapper::on_bookmark(const officev1::Bookmark& bookmark) {
  // A bookmark names a position other content points at, so it becomes a
  // named anchor; the target resolves once the body index is complete.
  pending_anchors_.push_back(
      {bookmark.name(), bookmark.char_start(), bookmark.char_end()});
}

void DoclingMapper::on_form_field(const officev1::FormField& field) {
  ensure_form_arena();
  // One field item per office form field, holding its heading and its
  // value: the schema's own form subtree rather than a text item with a bag
  // of attributes hanging off it.
  int field_index = document_.field_items_size();
  std::string field_ref = "#/field_items/" + std::to_string(field_index);
  docv1::FieldItem* item = document_.add_field_items();
  item->set_self_ref(field_ref);
  item->mutable_parent()->set_ref(field_region_ref_);
  item->set_label(docv1::DOC_ITEM_LABEL_FIELD_ITEM);
  item->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_collector_source(item->mutable_source());
  link_child(field_region_ref_, field_ref);

  const bool checkbox = field.kind() == officev1::FORM_FIELD_KIND_CHECKBOX;
  std::string heading_ref;
  if (!field.label().empty()) {
    TextHandle heading = add_text(TextKind::kFieldHeading,
                                  docv1::DOC_ITEM_LABEL_FIELD_KEY,
                                  docv1::CONTENT_LAYER_BODY, field_ref);
    heading.base->set_text(field.label());
    heading.base->set_orig(field.label());
    heading_ref = heading.ref;
  }

  docv1::DocItemLabel value_label = docv1::DOC_ITEM_LABEL_FIELD_VALUE;
  if (checkbox) {
    value_label = field.checked() ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                                  : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED;
  }
  TextHandle value = add_text(TextKind::kFieldValue, value_label,
                              docv1::CONTENT_LAYER_BODY, field_ref);
  // A checkbox renders no text of its own; its state is its label.
  std::string text = field.text();
  if (text.empty() && !checkbox) text = field.label();
  value.base->set_text(text);
  value.base->set_orig(text);
  // The field's own kind, in the office core's vocabulary when it names one.
  std::string kind = field.field_type();
  if (kind.empty()) {
    kind = officev1::FormFieldKind_Name(field.kind());
    const std::string prefix = "FORM_FIELD_KIND_";
    if (kind.starts_with(prefix)) kind = kind.substr(prefix.size());
    std::ranges::transform(kind, kind.begin(),
                           [](unsigned char c) { return std::tolower(c); });
  }
  value.item->mutable_field_value()->set_kind(kind);

  // The key-to-value pairing, as the graph the form arena is built around.
  docv1::GraphData* graph =
      document_.mutable_form_items(0)->mutable_graph();
  int key_cell = -1;
  if (!heading_ref.empty()) {
    docv1::GraphCell* cell = graph->add_cells();
    cell->set_label(docv1::GRAPH_CELL_LABEL_KEY);
    key_cell = graph_cell_id_++;
    cell->set_cell_id(key_cell);
    cell->set_text(field.label());
    cell->set_orig(field.label());
    cell->mutable_item_ref()->set_ref(heading_ref);
  }
  docv1::GraphCell* value_cell = graph->add_cells();
  value_cell->set_label(checkbox ? docv1::GRAPH_CELL_LABEL_CHECKBOX
                                 : docv1::GRAPH_CELL_LABEL_VALUE);
  int value_cell_id = graph_cell_id_++;
  value_cell->set_cell_id(value_cell_id);
  value_cell->set_text(text);
  value_cell->set_orig(text);
  value_cell->mutable_item_ref()->set_ref(value.ref);
  if (key_cell >= 0) {
    docv1::GraphLink* link = graph->add_links();
    link->set_label(docv1::GRAPH_LINK_LABEL_TO_VALUE);
    link->set_source_cell_id(key_cell);
    link->set_target_cell_id(value_cell_id);
  }

  // The field's own identity: what the form calls it, what a choice field
  // offers and which entry is chosen, and the parameters a fieldmark
  // stores. A draw-page form control is told from an in-text fieldmark by
  // whether the field carries a span.
  if (!field.name().empty()) item->set_field_name(field.name());
  for (const std::string& entry : field.list_entries()) {
    item->add_options(entry);
  }
  if (field.selected_index() >= 0) {
    item->set_selected_index(field.selected_index());
  }
  auto* parameters = item->mutable_parameters();
  for (const officev1::FormFieldParameter& parameter : field.parameters()) {
    switch (parameter.value_case()) {
      case officev1::FormFieldParameter::kBoolValue:
        (*parameters)[parameter.name()] =
            parameter.bool_value() ? "true" : "false";
        break;
      case officev1::FormFieldParameter::kIntValue:
        (*parameters)[parameter.name()] =
            std::to_string(parameter.int_value());
        break;
      case officev1::FormFieldParameter::kDoubleValue:
        (*parameters)[parameter.name()] =
            std::to_string(parameter.double_value());
        break;
      case officev1::FormFieldParameter::kStringValue:
        (*parameters)[parameter.name()] = parameter.string_value();
        break;
      case officev1::FormFieldParameter::VALUE_NOT_SET:
        if (parameter.string_list().empty()) {
          (*parameters)[parameter.name()] = std::string();
          break;
        }
        // A list keeps one entry per key rather than a joined string, so no
        // separator has to be guessed back out on the way in.
        for (int entry = 0; entry < parameter.string_list_size(); entry++) {
          (*parameters)[parameter.name() + "[" + std::to_string(entry) + "]"] =
              parameter.string_list(entry);
        }
        break;
    }
  }
  if (field.char_start() >= 0) {
    // The span resolves against the body index once the whole body has
    // streamed past, like every other anchor.
    pending_field_spans_.push_back(
        {field_index, field.char_start(), field.char_end()});
  }

  if (field.control() && field.width_twips() > 0 && field.has_anchor()) {
    add_prov(item->mutable_prov(), field.page_index(), false,
             static_cast<double>(field.anchor().x()),
             static_cast<double>(field.anchor().y()),
             static_cast<double>(field.anchor().x() + field.width_twips()),
             static_cast<double>(field.anchor().y() + field.height_twips()),
             0, 0);
  } else {
    add_caret_prov(item->mutable_prov(), field.page_index(), field.anchor(),
                   field.anchor(), 0, 0);
  }
  for (const docv1::ProvenanceItem& prov : item->prov()) {
    *value.base->add_prov() = prov;
  }
}

namespace {

const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader:
      return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading:
      return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

}  // namespace

std::vector<std::string> docling_integrity_errors(
    const docv1::Document& document) {
  std::vector<std::string> errors;
  std::set<std::string> refs = {"#/body", "#/furniture"};
  // (item ref, parent ref) pairs and every children list, gathered in one
  // walk so parents can be validated against their children afterwards.
  std::vector<std::pair<std::string, std::string>> parents;
  std::map<std::string, std::set<std::string>> children;

  auto collect = [&](const std::string& self_ref,
                     const google::protobuf::RepeatedPtrField<docv1::RefItem>&
                         child_refs,
                     bool has_parent, const std::string& parent_ref) {
    if (self_ref.empty()) {
      errors.push_back("item with empty self_ref");
      return;
    }
    if (!refs.insert(self_ref).second) {
      errors.push_back("duplicate self_ref " + self_ref);
    }
    for (const docv1::RefItem& child : child_refs) {
      children[self_ref].insert(child.ref());
    }
    if (has_parent) parents.emplace_back(self_ref, parent_ref);
  };

  // Provenance page numbers are 1-based in this dialect, so the proto3
  // default of 0 is never a page: an item carrying it points nowhere. The
  // box itself is not checked; zero-area placeholders are legitimate for
  // whole-sheet and whole-chart items that have no measured rectangle.
  auto check_prov = [&](const std::string& owner,
                        const google::protobuf::RepeatedPtrField<
                            docv1::ProvenanceItem>& prov) {
    for (const docv1::ProvenanceItem& item : prov) {
      // A page-plane locator needs a 1-based page. The page-less arms
      // (a media time span, a byte range, a sheet cell) locate content in
      // their own space and legitimately carry no page at all.
      const bool page_less =
          item.has_time() || item.has_byte_range() || item.has_grid();
      if (item.page_no() < 1 && !page_less) {
        errors.push_back("provenance of " + owner + " has page_no "
                         + std::to_string(item.page_no())
                         + ", which is not a 1-based page");
      }
    }
  };

  // The key-value and form arenas link into the item arenas through their
  // graph cells as well as through children, so a cell's item_ref is a
  // reference like any other and is resolved after the walk.
  std::vector<std::pair<std::string, std::string>> graph_item_refs;
  auto collect_graph = [&](const std::string& owner,
                           const docv1::GraphData& graph) {
    for (const docv1::GraphCell& cell : graph.cells()) {
      if (cell.has_prov() && cell.prov().page_no() < 1 &&
          !(cell.prov().has_time() || cell.prov().has_byte_range() ||
            cell.prov().has_grid())) {
        errors.push_back("provenance of graph cell "
                         + std::to_string(cell.cell_id()) + " of " + owner
                         + " has page_no " + std::to_string(cell.prov().page_no())
                         + ", which is not a 1-based page");
      }
      if (!cell.has_item_ref()) continue;
      graph_item_refs.emplace_back(owner, cell.item_ref().ref());
    }
  };

  for (const docv1::RefItem& child : document.body().children()) {
    children["#/body"].insert(child.ref());
  }
  for (const docv1::RefItem& child : document.furniture().children()) {
    children["#/furniture"].insert(child.ref());
  }
  for (const docv1::GroupItem& group : document.groups()) {
    collect(group.self_ref(), group.children(), group.has_parent(),
            group.parent().ref());
  }
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      // CodeItem carries the reference fields inline instead of in a nested
      // base, so it has no TextItemBase to read; it is still a linked arena
      // item and its references are checked like every other one.
      const docv1::CodeItem& code = item.code();
      collect(code.self_ref(), code.children(), code.has_parent(),
              code.parent().ref());
      check_prov(code.self_ref(), code.prov());
      continue;
    }
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) {
      errors.push_back("text item with unset variant");
      continue;
    }
    collect(base->self_ref(), base->children(), base->has_parent(),
            base->parent().ref());
    check_prov(base->self_ref(), base->prov());
  }
  for (const docv1::PictureItem& picture : document.pictures()) {
    collect(picture.self_ref(), picture.children(), picture.has_parent(),
            picture.parent().ref());
    check_prov(picture.self_ref(), picture.prov());
  }
  for (const docv1::TableItem& table : document.tables()) {
    collect(table.self_ref(), table.children(), table.has_parent(),
            table.parent().ref());
    check_prov(table.self_ref(), table.prov());
  }
  // The four form arenas carry the same reference shape as the item arenas
  // above; merge and the renderers follow their links, so they are held to
  // the same contract.
  for (const docv1::KeyValueItem& item : document.key_value_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
    collect_graph(item.self_ref(), item.graph());
  }
  for (const docv1::FormItem& item : document.form_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
    collect_graph(item.self_ref(), item.graph());
  }
  for (const docv1::FieldRegionItem& item : document.field_regions()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
  }
  for (const docv1::FieldItem& item : document.field_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
  }
  for (const auto& [owner, child_refs] : children) {
    for (const std::string& child : child_refs) {
      if (refs.find(child) == refs.end()) {
        errors.push_back("child " + child + " of " + owner
                         + " does not resolve");
      }
    }
  }
  for (const auto& [child_ref, parent_ref] : parents) {
    if (refs.find(parent_ref) == refs.end()) {
      errors.push_back("parent " + parent_ref + " of " + child_ref
                       + " does not resolve");
      continue;
    }
    if (auto listed = children.find(parent_ref);
        listed == children.end()
        || listed->second.find(child_ref) == listed->second.end()) {
      errors.push_back("parent " + parent_ref + " does not list "
                       + child_ref + " as a child");
    }
  }
  for (const docv1::TableItem& table : document.tables()) {
    for (const docv1::FineRef& comment : table.comments()) {
      if (refs.find(comment.ref()) == refs.end()) {
        errors.push_back("comment ref " + comment.ref() + " of "
                         + table.self_ref() + " does not resolve");
      }
    }
  }
  for (const auto& [owner, item_ref] : graph_item_refs) {
    if (refs.find(item_ref) == refs.end()) {
      errors.push_back("graph cell item_ref " + item_ref + " of " + owner
                       + " does not resolve");
    }
  }
  // The anchored references the fold resolves against the document-absolute
  // character space: a back-link, a change target, or a named anchor that
  // names nothing would be worse than one left unset.
  for (const docv1::BaseTextItem& item : document.texts()) {
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) continue;
    for (const docv1::FineRef& comment : base->comments()) {
      if (!refs.contains(comment.ref())) {
        errors.push_back("comment ref " + comment.ref() + " of "
                         + base->self_ref() + " does not resolve");
      }
    }
    for (const docv1::InlineSpan& span : base->spans()) {
      if (span.has_target() && !refs.contains(span.target().ref())) {
        errors.push_back("span target " + span.target().ref() + " of "
                         + base->self_ref() + " does not resolve");
      }
    }
  }
  for (const docv1::ChangeRecord& change : document.changes()) {
    if (change.has_target() && !refs.contains(change.target().ref())) {
      errors.push_back("change target " + change.target().ref() + " of "
                       + change.id() + " does not resolve");
    }
  }
  for (const docv1::NamedAnchor& anchor : document.anchors()) {
    if (anchor.has_target() && !refs.contains(anchor.target().ref())) {
      errors.push_back("anchor target " + anchor.target().ref() + " of "
                       + anchor.name() + " does not resolve");
    }
  }
  return errors;
}

}  // namespace grparse
