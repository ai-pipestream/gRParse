// Proves the chart derender leg against an in-process fake EnrichService:
// candidate selection (verdict, pixels, no office charts), the request
// shape on the wire (options first, crops, then the completing chunk), the
// fold (typed annotation, meta created_by, GenerationSource), the counters,
// the skip and empty-table paths, the deadline path, an unreachable peer,
// and the off-by-default path in which nothing is dialed.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/enrich/v1/enrich_service.grpc.pb.h"
#include "grparse/base64.h"
#include "grparse/chart_derender.h"
#include "grparse/data_totals.h"
#include "grparse/document_parser_service.h"

namespace docv1 = ai::pipestream::document::v1;
namespace enrichv1 = ai::pipestream::enrich::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

const std::string kPng = "\x89PNG\r\n\x1a\nfake-chart-pixels";

docv1::PictureItem* add_picture(docv1::Document* document, const std::string& verdict,
                                bool with_image) {
  docv1::PictureItem* picture = document->add_pictures();
  picture->set_self_ref("#/pictures/" + std::to_string(document->pictures_size() - 1));
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  document->mutable_body()->add_children()->set_ref(picture->self_ref());
  if (!verdict.empty()) {
    docv1::PictureClassificationData* classification =
        picture->add_annotations()->mutable_classification();
    classification->set_kind("classification");
    classification->set_provenance("figure-classifier");
    docv1::PictureClassificationClass* top = classification->add_predicted_classes();
    top->set_class_name(verdict);
    top->set_confidence(0.9);
    docv1::PictureClassificationClass* second = classification->add_predicted_classes();
    second->set_class_name("other");
    second->set_confidence(0.1);
  }
  if (with_image) {
    docv1::ImageRef* image = picture->mutable_image();
    image->set_mimetype("image/png");
    image->set_uri("data:image/png;base64," + grparse::encode_base64(kPng.data(), kPng.size()));
  }
  return picture;
}

// The document the leg sees after a parse: a raster bar chart with pixels,
// a photo, an office chart already bound to its typed table, and a chart
// verdict whose picture carries no pixels.
docv1::Document sample_document() {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.set_name("charts.png");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  add_picture(&document, "bar_chart", true);
  add_picture(&document, "other", true);
  docv1::PictureItem* office = add_picture(&document, "", true);
  office->set_label(docv1::DOC_ITEM_LABEL_CHART);
  office->add_annotations()->mutable_tabular_chart()->set_kind("tabular_chart_data");
  add_picture(&document, "pie_chart", false);
  return document;
}

enrichv1::ChartTable canned_table(const std::string& title) {
  enrichv1::ChartTable chart;
  chart.set_title(title);
  chart.set_csv("Region,Q1\nNorth,120\nSouth,80\n");
  docv1::TableData* table = chart.mutable_table();
  const char* texts[3][2] = {{"Region", "Q1"}, {"North", "120"}, {"South", "80"}};
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 2; column++) {
      docv1::TableCell* cell = table->add_table_cells();
      cell->set_start_row_offset_idx(row);
      cell->set_end_row_offset_idx(row + 1);
      cell->set_start_col_offset_idx(column);
      cell->set_end_col_offset_idx(column + 1);
      cell->set_row_span(1);
      cell->set_col_span(1);
      cell->set_text(texts[row][column]);
      if (row == 0) cell->set_column_header(true);
      if (row > 0 && column == 1) cell->mutable_value()->set_number(std::atof(texts[row][column]));
    }
  }
  return chart;
}

enum class FakeMode { kAnswer, kSkip, kEmptyTable, kSlow };

class FakeEnrichService final : public enrichv1::EnrichService::Service {
 public:
  explicit FakeEnrichService(FakeMode mode) : mode_(mode) {}

