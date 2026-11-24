// grpc-calamine client: the wire carries no document event, so the fold
// happens here. One ParseWorkbook stream uploads the bytes and streams every
// sheet's cells back, and each sheet folds into a sheet group holding one
// TableItem in absolute row and column offsets (the shape the office fold
// gives libreoffice sheets). The workbook lives on the server for that
// stream only. A server older than ParseWorkbook answers UNIMPLEMENTED and
// gets the handle path instead: OpenWorkbook, one StreamWorksheetRange per
// sheet, and CloseWorkbook on every path, success or failure.

#include "grparse/document_collectors.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "calamine/v1/calamine_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/workbook_cells.h"
#include "google/protobuf/util/time_util.h"

namespace calaminev1 = calamine::v1;
namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's
// algorithms), for the Excel serial dates calamine reports.
constexpr int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr void civil_from_days(int64_t z, int64_t* year, unsigned* month,
                               unsigned* day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp + (mp < 10 ? 3 : -9);
  *year = y + (*month <= 2);
}

// An Excel serial date as civil fields. The epoch matches calamine's own
// conversion: 1899-12-30 for the 1900 date system (which absorbs the
// phantom 1900-02-29 the way calamine does), 1904-01-01 for the 1904 one.
void excel_serial_civil(const calaminev1::ExcelDateTime& serial,
                        docv1::CivilDateTime* when) {
  const int64_t base = serial.is_1904() ? days_from_civil(1904, 1, 1)
                                        : days_from_civil(1899, 12, 30);
  const double whole_days = std::floor(serial.value());
  int64_t days = static_cast<int64_t>(whole_days);
  int64_t seconds =
      static_cast<int64_t>(std::llround((serial.value() - whole_days) * 86400.0));
  // A time that rounds up to midnight is the next day's 00:00:00.
  if (seconds >= 86400) {
    seconds -= 86400;
    ++days;
  }
  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(base + days, &year, &month, &day);
  when->set_year(static_cast<int32_t>(year));
  when->set_month(static_cast<int32_t>(month));
  when->set_day(static_cast<int32_t>(day));
  when->set_hour(static_cast<int32_t>((seconds / 3600) % 24));
  when->set_minute(static_cast<int32_t>((seconds / 60) % 60));
  when->set_second(static_cast<int32_t>(seconds % 60));
}

// The display string a date cell gets: the date alone when the serial
// carries no time of day.
std::string civil_text(const docv1::CivilDateTime& when, bool has_time) {
  if (!has_time) {
    return std::format("{:04}-{:02}-{:02}", when.year(), when.month(), when.day());
  }
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", when.year(),
                     when.month(), when.day(), when.hour(), when.minute(),
                     when.second());
}

// An ISO 8601 date ("2015-04-13") or date-time ("2015-04-13T09:30:00",
// fractional seconds ignored) as civil fields. False for anything else,
// a time of day alone included.
bool iso_civil(const std::string& text, docv1::CivilDateTime* when) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int consumed = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2d%n", &year, &month, &day, &consumed) != 3
      || consumed != 10 || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  when->set_year(year);
  when->set_month(month);
  when->set_day(day);
  if (text.size() == 10) return true;
  if (text[10] != 'T'
      || std::sscanf(text.c_str() + 11, "%2d:%2d:%2d", &hour, &minute, &second) != 3) {
    return false;
  }
  when->set_hour(hour);
  when->set_minute(minute);
  when->set_second(second);
  return true;
}

// The shortest round-trip spelling of a double ("1", "2.5", "10412459"), not
// the fixed six decimals of std::to_string. The plain form of to_chars picks
// fixed notation unless the exponent form is shorter; chars_format::general
// would spell an eight-digit reference number "1.0412459e+07".
std::string double_text(double value) {
  char buffer[32];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc()) return std::to_string(value);
  return std::string(buffer, end);
}

