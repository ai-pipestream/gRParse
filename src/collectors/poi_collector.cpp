// grPOIc client: the wire carries no document event, so the typed
// ParseEvent stream folds into a Document here. Paragraphs fold by their
// style names (Title, Heading N, list styles), tables and sheets into
// TableItems, slides into their own groups (with their tables), embedded
// objects into the attachment descriptors; the ParseStatus trailer ends a
// successful parse. Worksheets are requested in batches and the batches of
// one sheet fold back into its one table.

#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ai/pipestream/poi/v1/poi_service.grpc.pb.h"
#include "collector_support.h"

namespace docv1 = ai::pipestream::document::v1;
namespace poiv1 = ai::pipestream::poi::v1;

namespace grparse {
namespace {

// The heading depth a POI style name declares: "Heading1".."Heading 9" and
// friends. 0 when the style is not a heading.
int heading_level(const std::string& style) {
  std::string lowered = style;
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  if (!lowered.starts_with("heading")) return 0;
  const char* rest = lowered.c_str() + 7;
  while (*rest == ' ') ++rest;
  if (*rest < '1' || *rest > '9' || *(rest + 1) != '\0') return 0;
  return *rest - '0';
}

// True for the word processor's list styles ("ListParagraph", "List
// Bullet", "List Number", ...). The fold cannot tell a bullet from a number
// from the style name alone, so the item is an unenumerated list item.
bool list_style(const std::string& style) {
  std::string lowered = style;
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  return lowered.starts_with("list");
}

// The widest grid a table may claim: the spreadsheet column limit. A span
// on the wire is a claim, not a size: a hostile gridSpan of two billion
// would otherwise size the column ledger.
constexpr int64_t kMaxTableColumns = 16384;

// The tallest sheet a merged region may claim: the spreadsheet row limit.
// Like a table span, a CellRange is a claim from the wire, not a size.
constexpr int64_t kMaxSheetRows = 1048576;

// A wire span clamped to the room left: at least 1, at most `room` (itself
// at least 1). Sets `clamped` when the wire asked for more.
int clamp_span(int64_t wire, int64_t room, bool& clamped) {
  const int64_t limit = std::max<int64_t>(1, room);
  const int64_t span = std::max<int64_t>(1, wire);
  if (span <= limit) return static_cast<int>(span);
  clamped = true;
  return static_cast<int>(limit);
}

class PoiFold {
 public:
  PoiFold(docv1::Document& document, std::vector<std::string>& warnings)
      : document_(document), warnings_(warnings) {
    document_.mutable_body()->set_self_ref("#/body");
    document_.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
    document_.mutable_furniture()->set_self_ref("#/furniture");
    document_.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  }

  // The document's own account: the well-known core properties land in
  // source_meta, the tagged tail keeps its keys as user properties. The
  // merge resolves these against every other collector's claims.
  void document_info(const poiv1::DocumentInfo& info) {
    const poiv1::DocumentMetadata& metadata = info.metadata();
    docv1::DocumentMeta* meta = document_.mutable_source_meta();
    if (!metadata.title().empty()) meta->set_title(metadata.title());
    if (!metadata.author().empty()) meta->add_authors(metadata.author());
    if (!metadata.last_modified_by().empty()) {
      meta->set_modified_by(metadata.last_modified_by());
    }
    if (metadata.has_created()) *meta->mutable_created() = metadata.created();
    if (metadata.has_modified()) *meta->mutable_modified() = metadata.modified();
    for (const poiv1::MetadataEntry& entry : metadata.tail()) {
      for (const poiv1::MetadataValue& value : entry.values()) {
        docv1::UserProperty* property = meta->add_user_properties();
        property->set_name(entry.key());
        switch (value.value_case()) {
          case poiv1::MetadataValue::kStringValue:
            property->set_text(value.string_value());
            break;
          case poiv1::MetadataValue::kIntValue:
            property->set_number(static_cast<double>(value.int_value()));
            break;
          case poiv1::MetadataValue::kDoubleValue:
            property->set_number(value.double_value());
            break;
          case poiv1::MetadataValue::kBoolValue:
            property->set_boolean(value.bool_value());
            break;
          case poiv1::MetadataValue::kTimestampValue:
            *property->mutable_instant() = value.timestamp_value();
            break;
          default:
            break;
        }
      }
    }
  }