  grpc::Status EnrichDocument(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<enrichv1::EnrichDocumentResponse, enrichv1::EnrichDocumentRequest>*
          stream) override {
    enrichv1::EnrichDocumentRequest request;
    Seen seen;
    bool first = true;
    while (stream->Read(&request)) {
      if (first) {
        seen.options_first = request.has_options();
        first = false;
      }
      if (request.has_options()) {
        seen.options = request.options();
      } else if (request.has_image()) {
        seen.images_before_complete = seen.images_before_complete && !seen.complete_seen;
        seen.image_refs.push_back(request.image().self_ref());
        seen.image_bytes.push_back(request.image().data());
      } else if (request.has_chunk()) {
        seen.chunk_bytes += request.chunk().data();
        if (request.chunk().complete()) seen.complete_seen = true;
      }
    }
    seen.chunk_parsed = seen.document.ParseFromString(seen.chunk_bytes);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_ = seen;
    }
    ++calls_;
    if (mode_ == FakeMode::kSlow) {
      // Past any client deadline this test sets; the client must not wait.
      for (int tick = 0; tick < 150 && !context->IsCancelled(); tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return grpc::Status::OK;
    }
    enrichv1::EnrichDocumentResponse event;
    event.mutable_started()->set_chart_extractions(seen.document.pictures_size());
    stream->Write(event);
    for (const docv1::PictureItem& picture : seen.document.pictures()) {
      event.Clear();
      if (mode_ == FakeMode::kSkip) {
        event.mutable_skipped()->set_self_ref(picture.self_ref());
        event.mutable_skipped()->set_reason(enrichv1::SKIP_REASON_VLM_ERROR);
        event.mutable_skipped()->set_detail("endpoint answered 503");
      } else {
        enrichv1::ItemAnnotation* annotation = event.mutable_annotation();
        annotation->set_self_ref(picture.self_ref());
        annotation->set_model("fake-vlm");
        *annotation->mutable_chart_table() =
            mode_ == FakeMode::kEmptyTable ? enrichv1::ChartTable() : canned_table("Revenue by region");
      }
      stream->Write(event);
    }
    event.Clear();
    event.mutable_complete()->set_succeeded(mode_ == FakeMode::kAnswer ? seen.document.pictures_size() : 0);
    stream->Write(event);
    return grpc::Status::OK;
  }

  struct Seen {
    bool options_first = false;
    bool images_before_complete = true;
    bool complete_seen = false;
    bool chunk_parsed = false;
    enrichv1::EnrichOptions options;
    std::vector<std::string> image_refs;
    std::vector<std::string> image_bytes;
    std::string chunk_bytes;
    docv1::Document document;
  };

  Seen seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }
  int calls() const { return calls_.load(); }

 private:
  const FakeMode mode_;
  mutable std::mutex mutex_;
  Seen seen_;
  std::atomic<int> calls_{0};
};

class ServerFixture {
 public:
  explicit ServerFixture(grpc::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) {
      throw std::runtime_error("fake enrich server failed to start");
    }
  }
  ~ServerFixture() {
    if (server_ != nullptr) server_->Shutdown();
  }
  std::string target() const { return "127.0.0.1:" + std::to_string(port_); }
  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel(target(), grpc::InsecureChannelCredentials());
  }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

grparse::ChartDerenderOptions options_for(const std::string& target,
                                          std::chrono::milliseconds timeout =
                                              std::chrono::milliseconds(5000)) {
  grparse::ChartDerenderOptions options;
  options.target = target;
  options.timeout = timeout;
  options.vlm_endpoint = "http://vlm.test:8085";
  return options;
}

void verify_candidates_need_a_chart_verdict_pixels_and_no_typed_table() {
  const docv1::Document document = sample_document();
  const std::vector<grparse::ChartCandidate> candidates =
      grparse::chart_derender_candidates(document);
  require(candidates.size() == 1, "only the raster chart with pixels is a candidate");
  require(candidates[0].self_ref == "#/pictures/0" && candidates[0].picture_index == 0,
          "the candidate keeps its arena identity");
  require(candidates[0].mimetype == "image/png" && candidates[0].bytes == kPng,
          "the data URI decodes back to the original bytes");
  const docv1::Document request = grparse::chart_derender_request_document(document, candidates);
  require(request.pictures_size() == 1 && request.pictures(0).self_ref() == "#/pictures/0" &&
              request.pictures(0).image().uri().empty() &&
              request.pictures(0).annotations_size() == 1 &&
              request.body().children_size() == 1,
          "the request carries the candidate without its pixels and lists it under the body");
  require(grparse::chart_derender_candidates(request).empty(),
          "a stripped picture is not a candidate again: the pixels travel separately");
}