// The error literal Excel shows, as the calamine contract documents it.
std::string error_text(calaminev1::CellErrorType error) {
  switch (error) {
    case calaminev1::CELL_ERROR_TYPE_DIV0: return "#DIV/0!";
    case calaminev1::CELL_ERROR_TYPE_NA: return "#N/A";
    case calaminev1::CELL_ERROR_TYPE_NAME: return "#NAME?";
    case calaminev1::CELL_ERROR_TYPE_NULL: return "#NULL!";
    case calaminev1::CELL_ERROR_TYPE_NUM: return "#NUM!";
    case calaminev1::CELL_ERROR_TYPE_REF: return "#REF!";
    case calaminev1::CELL_ERROR_TYPE_VALUE: return "#VALUE!";
    case calaminev1::CELL_ERROR_TYPE_GETTING_DATA: return "#DATA!";
    default: return "#ERROR!";
  }
}

class CalamineFold {
 public:
  explicit CalamineFold(docv1::Document& document) : document_(document) {
    document_.mutable_body()->set_self_ref("#/body");
    document_.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
    document_.mutable_furniture()->set_self_ref("#/furniture");
    document_.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  }

  void begin_sheet(int index, const calaminev1::Sheet& sheet) {
    const bool visible = sheet.visible() != calaminev1::SHEET_VISIBLE_HIDDEN &&
                         sheet.visible() != calaminev1::SHEET_VISIBLE_VERY_HIDDEN;
    layer_ = visible ? docv1::CONTENT_LAYER_BODY : docv1::CONTENT_LAYER_INVISIBLE;
    const int group_index = document_.groups_size();
    docv1::GroupItem* group = document_.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(group_index));
    group->mutable_parent()->set_ref("#/body");
    group->set_content_layer(layer_);
    group->set_name(sheet.name());
    group->set_label(docv1::GROUP_LABEL_SHEET);
    group->mutable_sheet()->set_index(index);
    group->mutable_sheet()->set_visible(visible);
    document_.mutable_body()->add_children()->set_ref(group->self_ref());
    sheet_name_ = sheet.name();