  void paragraph(const poiv1::Paragraph& paragraph, const std::string& parent_ref) {
    const std::string& style = paragraph.style();
    const int level = heading_level(style);
    docv1::TextItemBase* base = nullptr;
    if (level == 0 && style == "Title") {
      base = document_.add_texts()->mutable_title()->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_TITLE);
    } else if (level > 0) {
      auto* header = document_.add_texts()->mutable_section_header();
      header->set_level(level);
      base = header->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
    } else if (list_style(style)) {
      base = document_.add_texts()->mutable_list_item()->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
    } else {
      base = document_.add_texts()->mutable_text()->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_PARAGRAPH);
    }
    fill_text(base, paragraph.text(), parent_ref);
    if (!style.empty()) base->set_style_name(style);
  }

  // A body table hangs off the body; a slide table (slide_index set) hangs
  // off its slide's group, which the Slide event right before it opened.
  void table(const poiv1::Table& table) {
    std::string parent_ref = "#/body";
    if (table.has_slide_index()) {
      const auto found = slide_groups_.find(table.slide_index());
      if (found != slide_groups_.end()) {
        parent_ref = found->second;
      } else {
        warnings_.push_back("poi table " + std::to_string(document_.tables_size()) +
                            ": slide " + std::to_string(table.slide_index()) +
                            " was never announced; the table is kept in the body");
      }
    }
    docv1::TableItem* item = add_table(parent_ref);
    docv1::TableData* data = item->mutable_data();
    data->set_num_rows(table.rows_size());
    int num_cols = 0;
    // The wire does not repeat the positions a merged cell covers, so a cell
    // spanning rows from above still holds its columns in the rows below:
    // each column remembers the first row it is free again, and a row's
    // cells skip the columns still held.
    std::vector<int> occupied_until;
    bool clamped = false;
    for (int row = 0; row < table.rows_size(); ++row) {
      int column = 0;
      for (const poiv1::TableCell& cell : table.rows(row).cells()) {
        while (column < static_cast<int>(occupied_until.size()) &&
               occupied_until[static_cast<size_t>(column)] > row) {
          ++column;
        }
        docv1::TableCell* out = data->add_table_cells();
        const int row_span =
            clamp_span(cell.row_span(), int64_t{table.rows_size()} - row, clamped);
        const int col_span =
            clamp_span(cell.col_span(), kMaxTableColumns - column, clamped);
        if (occupied_until.size() < static_cast<size_t>(column + col_span)) {
          occupied_until.resize(static_cast<size_t>(column + col_span), 0);
        }
        for (int held = column; held < column + col_span; ++held) {
          occupied_until[static_cast<size_t>(held)] = row + row_span;
        }
        out->set_start_row_offset_idx(row);
        out->set_end_row_offset_idx(row + row_span);
        out->set_start_col_offset_idx(column);
        out->set_end_col_offset_idx(column + col_span);
        out->set_row_span(row_span);
        out->set_col_span(col_span);
        out->set_text(cell.text());
        column += col_span;
      }
      num_cols = std::max(num_cols, column);
    }
    // A column held only by a merge from above still counts.
    data->set_num_cols(std::max(num_cols, static_cast<int>(occupied_until.size())));
    if (clamped) {
      warnings_.push_back("poi table " + std::to_string(document_.tables_size() - 1) +
                          ": a cell span past the table's bounds was clamped");
    }
  }

