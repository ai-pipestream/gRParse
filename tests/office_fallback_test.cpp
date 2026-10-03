// The office routes end to end, over the wire, with the libreoffice leg
// failing. A word processing or presentation upload has libreoffice as its
// only collector, so its failure is the parse's failure: a typed status that
// names the collector and its cause, on the unary and the streaming surface
// alike, never a successful parse with an empty body. A workbook keeps the
// calamine leg beside libreoffice, so the same failure leaves a partial
// success carrying calamine's reading.

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/office/v1/office_service.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "calamine/v1/calamine_service.grpc.pb.h"
#include "grparse/base64.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"
#include "support/check.h"

namespace {

using namespace std::chrono_literals;
namespace calaminev1 = calamine::v1;
namespace officev1 = ai::pipestream::office::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;

// Reads the whole upload, then fails the conversion with a fixed status, the
// way the office worker reports a document its import filter rejects.
class FailingOfficeService final : public officev1::OfficeRenderService::Service {
 public:
  explicit FailingOfficeService(grpc::Status status) : status_(std::move(status)) {}

  grpc::Status StreamPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<officev1::StreamPagesResponse, officev1::StreamPagesRequest>*
          stream) override {
    calls.fetch_add(1);
    officev1::StreamPagesRequest request;
    while (stream->Read(&request)) {
    }
    return status_;
  }

  std::atomic<int> calls{0};

 private:
  grpc::Status status_;
};

// One visible sheet holding a header row and one data row.
class OneSheetCalamineService final : public calaminev1::CalamineService::Service {
 public:
  grpc::Status OpenWorkbook(grpc::ServerContext*,
                            grpc::ServerReader<calaminev1::OpenWorkbookRequest>* reader,
                            calaminev1::OpenWorkbookResponse* response) override {
    opens.fetch_add(1);
    calaminev1::OpenWorkbookRequest frame;
    while (reader->Read(&frame)) {
    }
    response->set_workbook_id("wb-fallback");
    calaminev1::Sheet* sheet = response->mutable_metadata()->add_sheets();
    sheet->set_name("Ledger");
    sheet->set_typ(calaminev1::SHEET_TYPE_WORKSHEET);
    sheet->set_visible(calaminev1::SHEET_VISIBLE_VISIBLE);
    return grpc::Status::OK;
  }

  grpc::Status StreamWorksheetRange(
      grpc::ServerContext*, const calaminev1::StreamWorksheetRangeRequest*,
      grpc::ServerWriter<calaminev1::StreamWorksheetRangeResponse>* writer) override {
    calaminev1::StreamWorksheetRangeResponse event;
    calaminev1::WorksheetRowBatch* batch = event.mutable_rows();
    calaminev1::WorksheetRow* header = batch->add_rows();
    header->set_row_index(0);
    header->add_values()->set_string_value("Item");
    header->add_values()->set_string_value("Amount");
    calaminev1::WorksheetRow* data = batch->add_rows();
    data->set_row_index(1);
    data->add_values()->set_string_value("Rent");
    data->add_values()->set_int_value(1200);
    writer->Write(event);
    return grpc::Status::OK;
  }

  grpc::Status CloseWorkbook(grpc::ServerContext*, const calaminev1::CloseWorkbookRequest*,
                             calaminev1::CloseWorkbookResponse* response) override {
    response->set_closed(true);
    return grpc::Status::OK;
  }

  std::atomic<int> opens{0};
};

// The CV path is never reached on these routes; a call that got there would
// be the regression, so the recognizer refuses.
class UnusedRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    throw std::runtime_error("an office route reached the CV recognizer");
  }
};

// Serves one fake for each office collector and the gRParse services that
// dial them.
class OfficeRig final {
 public:
  explicit OfficeRig(grpc::Status office_status)
      : office_(std::move(office_status)),
        scheduler_(recognizer_, grparse::PageScheduler::Options{}),
        peers_(start({&office_, &calamine_}, &peer_port_)),
        parser_(scheduler_, endpoints()),
        streaming_(scheduler_, endpoints()),
        server_(start({&parser_, &streaming_}, &port_)) {}