    docv1::TableItem* item = document_.add_tables();
    item->set_self_ref("#/tables/" + std::to_string(document_.tables_size() - 1));
    item->mutable_parent()->set_ref(group->self_ref());
    item->set_content_layer(layer_);
    item->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    item->add_source()->mutable_collector()->set_collector("calamine");
    group->add_children()->set_ref(item->self_ref());
    current_ = item->mutable_data();
    num_rows_ = 0;
    num_cols_ = 0;
    ++sheets_folded_;
  }

  // One dense row. The stream anchors values at column A, so a value's
  // index is its absolute column; empty variants and rows the batches skip
  // are the sheet's empty regions and fold to nothing.
  void row(const calaminev1::WorksheetRow& row) {
    if (current_ == nullptr) return;
    if (!row.values().empty()) {
      docv1::ProvenanceItem* row_prov = current_->add_row_prov();
      row_prov->set_page_no(sheets_folded_);
      docv1::GridCell* grid = row_prov->mutable_grid();
      grid->set_row(static_cast<int32_t>(row.row_index()));
      grid->set_col(0);
      grid->set_sheet(sheet_name_);
    }
    for (int column = 0; column < row.values_size(); ++column) {
      cell(row.row_index(), static_cast<uint32_t>(column), row.values(column));
    }
    num_rows_ = std::max(num_rows_, static_cast<uint64_t>(row.row_index()) + 1);
  }

  // The sheet's merged areas, from the range header.
  void merged(const calaminev1::RangeStarted& started) {
    merged_.clear();
    formats_.clear();
    for (const auto& format : started.number_formats()) {
      formats_.emplace(format.style_id(), format);
    }
    for (const auto& declared : started.tables()) {
      const auto& first = declared.range().start();
      const auto& last = declared.range().end();
      constexpr auto limit = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
      if (last.row() < first.row() || last.col() < first.col()
          || last.row() >= limit || last.col() >= limit) continue;
      auto* table = current_->add_declared_tables();
      table->set_name(declared.name());
      table->mutable_range()->mutable_start()->set_row(static_cast<int32_t>(first.row()));
      table->mutable_range()->mutable_start()->set_col(static_cast<int32_t>(first.col()));
      table->mutable_range()->mutable_start()->set_sheet(sheet_name_);
      table->mutable_range()->mutable_end()->set_row(static_cast<int32_t>(last.row()));
      table->mutable_range()->mutable_end()->set_col(static_cast<int32_t>(last.col()));
      table->mutable_range()->mutable_end()->set_sheet(sheet_name_);
      table->set_header_row_count(declared.header_row_count());
      table->set_totals_row_count(declared.totals_row_count());
      *table->mutable_columns() = declared.columns();
    }
    for (const calaminev1::Dimensions& area : started.merged_regions()) {
      const auto& start = area.start();
      const auto& end = area.end();
      if (end.row() < start.row() || end.col() < start.col()) continue;
      merged_.push_back({static_cast<int>(start.row()), static_cast<int>(start.col()),
                         static_cast<int>(end.row() - start.row()) + 1,
                         static_cast<int>(end.col() - start.col()) + 1});
    }
  }

  // Closes the sheet: spans its merged areas and marks its header band.
  // Returns the number of header cells marked.
  int end_sheet() {
    if (current_ == nullptr) return 0;
    current_->set_num_rows(static_cast<int32_t>(num_rows_));
    current_->set_num_cols(static_cast<int32_t>(num_cols_));
    apply_merged_areas(current_, merged_);
    merged_.clear();
    const bool declared = !current_->declared_tables().empty();
    const int marked = mark_sheet_header(current_);
    current_ = nullptr;
    return declared ? 0 : marked;
  }

  void defined_names(const calaminev1::Metadata& metadata) {
    if (metadata.has_properties()) {
      const auto& properties = metadata.properties();
      auto* meta = document_.mutable_source_meta();
      if (properties.has_title()) meta->set_title(properties.title());
      if (properties.has_subject()) meta->set_subject(properties.subject());
      if (properties.has_description()) meta->set_description(properties.description());
      if (properties.has_last_modified_by()) meta->set_modified_by(properties.last_modified_by());
      if (properties.has_category()) meta->set_category(properties.category());
      if (properties.has_content_status()) meta->set_content_status(properties.content_status());
      if (properties.has_revision()) meta->set_revision(properties.revision());
      if (properties.has_version()) meta->set_format_version(properties.version());
      if (properties.has_application()) meta->set_authoring_tool(properties.application());
      if (properties.has_app_version()) meta->set_application_version(properties.app_version());
      if (properties.has_company()) meta->set_company(properties.company());
      if (properties.has_template_()) meta->set_template_(properties.template_());
      if (properties.has_manager()) meta->set_manager(properties.manager());
      if (properties.has_creator()) meta->add_authors(properties.creator());
      // Keywords are a source string; do not guess separators or discard spelling.
      if (properties.has_keywords()) meta->add_keywords(properties.keywords());
      if (properties.has_created()) {
        meta->set_created_raw(properties.created());
        google::protobuf::Timestamp stamp;
        if (google::protobuf::util::TimeUtil::FromString(properties.created(), &stamp)) {
          *meta->mutable_created() = stamp;
        }
      }
      if (properties.has_modified()) {
        meta->set_modified_raw(properties.modified());
        google::protobuf::Timestamp stamp;
        if (google::protobuf::util::TimeUtil::FromString(properties.modified(), &stamp)) {
          *meta->mutable_modified() = stamp;
        }
      }
    }
    for (const calaminev1::DefinedName& name : metadata.defined_names()) {
      docv1::NamedRange* range = document_.add_named_ranges();
      range->set_name(name.name());
      range->set_kind("named");
      // The definition arrives as calamine spells it (a reference string or
      // a formula); it is an expression, not a resolved rectangle.
      range->set_expression(name.definition());
    }
  }

  int sheets_folded() const { return sheets_folded_; }

  // Cells folded so far, across every sheet.
  int64_t cells_folded() const {
    int64_t cells = 0;
    for (const docv1::TableItem& table : document_.tables()) {
      cells += table.data().table_cells_size();
    }
    return cells;
  }

 private:
  void cell(uint32_t row_index, uint32_t column, const calaminev1::CellData& data) {
    std::string text;
    docv1::CellValue value;
    bool typed = true;
    switch (data.value_case()) {
      case calaminev1::CellData::kEmpty:
        if (!data.has_formula() && !data.has_style_id()) return;
        typed = false;
        break;
      case calaminev1::CellData::kIntValue:
        // The source states an integer: it stays one. A double holds
        // integers exactly only up to 2^53.
        value.set_integer(data.int_value());
        text = std::to_string(data.int_value());
        break;
      case calaminev1::CellData::kFloatValue:
        value.set_number(data.float_value());
        text = double_text(data.float_value());
        break;
      case calaminev1::CellData::kStringValue:
      case calaminev1::CellData::kSharedStringValue:
        // A string cell's text IS the value; CellValue has no string kind.
        text = data.value_case() == calaminev1::CellData::kStringValue
                   ? data.string_value()
                   : data.shared_string_value();
        typed = false;
        break;
      case calaminev1::CellData::kBoolValue:
        value.set_boolean(data.bool_value());
        text = data.bool_value() ? "TRUE" : "FALSE";
        break;
      case calaminev1::CellData::kDateTime: {
        // A spreadsheet date is a wall-clock value; it stays civil.
        excel_serial_civil(data.date_time(), value.mutable_datetime());
        text = civil_text(value.datetime(),
                          std::floor(data.date_time().value()) != data.date_time().value());
        break;
      }
      case calaminev1::CellData::kDateTimeIso:
        // ODS stores dates as ISO text; a date or date-time it spells is
        // the same civil value an Excel serial gives, so it is typed the
        // same way. A bare time of day stays text.
        text = data.date_time_iso();
        typed = iso_civil(text, value.mutable_datetime());
        if (!typed) value.Clear();
        break;
      case calaminev1::CellData::kDurationIso:
        text = data.duration_iso();
        typed = false;
        break;
      case calaminev1::CellData::kError:
        text = error_text(data.error());
        value.set_error(text);
        break;
      default:
        // shared_string_id only arrives in use_string_table mode, which
        // this client never opts into.
        return;
    }
    docv1::TableCell* out = current_->add_table_cells();
    out->set_start_row_offset_idx(static_cast<int32_t>(row_index));
    out->set_end_row_offset_idx(static_cast<int32_t>(row_index) + 1);
    out->set_start_col_offset_idx(static_cast<int32_t>(column));
    out->set_end_col_offset_idx(static_cast<int32_t>(column) + 1);
    out->set_row_span(1);
    out->set_col_span(1);
    out->set_text(text);
    if (typed) *out->mutable_value() = std::move(value);
    // Formula source is independent of the cached value (including strings
    // and empty caches), so neither reading replaces the other.
    if (data.has_formula()) out->mutable_value()->set_formula_source(data.formula());
    if (data.has_style_id()) {
      auto* typed_value = out->mutable_value();
      typed_value->set_style_id(data.style_id());
      const auto found = formats_.find(data.style_id());
      if (found != formats_.end()) {
        typed_value->set_number_format_id(found->second.format_id());
        if (found->second.has_code()) typed_value->set_number_format(found->second.code());
      }
    }
    num_cols_ = std::max(num_cols_, static_cast<uint64_t>(column) + 1);
    num_rows_ = std::max(num_rows_, static_cast<uint64_t>(row_index) + 1);
  }

  docv1::Document& document_;
  docv1::TableData* current_ = nullptr;
  std::vector<MergedArea> merged_;
  std::map<uint32_t, calaminev1::NumberFormat> formats_;
  docv1::ContentLayer layer_ = docv1::CONTENT_LAYER_BODY;
  std::string sheet_name_;
  uint64_t num_rows_ = 0;
  uint64_t num_cols_ = 0;
  int sheets_folded_ = 0;
};