  // One worksheet folds into a sheet group holding one TableItem in
  // absolute row and column offsets, the same shape the office fold gives
  // libreoffice sheets. The client asks for sheet batches, so a large sheet
  // arrives as consecutive Sheet events with the same index and name, every
  // one but the last setting more_rows: each batch appends its rows to the
  // table the first one opened, and the last carries the merged regions.
  void sheet(const poiv1::Sheet& sheet) {
    if (open_sheet_.has_value() &&
        (open_sheet_->index != sheet.index() || open_sheet_->name != sheet.name())) {
      end_sheet_batches();
    }
    if (!open_sheet_.has_value()) open_sheet(sheet);
    OpenSheet& open = *open_sheet_;
    docv1::TableData* data = open.data;
    for (const poiv1::SheetRow& row : sheet.rows()) {
      if (row.cells().empty()) continue;
      docv1::ProvenanceItem* row_prov = data->add_row_prov();
      row_prov->set_page_no(static_cast<int32_t>(sheet.index()) + 1);
      docv1::GridCell* grid = row_prov->mutable_grid();
      grid->set_row(static_cast<int32_t>(row.row_index()));
      grid->set_col(static_cast<int32_t>(row.cells(0).column_index()));
      grid->set_sheet(sheet.name());
      for (const poiv1::SheetCell& cell : row.cells()) {
        docv1::TableCell* out = data->add_table_cells();
        out->set_start_row_offset_idx(static_cast<int32_t>(row.row_index()));
        out->set_end_row_offset_idx(static_cast<int32_t>(row.row_index()) + 1);
        out->set_start_col_offset_idx(static_cast<int32_t>(cell.column_index()));
        out->set_end_col_offset_idx(static_cast<int32_t>(cell.column_index()) + 1);
        out->set_row_span(1);
        out->set_col_span(1);
        out->set_text(!cell.formatted().empty() ? cell.formatted() : cell.text());
        docv1::CellValue value;
        if (typed_sheet_value(cell, &value)) *out->mutable_value() = value;
        open.num_rows = std::max<int64_t>(open.num_rows, int64_t{row.row_index()} + 1);
        open.num_cols = std::max<int64_t>(open.num_cols, int64_t{cell.column_index()} + 1);
      }
    }
    if (!sheet.more_rows()) merge_regions(sheet);
    data->set_num_rows(static_cast<int32_t>(open.num_rows));
    data->set_num_cols(static_cast<int32_t>(open.num_cols));
    if (!sheet.more_rows()) open_sheet_.reset();
  }

  // Ends a sheet whose batches stopped before one cleared more_rows: any
  // other event, another sheet, or the end of the stream. The rows already
  // folded stay; the gap is reported, because the rest never arrived.
  void end_sheet_batches() {
    if (!open_sheet_.has_value()) return;
    warnings_.push_back("poi sheet '" + open_sheet_->name +
                        "': its batches ended before the last one; later rows are missing");
    open_sheet_.reset();
  }