void verify_fold_attributes_the_table_to_the_model() {
  docv1::Document document = sample_document();
  enrichv1::ItemAnnotation annotation;
  annotation.set_self_ref("#/pictures/0");
  annotation.set_model("granite-vision");
  *annotation.mutable_chart_table() = canned_table("Revenue by region");
  annotation.mutable_chart_table()->mutable_table()->set_num_rows(0);
  require(grparse::fold_chart_table(annotation, "http://vlm.test:8085", &document),
          "a table for a known picture folds");
  const docv1::PictureItem& picture = document.pictures(0);
  const docv1::PictureTabularChartData* tabular = nullptr;
  for (const docv1::PictureAnnotation& one : picture.annotations()) {
    if (one.has_tabular_chart()) tabular = &one.tabular_chart();
  }
  require(tabular != nullptr && tabular->kind() == "tabular_chart_data" &&
              tabular->title() == "Revenue by region" && tabular->chart_data().num_rows() == 3 &&
              tabular->chart_data().num_cols() == 2 && tabular->chart_data().table_cells_size() == 6,
          "the typed annotation carries the title and the cells with the extent stated");
  require(tabular->chart_data().table_cells(3).value().number() == 120,
          "numeric cells keep their typed value");
  require(picture.meta().tabular_chart().created_by() == "granite-vision" &&
              picture.meta().tabular_chart().title() == "Revenue by region" &&
              picture.meta().tabular_chart().chart_data().table_cells_size() == 6,
          "meta names the model that produced the table");
  require(picture.source_size() == 1 && picture.source(0).has_generation() &&
              picture.source(0).generation().model() == "granite-vision" &&
              picture.source(0).generation().endpoint() == "http://vlm.test:8085",
          "provenance is a typed GenerationSource, not a keyed string");
  require(picture.meta().custom_fields().empty(), "no custom_fields keys");
  require(grparse::chart_derender_candidates(document).empty(),
          "a folded picture is no longer a candidate");

  enrichv1::ItemAnnotation stranger = annotation;
  stranger.set_self_ref("#/pictures/9");
  require(!grparse::fold_chart_table(stranger, "", &document), "an unknown self_ref folds nothing");
  enrichv1::ItemAnnotation empty = annotation;
  empty.set_self_ref("#/pictures/1");
  empty.mutable_chart_table()->clear_table();
  require(!grparse::fold_chart_table(empty, "", &document) &&
              document.pictures(1).annotations_size() == 1,
          "an empty table folds nothing and leaves the picture untouched");
}

void verify_leg_dials_folds_and_counts() {
  FakeEnrichService fake(FakeMode::kAnswer);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  const uint64_t before = grparse::data_totals().charts_derendered;
  const uint64_t skipped_before = grparse::data_totals().chart_derender_skipped;
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(server.channel(), options_for(server.target()), &document);
  require(report.candidates == 1 && report.derendered == 1 && report.skipped == 0,
          "one candidate, one table, nothing skipped: " + std::to_string(report.warnings.size()));
  require(report.warnings.empty(), "a clean leg has no warnings");
  require(grparse::data_totals().charts_derendered == before + 1 &&
              grparse::data_totals().chart_derender_skipped == skipped_before,
          "the counters move by the folded count only");
  const FakeEnrichService::Seen seen = fake.seen();
  require(seen.options_first && seen.options.do_chart_extraction() &&
              !seen.options.do_picture_description() && seen.options.timeout_seconds() == 5 &&
              seen.options.vlm_endpoint() == "http://vlm.test:8085" && !seen.options.return_document(),
          "options lead the stream and ask for chart extraction only");
  require(seen.chunk_parsed, "the completing chunk parses as a Document");
  require(seen.images_before_complete && seen.complete_seen && seen.image_refs.size() == 1 &&
              seen.image_refs[0] == "#/pictures/0" && seen.image_bytes[0] == kPng,
          "the crop arrives before the completing chunk, keyed by self_ref");
  require(seen.document.pictures_size() == 1 && seen.document.pictures(0).image().uri().empty(),
          "the peer receives only the candidate, stripped of inline pixels");
  require(document.pictures(0).meta().tabular_chart().created_by() == "fake-vlm" &&
              document.pictures(0).source_size() == 1,
          "the fold landed on the original document");
  require(document.pictures(1).annotations_size() == 1 && document.pictures(3).annotations_size() == 1,
          "the other pictures are untouched");
  require(document.texts_size() == 0 && document.tables_size() == 0,
          "the leg adds annotations only, never items");
}

void verify_skip_events_and_empty_tables_count_as_skipped() {
  for (FakeMode mode : {FakeMode::kSkip, FakeMode::kEmptyTable}) {
    FakeEnrichService fake(mode);
    ServerFixture server(&fake);
    docv1::Document document = sample_document();
    const std::string before = document.SerializeAsString();
    const uint64_t skipped_before = grparse::data_totals().chart_derender_skipped;
    const grparse::ChartDerenderReport report =
        grparse::derender_charts(server.channel(), options_for(server.target()), &document);
    require(report.candidates == 1 && report.derendered == 0 && report.skipped == 1,
            "a skip event or an empty table is one skipped candidate");
    require(report.warnings.size() == 1 && report.warnings[0].contains("#/pictures/0"),
            "the skip names the picture: " +
                (report.warnings.empty() ? std::string("no warning") : report.warnings[0]));
    if (mode == FakeMode::kSkip) {
      require(report.warnings[0].contains("SKIP_REASON_VLM_ERROR") &&
                  report.warnings[0].contains("503"),
              "the peer's reason and detail survive");
    }
    require(grparse::data_totals().chart_derender_skipped == skipped_before + 1,
            "the skipped counter moves by one");
    require(document.SerializeAsString() == before, "the document is byte-identical");
  }
}