// Folds one event of a sheet's range stream. Returns true for a terminal
// in-band error, whose message lands in `error`; non-terminal ones become
// warnings.
bool fold_range_event(CalamineFold& fold, const std::string& sheet_name,
                      const calaminev1::StreamWorksheetRangeResponse& event,
                      std::vector<Warning>& warnings, std::string& error) {
  switch (event.event_case()) {
    case calaminev1::StreamWorksheetRangeResponse::kStarted:
      fold.merged(event.started());
      break;
    case calaminev1::StreamWorksheetRangeResponse::kRow:
      fold.row(event.row());
      break;
    case calaminev1::StreamWorksheetRangeResponse::kRows:
      for (const calaminev1::WorksheetRow& row : event.rows().rows()) {
        fold.row(row);
      }
      break;
    case calaminev1::StreamWorksheetRangeResponse::kRowGap:
      // A run of rows holding nothing: the fold places populated cells
      // at their absolute rows, so the run needs no blank rows spelled
      // out, and a gap is never the last row event of a range.
      break;
    case calaminev1::StreamWorksheetRangeResponse::kError:
      if (event.error().terminal()) {
        error = event.error().error().message();
        return true;
      }
      warnings.push_back("sheet " + sheet_name + ": " + event.error().error().message());
      break;
    default:
      break;
  }
  return false;
}