  // One slide: its own group, the title as a level-1 section header, the
  // text frames as paragraphs, the speaker notes on the notes layer.
  void slide(const poiv1::Slide& slide) {
    const std::string name = !slide.title().empty()
                                 ? slide.title()
                                 : "slide " + std::to_string(slide.index() + 1);
    docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SLIDE, name,
                                        docv1::CONTENT_LAYER_BODY);
    slide_groups_[slide.index()] = group->self_ref();
    if (!slide.title().empty()) {
      auto* header = document_.add_texts()->mutable_section_header();
      header->set_level(1);
      docv1::TextItemBase* base = header->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
      fill_text(base, slide.title(), group->self_ref());
    }
    for (const std::string& text : slide.texts()) {
      docv1::TextItemBase* base = document_.add_texts()->mutable_text()->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_PARAGRAPH);
      fill_text(base, text, group->self_ref());
    }
    for (const std::string& note : slide.notes()) {
      docv1::TextItemBase* base = document_.add_texts()->mutable_text()->mutable_base();
      base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
      fill_text(base, note, group->self_ref());
      base->set_content_layer(docv1::CONTENT_LAYER_NOTES);
    }
  }

  // Descriptor-only by contract (the bytes are not streamed), so the object
  // registers as an attachment pointer for fan-out parsing.
  void embedded_object(const poiv1::EmbeddedObject& object) {
    docv1::SubDocumentRef* attachment = document_.add_attachments();
    attachment->set_id(object.id());
    attachment->set_name(object.filename());
    attachment->set_media_type(object.content_type());
    attachment->set_size_bytes(object.size_bytes());
  }

 private:
  // The sheet whose batches are still arriving (the last one set more_rows).
  struct OpenSheet {
    uint32_t index = 0;
    std::string name;
    docv1::TableData* data = nullptr;
    int64_t num_rows = 0;
    int64_t num_cols = 0;
  };

  // A hidden sheet keeps its content on the invisible layer, as the office
  // and calamine folds place theirs, so the merge sees the same shape.
  void open_sheet(const poiv1::Sheet& sheet) {
    const docv1::ContentLayer layer =
        sheet.hidden() ? docv1::CONTENT_LAYER_INVISIBLE : docv1::CONTENT_LAYER_BODY;
    docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SHEET, sheet.name(), layer);
    group->mutable_sheet()->set_index(static_cast<int32_t>(sheet.index()));
    group->mutable_sheet()->set_visible(!sheet.hidden());
    docv1::TableItem* item = add_table(group->self_ref());
    item->set_content_layer(layer);
    open_sheet_ = OpenSheet{.index = sheet.index(), .name = sheet.name(),
                            .data = item->mutable_data()};
  }

  // The sheet's merged cells: the anchor (top-left) cell of each range takes
  // the range's spans. A range whose anchor holds nothing has no cell to
  // carry them and is left out; a range that runs backwards, or past the
  // spreadsheet's own limits, is a claim the fold refuses or clamps.
  void merge_regions(const poiv1::Sheet& sheet) {
    if (sheet.merged_regions().empty()) return;
    OpenSheet& open = *open_sheet_;
    std::unordered_map<uint64_t, int> anchors;
    anchors.reserve(static_cast<size_t>(open.data->table_cells_size()));
    for (int index = 0; index < open.data->table_cells_size(); ++index) {
      const docv1::TableCell& cell = open.data->table_cells(index);
      anchors.emplace(cell_key(cell.start_row_offset_idx(), cell.start_col_offset_idx()), index);
    }
    bool refused = false;
    for (const poiv1::CellRange& range : sheet.merged_regions()) {
      if (range.last_row() < range.first_row() || range.last_column() < range.first_column() ||
          range.first_row() >= kMaxSheetRows || range.first_column() >= kMaxTableColumns) {
        refused = true;
        continue;
      }
      const auto found = anchors.find(cell_key(range.first_row(), range.first_column()));
      if (found == anchors.end()) continue;
      bool clamped = false;
      const int row_span =
          clamp_span(int64_t{range.last_row()} - range.first_row() + 1,
                     kMaxSheetRows - range.first_row(), clamped);
      const int col_span =
          clamp_span(int64_t{range.last_column()} - range.first_column() + 1,
                     kMaxTableColumns - range.first_column(), clamped);
      refused = refused || clamped;
      docv1::TableCell* anchor = open.data->mutable_table_cells(found->second);
      anchor->set_end_row_offset_idx(anchor->start_row_offset_idx() + row_span);
      anchor->set_end_col_offset_idx(anchor->start_col_offset_idx() + col_span);
      anchor->set_row_span(row_span);
      anchor->set_col_span(col_span);
      open.num_rows = std::max<int64_t>(open.num_rows, int64_t{range.first_row()} + row_span);
      open.num_cols = std::max<int64_t>(open.num_cols, int64_t{range.first_column()} + col_span);
    }
    if (refused) {
      warnings_.push_back("poi sheet '" + open.name +
                          "': a merged region outside the sheet's bounds was refused or clamped");
    }
  }

  static uint64_t cell_key(uint32_t row, uint32_t column) {
    return (uint64_t{row} << 32U) | column;
  }

  docv1::GroupItem* add_group(const std::string& parent_ref, docv1::GroupLabel label,
                              const std::string& name, docv1::ContentLayer layer) {
    const int index = document_.groups_size();
    docv1::GroupItem* group = document_.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(index));
    group->mutable_parent()->set_ref(parent_ref);
    group->set_content_layer(layer);
    group->set_name(name);
    group->set_label(label);
    if (parent_ref == "#/body") {
      document_.mutable_body()->add_children()->set_ref(group->self_ref());
    } else if (docv1::GroupItem* parent = find_group(parent_ref)) {
      parent->add_children()->set_ref(group->self_ref());
    }
    return group;
  }

  docv1::GroupItem* find_group(const std::string& self_ref) {
    for (docv1::GroupItem& group : *document_.mutable_groups()) {
      if (group.self_ref() == self_ref) return &group;
    }
    return nullptr;
  }

  void fill_text(docv1::TextItemBase* base, const std::string& text,
                 const std::string& parent_ref) {
    base->set_self_ref("#/texts/" + std::to_string(document_.texts_size() - 1));
    base->mutable_parent()->set_ref(parent_ref);
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_orig(text);
    base->set_text(text);
    base->add_source()->mutable_collector()->set_collector("poi");
    if (parent_ref == "#/body") {
      document_.mutable_body()->add_children()->set_ref(base->self_ref());
    } else if (docv1::GroupItem* parent = find_group(parent_ref)) {
      parent->add_children()->set_ref(base->self_ref());
    }
  }

  docv1::TableItem* add_table(const std::string& parent_ref) {
    docv1::TableItem* item = document_.add_tables();
    item->set_self_ref("#/tables/" + std::to_string(document_.tables_size() - 1));
    item->mutable_parent()->set_ref(parent_ref);
    item->set_content_layer(docv1::CONTENT_LAYER_BODY);
    item->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    item->add_source()->mutable_collector()->set_collector("poi");
    if (parent_ref == "#/body") {
      document_.mutable_body()->add_children()->set_ref(item->self_ref());
    } else if (docv1::GroupItem* parent = find_group(parent_ref)) {
      parent->add_children()->set_ref(item->self_ref());
    }
    return item;
  }

  // The cell's typed value: an error beats a formula, because a formula
  // that failed has no value to report, and a formula beats its cached
  // result (the display string keeps it, exactly like the office fold).
  // False when the cell holds nothing the schema types (a plain string).
  static bool typed_sheet_value(const poiv1::SheetCell& cell, docv1::CellValue* out) {
    if (!cell.formula().empty()) {
      out->set_formula(cell.formula());
      return true;
    }
    switch (cell.value_case()) {
      case poiv1::SheetCell::kNumber:
        out->set_number(cell.number());
        return true;
      case poiv1::SheetCell::kBoolean:
        out->set_boolean(cell.boolean());
        return true;
      case poiv1::SheetCell::kDate: {
        // A spreadsheet date is a wall-clock value; POI hands it over as an
        // instant, read here as UTC civil fields.
        const std::time_t seconds =
            static_cast<std::time_t>(cell.date().seconds());
        std::tm broken{};
        if (gmtime_r(&seconds, &broken) == nullptr) return false;
        docv1::CivilDateTime* when = out->mutable_datetime();
        when->set_year(broken.tm_year + 1900);
        when->set_month(broken.tm_mon + 1);
        when->set_day(broken.tm_mday);
        when->set_hour(broken.tm_hour);
        when->set_minute(broken.tm_min);
        when->set_second(broken.tm_sec);
        return true;
      }
      default:
        return false;
    }
  }

  docv1::Document& document_;
  std::vector<std::string>& warnings_;
  std::optional<OpenSheet> open_sheet_;
  // Slide index -> its group's self_ref, for the slide's tables.
  std::map<uint32_t, std::string> slide_groups_;
};

}  // namespace