  ~OfficeRig() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
    peers_->Shutdown(std::chrono::system_clock::now() + 2s);
    peers_->Wait();
  }

  std::unique_ptr<parsev1::ParseService::Stub> unary() const {
    return parsev1::ParseService::NewStub(channel());
  }

  std::unique_ptr<parsev1::ParseStreamingService::Stub> streaming() const {
    return parsev1::ParseStreamingService::NewStub(channel());
  }

  int office_calls() const { return office_.calls.load(); }
  int calamine_opens() const { return calamine_.opens.load(); }

 private:
  // Both office collectors dial the one peer server, which is up before
  // the gRParse services are built (member order).
  std::shared_ptr<grparse::CollectorEndpoints> endpoints() const {
    grparse::CollectorTargets targets;
    targets.libreoffice = "127.0.0.1:" + std::to_string(peer_port_);
    targets.calamine = targets.libreoffice;
    return std::make_shared<grparse::CollectorEndpoints>(targets);
  }

  static std::unique_ptr<grpc::Server> start(std::vector<grpc::Service*> services, int* port) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
    for (grpc::Service* service : services) builder.RegisterService(service);
    auto server = builder.BuildAndStart();
    if (!server || *port == 0) throw std::runtime_error("office fallback rig failed to start");
    return server;
  }

  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                               grpc::InsecureChannelCredentials());
  }

  FailingOfficeService office_;
  OneSheetCalamineService calamine_;
  UnusedRecognizer recognizer_;
  grparse::PageScheduler scheduler_;
  int peer_port_ = 0;
  std::unique_ptr<grpc::Server> peers_;
  grparse::DocumentParserService parser_;
  grparse::DocumentStreamingService streaming_;
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

const grpc::Status kCorrupt(grpc::StatusCode::INVALID_ARGUMENT,
                            "import filter rejected the document: corrupt zip directory");

grpc::Status convert(const OfficeRig& rig, const std::string& filename,
                     parsev1::ConvertSourceResponse* response) {
  parsev1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename(filename);
  const std::string bytes = "PK\x03\x04 not really an office file";
  source->set_base64_string(grparse::encode_base64(bytes.data(), bytes.size()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 20s);
  return rig.unary()->ConvertSource(&context, request, response);
}

struct StreamResult {
  grpc::Status status;
  std::vector<parsev1::DocumentStreamEvent> events;
};

StreamResult stream(const OfficeRig& rig, const std::string& filename) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 20s);
  auto call = rig.streaming()->StreamProcessDocument(&context);
  parsev1::DocumentChunk chunk;
  chunk.set_document_id("fallback");
  chunk.set_filename(filename);
  chunk.set_data("PK\x03\x04 not really an office file");
  chunk.set_complete(true);
  require(call->Write(chunk), "the stream accepts the upload");
  call->WritesDone();
  StreamResult result;
  parsev1::DocumentStreamEvent event;
  while (call->Read(&event)) result.events.push_back(event);
  result.status = call->Finish();
  return result;
}

// The unary surface: the request fails with the leg's own status class and a
// message naming the collector and the cause. No other leg runs.
void verify_unary_docx_failure_is_typed() {
  OfficeRig rig(kCorrupt);
  parsev1::ConvertSourceResponse response;
  const grpc::Status status = convert(rig, "report.docx", &response);
  require(!status.ok(), "a docx whose only collector failed fails the request");
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "the libreoffice leg's status class is the request's: " + status.error_message());
  require(status.error_message().starts_with("libreoffice: ") &&
              status.error_message().contains("corrupt zip directory"),
          "the message names the collector and the cause: " + status.error_message());
  require(rig.office_calls() == 1, "the libreoffice leg ran once");
  require(rig.calamine_opens() == 0, "a docx gains no calamine leg");
}