// Closes a folded sheet, warning when its header band was inferred.
void end_sheet(CalamineFold& fold, const std::string& sheet_name,
               std::vector<Warning>& warnings) {
  if (fold.end_sheet() > 0) {
    warnings.push_back("sheet '" + sheet_name
                       + "': header row inferred by the labels-over-data band "
                         "rule; no database range declares it");
  }
}

// Closes the workbook handle however the leg ends: the server holds the
// parsed workbook per handle, so a leg that fails mid-read still owes the
// close. Best effort with its own short leash; the inbound deadline may
// already have passed by the time the guard runs.
class WorkbookGuard {
 public:
  WorkbookGuard(calaminev1::CalamineService::Stub* stub, std::string workbook_id)
      : stub_(stub), workbook_id_(std::move(workbook_id)) {}
  ~WorkbookGuard() {
    if (workbook_id_.empty()) return;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    calaminev1::CloseWorkbookRequest request;
    request.set_workbook_id(workbook_id_);
    calaminev1::CloseWorkbookResponse response;
    static_cast<void>(stub_->CloseWorkbook(&context, request, &response));
  }

  WorkbookGuard(const WorkbookGuard&) = delete;
  WorkbookGuard& operator=(const WorkbookGuard&) = delete;

 private:
  calaminev1::CalamineService::Stub* stub_;
  std::string workbook_id_;
};

// The handle path, for a server that predates ParseWorkbook.
CollectorOutcome collect_via_handle(calaminev1::CalamineService::Stub* stub,
                                    const std::string& bytes,
                                    CollectorDeadline inbound_deadline,
                                    const CollectorCancelled& cancelled) {
  CollectorOutcome outcome;
  grpc::ClientContext open_context;
  open_context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch open_watch(open_context, cancelled);
  calaminev1::OpenWorkbookResponse opened;
  auto upload = stub->OpenWorkbook(&open_context, &opened);
  calaminev1::OpenWorkbookRequest frame;
  // Auto-detect from the bytes; the default header-row rule stays the
  // server's, the fold reads cells positionally either way.
  frame.mutable_options();
  upload_stream(*upload, frame, bytes, /*always_send_chunk=*/false,
                [&bytes](calaminev1::OpenWorkbookRequest& chunk_frame, size_t offset,
                         size_t length, bool /*last*/) {
                  chunk_frame.set_chunk(bytes.data() + offset, length);
                });
  const grpc::Status open_status = upload->Finish();
  if (!open_status.ok()) {
    outcome.error = collector_status_text("calamine", open_status);
    outcome.code = map_code(open_status.error_code());
    return outcome;
  }

  const WorkbookGuard close_handle(stub, opened.workbook_id());
  CalamineFold fold(outcome.document);
  fold.defined_names(opened.metadata());

  int sheet_index = 0;
  for (const calaminev1::Sheet& sheet : opened.metadata().sheets()) {
    grpc::ClientContext sheet_context;
    sheet_context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
    const CancelWatch sheet_watch(sheet_context, cancelled);
    calaminev1::StreamWorksheetRangeRequest request;
    request.set_workbook_id(opened.workbook_id());
    request.set_include_formulas(true);
    request.mutable_sheet()->set_sheet_index(static_cast<uint32_t>(sheet_index));
    auto stream = stub->StreamWorksheetRange(&sheet_context, request);

    fold.begin_sheet(sheet_index, sheet);
    bool terminal_error = false;
    std::string sheet_error;
    calaminev1::StreamWorksheetRangeResponse event;
    while (stream->Read(&event)) {
      terminal_error = fold_range_event(fold, sheet.name(), event, outcome.warnings,
                                        sheet_error);
      event.Clear();
      if (terminal_error) break;
    }
    end_sheet(fold, sheet.name(), outcome.warnings);
    // A terminal error left the stream unread: Finish would wait on the
    // messages still in flight, so the call is cancelled first.
    if (terminal_error) sheet_context.TryCancel();
    const grpc::Status status = stream->Finish();
    ++sheet_index;
    if (status.ok() && !terminal_error) continue;
    const std::string why = terminal_error ? sheet_error : status.error_message();
    if (fold.cells_folded() == 0) {
      // Nothing folded at all: the failure is the outcome, not an empty
      // workbook that happens to report success.
      outcome.error = "calamine collector: sheet " + sheet.name() + ": " + why;
      outcome.code = terminal_error ? grpc::StatusCode::INVALID_ARGUMENT
                                    : map_code(status.error_code());
      return outcome;
    }
    // Sheets already folded survive a later sheet's failure: a partially
    // read workbook is a partial success, not a failed parse. The read stops
    // here, so this sheet and every sheet after it are named as not read to
    // the end: what the fold holds of them is a part, or nothing.
    outcome.warnings.push_back(Warning(docv1::WARNING_CODE_COLLECTOR_STREAM_INCOMPLETE,
                                       "sheet " + sheet.name() + " failed: " + why));
    for (int unread = sheet_index - 1; unread < opened.metadata().sheets_size(); ++unread) {
      outcome.incomplete_sheets.push_back({opened.metadata().sheets(unread).name(), why});
    }
    break;
  }

  outcome.success = true;
  return outcome;
}