CollectorOutcome collect_poi_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& document_id,
                                      const std::string& filename,
                                      const std::string& content_type,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline,
                                      CollectorCancelled cancelled) {
  auto stub = poiv1::PoiParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->ParseDocument(&context);

  // The wire reads identity from the first chunk and wants the last chunk
  // marked complete; a half-close without one is a truncated upload.
  poiv1::ParseRequestChunk request;
  request.set_document_id(document_id);
  request.set_filename(filename);
  request.set_content_type(content_type);
  // Without batches a worksheet is one message that grows with the sheet,
  // and the server refuses one past 256 MiB; batches keep each event near
  // 1 MiB, and the fold puts the sheet back together.
  request.set_sheet_batches(true);
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/true,
      [&bytes](poiv1::ParseRequestChunk& frame, size_t offset, size_t length, bool last) {
        frame.set_data(bytes.data() + offset, length);
        frame.set_complete(last);
      });

  CollectorOutcome outcome;
  PoiFold fold(outcome.document, outcome.warnings);
  bool status_seen = false;
  poiv1::ParseEvent event;
  while (stream->Read(&event)) {
    // A sheet's batches are consecutive: anything else ends them.
    if (event.event_case() != poiv1::ParseEvent::kSheet) fold.end_sheet_batches();
    switch (event.event_case()) {
      case poiv1::ParseEvent::kDocumentInfo:
        fold.document_info(event.document_info());
        break;
      case poiv1::ParseEvent::kParagraph:
        fold.paragraph(event.paragraph(), "#/body");
        break;
      case poiv1::ParseEvent::kTable:
        fold.table(event.table());
        break;
      case poiv1::ParseEvent::kSheet:
        fold.sheet(event.sheet());
        break;
      case poiv1::ParseEvent::kSlide:
        fold.slide(event.slide());
        break;
      case poiv1::ParseEvent::kEmbeddedObject:
        fold.embedded_object(event.embedded_object());
        break;
      case poiv1::ParseEvent::kStatus:
        status_seen = true;
        for (const std::string& warning : event.status().warnings()) {
          outcome.warnings.push_back(warning);
        }
        break;
      default:
        break;
    }
    event.Clear();
  }
  fold.end_sheet_batches();

  upload.join();
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    outcome.error = collector_status_text("poi", status);
    outcome.code = map_code(status.error_code());
    return outcome;
  }
  if (!status_seen) {
    outcome.error = "poi collector: stream ended without a terminal status";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    return outcome;
  }
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