// A worker crash surfaces the way every collector's transport-class failure
// does: UNAVAILABLE, still naming libreoffice and the cause.
void verify_unary_pptx_crash_is_unavailable() {
  OfficeRig rig(grpc::Status(grpc::StatusCode::INTERNAL, "office worker exited"));
  parsev1::ConvertSourceResponse response;
  const grpc::Status status = convert(rig, "deck.pptx", &response);
  require(status.error_code() == grpc::StatusCode::UNAVAILABLE,
          "an internal worker failure maps to UNAVAILABLE: " + status.error_message());
  require(status.error_message().contains("libreoffice") &&
              status.error_message().contains("office worker exited"),
          "the message names the collector and the cause: " + status.error_message());
}

// The streaming surface: the stream ends with the same status class and no
// collector document and no completion event ahead of it.
void verify_streaming_docx_failure_is_typed() {
  OfficeRig rig(kCorrupt);
  const StreamResult result = stream(rig, "report.docx");
  require(result.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "the stream fails with the leg's status class: " + result.status.error_message());
  require(result.status.error_message().contains("libreoffice") &&
              result.status.error_message().contains("corrupt zip directory"),
          "the stream's message names the collector and the cause: " +
              result.status.error_message());
  for (const auto& event : result.events) {
    require(!event.has_collector_document() && !event.has_complete(),
            "a failed parse streams no document and no completion");
  }
  require(rig.calamine_opens() == 0, "a streamed docx gains no calamine leg");
}

// A workbook keeps calamine beside libreoffice: the request succeeds
// partially, the body is calamine's sheet, and the libreoffice failure is on
// the response under its collector.
void verify_unary_workbook_falls_back_to_calamine() {
  OfficeRig rig(kCorrupt);
  parsev1::ConvertSourceResponse response;
  const grpc::Status status = convert(rig, "ledger.xlsx", &response);
  require(status.ok(), "a workbook with a live calamine leg parses: " + status.error_message());
  const auto& converted = response.response();
  require(converted.status() == parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS,
          "the libreoffice failure makes the parse a partial success");
  require(converted.errors_size() == 1 &&
              converted.errors(0).module_name() == "collector:libreoffice" &&
              converted.errors(0).error_message().contains("corrupt zip directory") &&
              converted.errors(0).category() == parsev1::FAILURE_CATEGORY_BACKEND_FAILURE,
          "the libreoffice failure is reported under its collector");
  const auto& document = converted.document().doc();
  require(document.tables_size() == 1 &&
              document.tables(0).data().table_cells_size() == 4,
          "calamine's sheet is the document body");
  require(rig.calamine_opens() == 1, "the calamine leg ran once");
}

// The same workbook over the stream: calamine's collector document, then the
// completion event carrying the libreoffice failure.
void verify_streaming_workbook_falls_back_to_calamine() {
  OfficeRig rig(kCorrupt);
  const StreamResult result = stream(rig, "ledger.xlsx");
  require(result.status.ok(), "the workbook stream succeeds: " + result.status.error_message());
  bool calamine_document = false;
  const parsev1::DocumentComplete* complete = nullptr;
  for (const auto& event : result.events) {
    if (event.has_collector_document() &&
        event.collector_document().collector() == parsev1::COLLECTOR_CALAMINE) {
      calamine_document = event.collector_document().document().tables_size() == 1;
    }
    if (event.has_complete()) complete = &event.complete();
  }
  require(calamine_document, "calamine's sheet streams as its collector document");
  require(complete != nullptr && complete->collector_failures_size() == 1 &&
              complete->collector_failures(0).collector() == parsev1::COLLECTOR_LIBREOFFICE &&
              complete->collector_failures(0).error().contains("corrupt zip directory"),
          "the completion carries the libreoffice failure");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("office fallback test", "passed", {
      verify_unary_docx_failure_is_typed,
      verify_unary_pptx_crash_is_unavailable,
      verify_streaming_docx_failure_is_typed,
      verify_unary_workbook_falls_back_to_calamine,
      verify_streaming_workbook_falls_back_to_calamine,
  });
}