// The ParseWorkbook stream. `unimplemented` is set when the server answered
// UNIMPLEMENTED before sending anything, which is the one case the caller
// falls back on.
CollectorOutcome collect_via_parse_workbook(calaminev1::CalamineService::Stub* stub,
                                            const std::string& bytes,
                                            CollectorDeadline inbound_deadline,
                                            const CollectorCancelled& cancelled,
                                            bool* unimplemented) {
  CollectorOutcome outcome;
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, cancelled);
  auto stream = stub->ParseWorkbook(&context);

  calaminev1::ParseWorkbookRequest frame;
  calaminev1::ParseWorkbookOpen* open = frame.mutable_open();
  // Auto-detect from the bytes; the default header-row rule stays the
  // server's, the fold reads cells positionally either way. Metadata
  // always comes first and carries the defined names, so ranges are the
  // only part asked for.
  open->mutable_options();
  open->set_emit_ranges(true);
  open->mutable_range_options()->set_include_formulas(true);
  // The upload half-closes when it is through, which ends it; nothing here
  // cancels from the request side, the context's watch does that.
  ConcurrentUpload upload(context, *stream, frame, bytes, /*always_send_chunk=*/false,
                          [&bytes](calaminev1::ParseWorkbookRequest& chunk_frame,
                                   size_t offset, size_t length, bool /*last*/) {
                            chunk_frame.mutable_chunk()->set_data(bytes.data() + offset,
                                                                  length);
                          });

  CalamineFold fold(outcome.document);
  std::vector<calaminev1::Sheet> sheets;
  bool anything_read = false;
  bool done_seen = false;
  bool protocol_error = false;
  std::string protocol_detail;
  int current = -1;
  bool current_failed = false;
  // The sheets whose range part has started, by index.
  std::set<int> begun;
  int sheets_failed = 0;
  std::string first_failure;
  calaminev1::ParseWorkbookResponse event;
  while (stream->Read(&event)) {
    anything_read = true;
    switch (event.event_case()) {
      case calaminev1::ParseWorkbookResponse::kMetadata:
        for (const calaminev1::Sheet& sheet : event.metadata().metadata().sheets()) {
          sheets.push_back(sheet);
        }
        fold.defined_names(event.metadata().metadata());
        break;
      case calaminev1::ParseWorkbookResponse::kRange: {
        const uint32_t index = event.range().sheet_index();
        if (index >= sheets.size()) {
          protocol_error = true;
          protocol_detail = "range event for sheet " + std::to_string(index)
                            + " beyond the metadata's " + std::to_string(sheets.size());
          break;
        }
        if (static_cast<int>(index) != current) {
          if (current >= 0) end_sheet(fold, sheets[current].name(), outcome.warnings);
          current = static_cast<int>(index);
          current_failed = false;
          begun.insert(current);
          fold.begin_sheet(current, sheets[current]);
        }
        // A terminal error ends this sheet's part only; the server moves on
        // to the next sheet, and so does the fold.
        if (current_failed) break;
        std::string error;
        if (fold_range_event(fold, sheets[current].name(), event.range().event(),
                             outcome.warnings, error)) {
          current_failed = true;
          ++sheets_failed;
          if (first_failure.empty()) {
            first_failure = "sheet " + sheets[current].name() + ": " + error;
          }
          outcome.warnings.push_back("sheet " + sheets[current].name()
                                     + " failed: " + error);
          outcome.incomplete_sheets.push_back({sheets[current].name(), error});
        }
        break;
      }
      case calaminev1::ParseWorkbookResponse::kDone:
        done_seen = true;
        break;
      default:
        // Parts this client did not ask for, and events a later server may
        // add: the contract says to ignore them.
        break;
    }
    event.Clear();
    if (protocol_error) break;
  }
  if (current >= 0) end_sheet(fold, sheets[current].name(), outcome.warnings);
  // An abandoned read leaves messages in flight; Finish would wait on them.
  if (protocol_error) context.TryCancel();
  upload.join();
  const grpc::Status status = stream->Finish();

  if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED && !anything_read) {
    *unimplemented = true;
    outcome.error = collector_status_text("calamine", status);
    outcome.code = map_code(status.error_code());
    return outcome;
  }

  std::string why;
  grpc::StatusCode code = grpc::StatusCode::OK;
  if (protocol_error) {
    why = protocol_detail;
    code = grpc::StatusCode::INTERNAL;
  } else if (!status.ok()) {
    why = status.error_message();
    code = map_code(status.error_code());
  } else if (!done_seen) {
    why = "the stream ended without its done event";
    code = grpc::StatusCode::UNAVAILABLE;
  }
  if (code == grpc::StatusCode::OK && sheets_failed > 0 && fold.cells_folded() == 0) {
    // Every sheet that had anything failed: the failure is the outcome, not
    // an empty workbook that happens to report success.
    outcome.error = "calamine collector: " + first_failure;
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  if (code != grpc::StatusCode::OK) {
    if (fold.cells_folded() == 0) {
      outcome.error = protocol_error || status.ok()
                          ? "calamine collector: " + why
                          : collector_status_text("calamine", status);
      outcome.code = code;
      return outcome;
    }
    // Sheets already folded survive a later failure: a partially read
    // workbook is a partial success, not a failed parse.
    outcome.warnings.push_back(Warning(docv1::WARNING_CODE_COLLECTOR_STREAM_INCOMPLETE,
                                       "calamine stream failed after "
                                           + std::to_string(fold.sheets_folded())
                                           + " sheet(s): " + why));
    // Nothing ends a sheet's part but the next sheet's first event or the
    // done event, so the sheet the stream stopped in cannot be told from a
    // whole one by its cells, and a sheet whose part never started was not
    // read at all. Neither is a reading of the sheet to its end.
    for (int index = 0; index < static_cast<int>(sheets.size()); ++index) {
      const bool stopped_inside = index == current && !current_failed;
      if (stopped_inside || !begun.contains(index)) {
        outcome.incomplete_sheets.push_back({sheets[index].name(), why});
      }
    }
  }
  outcome.success = true;
  return outcome;
}

}  // namespace

CollectorOutcome collect_calamine_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline,
                                           CollectorCancelled cancelled) {
  auto stub = calaminev1::CalamineService::NewStub(channel);
  bool unimplemented = false;
  CollectorOutcome outcome = collect_via_parse_workbook(stub.get(), bytes, inbound_deadline,
                                                        cancelled, &unimplemented);
  if (!unimplemented) return outcome;
  CollectorOutcome fallback = collect_via_handle(stub.get(), bytes, inbound_deadline, cancelled);
  fallback.protocol = CollectorProtocol::kLegacyFallback;
  fallback.warnings.insert(fallback.warnings.begin(),
                           "calamine collector: the server does not implement ParseWorkbook "
                           "(" + outcome.error + "); used the OpenWorkbook handle path");
  return fallback;
}

}  // namespace grparse
