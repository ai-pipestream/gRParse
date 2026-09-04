// grpc-calamine client: the wire is handle-based and carries no document
// event, so the fold happens here. OpenWorkbook uploads the bytes once,
// one StreamWorksheetRange per sheet streams the cells, and each sheet
// folds into a sheet group holding one TableItem in absolute row and column
// offsets (the shape the office fold gives libreoffice sheets). The handle
// is closed on every path, success or failure.

#include "grparse/document_collectors.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "calamine/v1/calamine_service.grpc.pb.h"
#include "collector_support.h"

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
  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(base + static_cast<int64_t>(whole_days), &year, &month, &day);
  when->set_year(static_cast<int32_t>(year));
  when->set_month(static_cast<int32_t>(month));
  when->set_day(static_cast<int32_t>(day));
  const int64_t seconds =
      static_cast<int64_t>(std::llround((serial.value() - whole_days) * 86400.0));
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

// The shortest round-trip spelling of a double ("1", "2.5"), not the fixed
// six decimals of std::to_string.
std::string double_text(double value) {
  char buffer[32];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
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

  void end_sheet() {
    if (current_ == nullptr) return;
    current_->set_num_rows(static_cast<int32_t>(num_rows_));
    current_->set_num_cols(static_cast<int32_t>(num_cols_));
    current_ = nullptr;
  }

  void defined_names(const calaminev1::Metadata& metadata) {
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

 private:
  void cell(uint32_t row_index, uint32_t column, const calaminev1::CellData& data) {
    std::string text;
    docv1::CellValue value;
    bool typed = true;
    switch (data.value_case()) {
      case calaminev1::CellData::kEmpty:
        return;
      case calaminev1::CellData::kIntValue:
        value.set_number(static_cast<double>(data.int_value()));
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
        text = data.date_time_iso();
        typed = false;
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
    num_cols_ = std::max(num_cols_, static_cast<uint64_t>(column) + 1);
    num_rows_ = std::max(num_rows_, static_cast<uint64_t>(row_index) + 1);
  }

  docv1::Document& document_;
  docv1::TableData* current_ = nullptr;
  docv1::ContentLayer layer_ = docv1::CONTENT_LAYER_BODY;
  std::string sheet_name_;
  uint64_t num_rows_ = 0;
  uint64_t num_cols_ = 0;
  int sheets_folded_ = 0;
};

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

}  // namespace

CollectorOutcome collect_calamine_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline) {
  CollectorOutcome outcome;
  auto stub = calaminev1::CalamineService::NewStub(channel);

  grpc::ClientContext open_context;
  open_context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
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

  const WorkbookGuard close_handle(stub.get(), opened.workbook_id());
  CalamineFold fold(outcome.document);
  fold.defined_names(opened.metadata());

  int sheet_index = 0;
  for (const calaminev1::Sheet& sheet : opened.metadata().sheets()) {
    grpc::ClientContext sheet_context;
    sheet_context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
    calaminev1::StreamWorksheetRangeRequest request;
    request.set_workbook_id(opened.workbook_id());
    request.mutable_sheet()->set_sheet_index(static_cast<uint32_t>(sheet_index));
    auto stream = stub->StreamWorksheetRange(&sheet_context, request);

    fold.begin_sheet(sheet_index, sheet);
    bool terminal_error = false;
    std::string sheet_error;
    calaminev1::StreamWorksheetRangeResponse event;
    while (stream->Read(&event)) {
      switch (event.event_case()) {
        case calaminev1::StreamWorksheetRangeResponse::kRow:
          fold.row(event.row());
          break;
        case calaminev1::StreamWorksheetRangeResponse::kRows:
          for (const calaminev1::WorksheetRow& row : event.rows().rows()) {
            fold.row(row);
          }
          break;
        case calaminev1::StreamWorksheetRangeResponse::kError:
          if (event.error().terminal()) {
            terminal_error = true;
            sheet_error = event.error().error().message();
          } else {
            outcome.warnings.push_back("sheet " + sheet.name() + ": " +
                                       event.error().error().message());
          }
          break;
        default:
          break;
      }
      event.Clear();
      if (terminal_error) break;
    }
    fold.end_sheet();
    const grpc::Status status = stream->Finish();
    ++sheet_index;
    if (status.ok() && !terminal_error) continue;
    const std::string why = terminal_error ? sheet_error : status.error_message();
    int64_t cells_folded = 0;
    for (const docv1::TableItem& table : outcome.document.tables()) {
      cells_folded += table.data().table_cells_size();
    }
    if (cells_folded == 0) {
      // Nothing folded at all: the failure is the outcome, not an empty
      // workbook that happens to report success.
      outcome.error = "calamine collector: sheet " + sheet.name() + ": " + why;
      outcome.code = terminal_error ? grpc::StatusCode::INVALID_ARGUMENT
                                    : map_code(status.error_code());
      return outcome;
    }
    // Sheets already folded survive a later sheet's failure: a partially
    // read workbook is a partial success, not a failed parse.
    outcome.warnings.push_back("sheet " + sheet.name() + " failed: " + why);
    break;
  }

  outcome.success = true;
  return outcome;
}

}  // namespace grparse