void verify_deadline_bounds_the_leg_and_never_fails_the_document() {
  FakeEnrichService fake(FakeMode::kSlow);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  const std::string before = document.SerializeAsString();
  const auto started = std::chrono::steady_clock::now();
  const grparse::ChartDerenderReport report = grparse::derender_charts(
      server.channel(), options_for(server.target(), std::chrono::milliseconds(300)), &document);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(elapsed < std::chrono::milliseconds(1200),
          "the leg returns at its own deadline, not the peer's pace");
  require(report.candidates == 1 && report.derendered == 0 && report.skipped == 1,
          "a timed-out candidate is skipped");
  require(!report.warnings.empty() && report.warnings[0].contains("DEADLINE_EXCEEDED"),
          "the timeout is reported as such: " +
              (report.warnings.empty() ? std::string("no warning") : report.warnings[0]));
  require(document.SerializeAsString() == before, "a timeout leaves the document untouched");
  // The inbound call's own deadline caps the leg even when the configured
  // timeout is generous.
  docv1::Document again = sample_document();
  const auto started_again = std::chrono::steady_clock::now();
  grparse::derender_charts(server.channel(), options_for(server.target()), &again,
                           std::chrono::system_clock::now() + std::chrono::milliseconds(200));
  require(std::chrono::steady_clock::now() - started_again < std::chrono::milliseconds(1200),
          "the inbound deadline wins over the leg's own timeout");
}

void verify_unreachable_peer_is_a_warning() {
  docv1::Document document = sample_document();
  const std::string before = document.SerializeAsString();
  // Port 1 refuses; wait_for_ready is off so the call fails fast.
  auto channel = grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
  const auto started = std::chrono::steady_clock::now();
  const grparse::ChartDerenderReport report = grparse::derender_charts(
      channel, options_for("127.0.0.1:1", std::chrono::milliseconds(2000)), &document);
  require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2500),
          "an unreachable peer does not hold the parse past the leg's timeout");
  require(report.derendered == 0 && report.skipped == 1 && !report.warnings.empty(),
          "the failure is one skipped candidate and a warning");
  require(document.SerializeAsString() == before, "the document is untouched");
}

void verify_off_by_default_dials_nothing() {
  grparse::CollectorTargets targets;
  require(!targets.derender.enabled(), "no target, no leg");
  grparse::CollectorEndpoints endpoints(targets);
  require(!endpoints.has_derender() && endpoints.enrich_channel() == nullptr,
          "unconfigured endpoints hand out no enrich channel");
  docv1::Document document = sample_document();
  const std::string before = document.SerializeAsString();
  const uint64_t skipped_before = grparse::data_totals().chart_derender_skipped;
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(nullptr, grparse::ChartDerenderOptions{}, &document);
  require(report.candidates == 1 && report.derendered == 0 && report.skipped == 1 &&
              report.warnings.size() == 1 && report.warnings[0].contains("GRPARSE_ENRICH_TARGET"),
          "calling the leg without a target names the variable and skips");
  require(grparse::data_totals().chart_derender_skipped == skipped_before + 1,
          "the skip is counted");
  require(document.SerializeAsString() == before, "nothing changed");
  docv1::Document plain;
  plain.mutable_body()->set_self_ref("#/body");
  const grparse::ChartDerenderReport nothing =
      grparse::derender_charts(nullptr, grparse::ChartDerenderOptions{}, &plain);
  require(nothing.candidates == 0 && nothing.warnings.empty(),
          "a document without chart candidates is not even a warning");

  grparse::CollectorTargets configured;
  configured.derender.target = "enrich:50056";
  grparse::CollectorEndpoints wired(configured);
  require(wired.has_derender() && wired.enrich_channel() != nullptr &&
              wired.enrich_channel() == wired.enrich_channel(),
          "a configured target gets one lazily created channel");
}

}  // namespace

int main() {
  try {
    verify_candidates_need_a_chart_verdict_pixels_and_no_typed_table();
    verify_fold_attributes_the_table_to_the_model();
    verify_leg_dials_folds_and_counts();
    verify_skip_events_and_empty_tables_count_as_skipped();
    verify_deadline_bounds_the_leg_and_never_fails_the_document();
    verify_unreachable_peer_is_a_warning();
    verify_off_by_default_dials_nothing();
    std::println("chart-derender-test: all checks passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "chart-derender-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
