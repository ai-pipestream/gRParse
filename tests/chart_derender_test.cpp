// Proves the chart derender leg against an in-process fake EnrichService:
// candidate selection (verdict, pixels, no office charts), the request
// shape on the wire (options first, crops, then the completing chunk), the
// fold (typed annotation, meta created_by, GenerationSource), the counters,
// the skip and empty-table paths, the deadline path, an unreachable peer,
// and the off-by-default path in which nothing is dialed.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include "../src/source_parse.h"
#include "ai/pipestream/enrich/v1/enrich_service.grpc.pb.h"
#include "grparse/base64.h"
#include "grparse/chart_derender.h"
#include "grparse/data_totals.h"
#include "grparse/document_parser_service.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace enrichv1 = ai::pipestream::enrich::v1;

namespace {

using grparse_test::require;

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

enum class FakeMode { kAnswer, kSkip, kEmptyTable, kSlow, kDuplicate, kOutputs, kSummarySkipped };

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
      if (mode_ == FakeMode::kOutputs || mode_ == FakeMode::kSummarySkipped) {
        // One event per chart output, the way grpc-enrich answers a request
        // carrying ChartExtractionOptions.
        enrichv1::ItemAnnotation* table = event.mutable_annotation();
        table->set_self_ref(picture.self_ref());
        table->set_model("chart-vlm");
        *table->mutable_chart_table() = canned_table("Revenue by region");
        stream->Write(event);
        event.Clear();
        if (mode_ == FakeMode::kSummarySkipped) {
          event.mutable_skipped()->set_self_ref(picture.self_ref());
          event.mutable_skipped()->set_reason(enrichv1::SKIP_REASON_VLM_ERROR);
          event.mutable_skipped()->set_chart_output(enrichv1::CHART_OUTPUT_SUMMARY);
          event.mutable_skipped()->set_detail("chart model returned an empty summary");
        } else {
          enrichv1::ItemAnnotation* summary = event.mutable_annotation();
          summary->set_self_ref(picture.self_ref());
          summary->set_model("chart-vlm");
          summary->mutable_chart_summary()->set_text("Revenue is higher in the North.");
        }
        stream->Write(event);
        event.Clear();
        enrichv1::ItemAnnotation* code = event.mutable_annotation();
        code->set_self_ref(picture.self_ref());
        code->set_model("chart-vlm");
        code->mutable_chart_code()->set_text("import matplotlib.pyplot as plt");
        code->mutable_chart_code()->set_language(docv1::CODE_LANGUAGE_LABEL_PYTHON);
        stream->Write(event);
        continue;
      }
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
      // A peer that answers the same picture twice (a retry that also
      // delivered the first answer): the second table must not count.
      if (mode_ == FakeMode::kDuplicate) stream->Write(event);
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
  docv1::Document malformed = document;
  malformed.mutable_pictures(0)->mutable_image()->set_uri("data:image/png;base64,@@@");
  require(grparse::chart_derender_candidates(malformed).empty(),
          "a data URI that is not base64 skips its picture instead of throwing");
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

void verify_fold_picture_description_and_code() {
  docv1::Document document = sample_document();
  enrichv1::ItemAnnotation description;
  description.set_self_ref("#/pictures/1");
  description.set_model("granite-vision");
  description.mutable_description()->set_text("A photo of a circuit board");
  require(grparse::fold_picture_description(description, "http://vlm.test:8085", &document),
          "a description for a known picture folds");
  require(document.pictures(1).meta().description().text() == "A photo of a circuit board" &&
              document.pictures(1).meta().description().created_by() == "granite-vision",
          "meta description names the model");
  require(!grparse::fold_picture_description(description, "", &document),
          "a second description does not overwrite");

  auto* code = document.add_texts()->mutable_code();
  code->set_self_ref("#/texts/0");
  code->set_text("print(1)");
  enrichv1::ItemAnnotation code_ann;
  code_ann.set_self_ref("#/texts/0");
  code_ann.mutable_code()->set_text("print(42)");
  code_ann.mutable_code()->set_language(docv1::CODE_LANGUAGE_LABEL_PYTHON);
  require(grparse::fold_code_annotation(code_ann, &document), "code enrichment folds");
  require(document.texts(0).code().text() == "print(42)" &&
              document.texts(0).code().code_language() == docv1::CODE_LANGUAGE_LABEL_PYTHON,
          "code text and language update");

  auto* formula = document.add_texts()->mutable_formula();
  formula->mutable_base()->set_self_ref("#/texts/1");
  formula->mutable_base()->set_text("x");
  enrichv1::ItemAnnotation formula_ann;
  formula_ann.set_self_ref("#/texts/1");
  formula_ann.mutable_formula()->set_text("x^2");
  require(grparse::fold_formula_annotation(formula_ann, &document), "formula enrichment folds");
  require(document.texts(1).formula().base().text() == "x^2", "formula text updates");
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

void verify_picture_description_engine_fields_reach_enrich() {
  FakeEnrichService fake(FakeMode::kAnswer);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  grparse::ChartDerenderOptions options = options_for(server.target());
  options.do_chart_extraction = false;
  options.do_picture_description = true;
  options.picture_description_preset_raw = "ibm-granite/granite-vision";
  options.vlm_endpoint = "http://api.vlm:9000/v1";
  options.concurrency = 3;
  options.timeout = std::chrono::milliseconds(2500);
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(server.channel(), options, &document);
  require(report.warnings.size() <= 1, "description-only dial may warn on empty describe set");
  const FakeEnrichService::Seen seen = fake.seen();
  require(seen.options.do_picture_description() && !seen.options.do_chart_extraction() &&
              seen.options.picture_description_preset_raw() == "ibm-granite/granite-vision" &&
              seen.options.vlm_endpoint() == "http://api.vlm:9000/v1" &&
              seen.options.concurrency() == 3 && seen.options.timeout_seconds() == 3,
          "local repo_id and api url/concurrency/timeout reach enrich options");
}

void verify_picture_description_class_filters() {
  docv1::Document document = sample_document();
  // sample_document picture 0 is bar_chart with pixels; others vary.
  grparse::ChartDerenderOptions options;
  options.do_picture_description = true;
  options.do_chart_extraction = false;
  options.picture_description_allow = {"photograph"};
  // Force a path that builds candidates without dialing: empty target.
  const grparse::ChartDerenderReport denied =
      grparse::derender_charts(nullptr, options, &document);
  require(denied.candidates == 0 && denied.warnings.empty(),
          "allow=photograph excludes bar_chart candidates before dial");

  options.picture_description_allow = {"bar_chart"};
  options.picture_description_min_confidence = 0.99;
  // sample top confidence is 0.9 — below the floor.
  const grparse::ChartDerenderReport low =
      grparse::derender_charts(nullptr, options, &document);
  require(low.candidates == 0,
          "min confidence above the top class score excludes the picture");

  options.picture_description_min_confidence = 0.5;
  options.target = "enrich:unused";
  const grparse::ChartDerenderReport ready =
      grparse::derender_charts(nullptr, options, &document);
  require(ready.candidates == 0 && ready.warnings.size() == 1 &&
              ready.warnings[0].contains("GRPARSE_ENRICH_TARGET"),
          "allow=bar_chart at 0.5 selects the chart but skips without a channel");
}

// Everything the process writes to stdout and stderr while `body` runs.
template <typename Body>
std::string captured_output(Body body) {
  std::fflush(stdout);
  std::fflush(stderr);
  char path[] = "/tmp/chart-derender-output-XXXXXX";
  const int file = mkstemp(path);
  if (file < 0) throw std::runtime_error("mkstemp failed");
  const int saved_out = dup(STDOUT_FILENO);
  const int saved_err = dup(STDERR_FILENO);
  dup2(file, STDOUT_FILENO);
  dup2(file, STDERR_FILENO);
  body();
  std::fflush(stdout);
  std::fflush(stderr);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_err, STDERR_FILENO);
  close(saved_out);
  close(saved_err);
  close(file);
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  std::remove(path);
  return text.str();
}

// Docling's picture_description_api prompt, params and headers reach
// grpc-enrich as the typed EnrichOptions fields, resolved by the same
// function validate_options runs; a header value never shows up in a
// warning or on the process's output, with the data log on.
void verify_picture_description_api_call_reaches_enrich() {
  namespace parsev1 = ai::pipestream::parse::v1;
  const std::string secret = "Bearer sk-never-logged-4711";
  parsev1::PictureDescriptionApi api;
  api.set_url("http://api.vlm:9000/v1");
  api.set_prompt("List every object in the picture.");
  (*api.mutable_params())["model"].set_string_value("granite-vision");
  (*api.mutable_params())["max_completion_tokens"].set_int_value(512);
  (*api.mutable_params())["temperature"].set_double_value(0.2);
  (*api.mutable_params())["top_p"].set_double_value(0.9);
  (*api.mutable_params())["seed"].set_int_value(7);
  (*api.mutable_headers())["X-Tenant"] = "acme";
  (*api.mutable_headers())["Authorization"] = secret;

  FakeEnrichService fake(FakeMode::kAnswer);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  grparse::ChartDerenderOptions options = options_for(server.target());
  options.do_chart_extraction = false;
  options.do_picture_description = true;
  options.vlm_endpoint = api.url();
  const grpc::Status status =
      grparse::picture_description_call(api, "ConvertSource", &options.picture_description_call);
  require(status.ok(), "the api's prompt, params and headers resolve: " + status.error_message());
  grparse::ChartDerenderReport report;
  const std::string output = captured_output(
      [&] { report = grparse::derender_charts(server.channel(), options, &document); });

  const enrichv1::EnrichOptions seen = fake.seen().options;
  require(seen.picture_description_prompt() == "List every object in the picture.",
          "the prompt reaches enrich");
  const enrichv1::VlmGenerationParams& params = seen.picture_description_params();
  require(seen.has_picture_description_params() && params.model() == "granite-vision" &&
              params.has_max_tokens() && params.max_tokens() == 512 &&
              params.has_temperature() && params.temperature() == 0.2 &&
              params.has_top_p() && params.top_p() == 0.9 && params.has_seed() &&
              params.seed() == 7,
          "the params reach enrich as typed fields, max_completion_tokens as max_tokens");
  require(seen.vlm_headers_size() == 2 && seen.vlm_headers(0).name() == "Authorization" &&
              seen.vlm_headers(0).value() == secret && seen.vlm_headers(1).name() == "X-Tenant" &&
              seen.vlm_headers(1).value() == "acme",
          "the headers reach enrich as VlmHeader entries in name order");
  require(seen.vlm_endpoint() == "http://api.vlm:9000/v1",
          "the headers travel with the per-request endpoint they are for");
  for (const std::string& warning : report.warnings) {
    require(!warning.contains("sk-never-logged"), "no warning carries a header value");
  }
  require(!output.contains("sk-never-logged"), "nothing written to stdout or stderr carries "
                                               "a header value");

  // No params and no headers send no message and no entries, so the enrich
  // preset's model and budget stay in place.
  grparse::PictureDescriptionCall plain;
  parsev1::PictureDescriptionApi bare;
  bare.set_url("http://api.vlm:9000/v1");
  require(grparse::picture_description_call(bare, "ConvertSource", &plain).ok() &&
              plain.prompt.empty() && !plain.params.has_value() && plain.headers.empty(),
          "an api without prompt, params or headers adds nothing");
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

// The leg ends with the inbound call: a cancelled caller cancels the enrich
// call instead of leaving it to run out its own timeout.
void verify_cancel_hook_ends_the_leg() {
  FakeEnrichService fake(FakeMode::kSlow);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  const auto started = std::chrono::steady_clock::now();
  const grparse::ChartDerenderReport report = grparse::derender_charts(
      server.channel(), options_for(server.target()), &document, grparse::kNoCollectorDeadline,
      [started] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(200); });
  require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000),
          "the leg returns once the caller is gone, not at the peer's pace");
  require(!report.warnings.empty() && report.warnings[0].contains("CANCELLED"),
          "the cancel is reported as such: " +
              (report.warnings.empty() ? std::string("no warning") : report.warnings[0]));
}

// A peer that answers each image before it reads the next: its answers fill
// this client's receive window, it stops reading, and an upload that sent
// every image before reading anything would stall until the deadline.
class InterleavingEnrichService final : public enrichv1::EnrichService::Service {
 public:
  grpc::Status EnrichDocument(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<enrichv1::EnrichDocumentResponse, enrichv1::EnrichDocumentRequest>*
          stream) override {
    enrichv1::EnrichDocumentRequest request;
    enrichv1::EnrichDocumentResponse event;
    while (stream->Read(&request)) {
      if (!request.has_image()) continue;
      event.Clear();
      event.mutable_skipped()->set_self_ref(request.image().self_ref());
      event.mutable_skipped()->set_reason(enrichv1::SKIP_REASON_VLM_ERROR);
      event.mutable_skipped()->set_detail(std::string(512U * 1024U, 'x'));
      if (!stream->Write(event)) break;
    }
    event.Clear();
    event.mutable_complete();
    stream->Write(event);
    return grpc::Status::OK;
  }
};

void verify_answers_are_read_while_images_go_out() {
  InterleavingEnrichService fake;
  ServerFixture server(&fake);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const std::string pixels = kPng + std::string(512U * 1024U, 'p');
  constexpr int kPictures = 40;
  for (int index = 0; index < kPictures; ++index) {
    add_picture(&document, "bar_chart", false)
        ->mutable_image()
        ->set_uri("data:image/png;base64," + grparse::encode_base64(pixels.data(), pixels.size()));
  }
  const auto started = std::chrono::steady_clock::now();
  const grparse::ChartDerenderReport report = grparse::derender_charts(
      server.channel(), options_for(server.target(), std::chrono::milliseconds(20000)), &document);
  require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(10000),
          "the leg finishes long before its deadline");
  require(report.candidates == kPictures && report.skipped == kPictures,
          "every image was answered: " + std::to_string(report.skipped));
  for (const std::string& warning : report.warnings) {
    require(!warning.contains("DEADLINE_EXCEEDED"), "the leg did not stall: " + warning.substr(0, 200));
  }
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

// A peer that sends two tables for one picture folds one: the picture ends
// with a single tabular annotation and a single GenerationSource, the report
// never goes negative, and the process skip counter does not move (a
// negative skip count cast to uint64 would wrap the exported metric).
void verify_duplicate_tables_fold_once_and_never_go_negative() {
  FakeEnrichService fake(FakeMode::kDuplicate);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  const uint64_t derendered_before = grparse::data_totals().charts_derendered;
  const uint64_t skipped_before = grparse::data_totals().chart_derender_skipped;
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(server.channel(), options_for(server.target()), &document);
  require(report.candidates == 1 && report.derendered == 1,
          "one candidate answered twice is one derendered chart, got " +
              std::to_string(report.derendered));
  require(report.skipped == 0, "a duplicate answer is not a skip, got skipped=" +
                                   std::to_string(report.skipped));
  require(grparse::data_totals().charts_derendered == derendered_before + 1 &&
              grparse::data_totals().chart_derender_skipped == skipped_before,
          "the counters move by one derendered chart and no skips");
  int tabular = 0;
  for (const docv1::PictureAnnotation& one : document.pictures(0).annotations()) {
    if (one.has_tabular_chart()) ++tabular;
  }
  require(tabular == 1, "the picture carries one tabular annotation, got " + std::to_string(tabular));
  require(document.pictures(0).source_size() == 1, "the picture carries one GenerationSource");
  require(report.warnings.size() == 1 && report.warnings[0].contains("#/pictures/0") &&
              report.warnings[0].contains("duplicate"),
          "the second table is reported as a duplicate: " +
              (report.warnings.empty() ? std::string("no warning") : report.warnings[0]));
}

// A picture the arena never named gets the positional self_ref on the wire,
// so the table the peer returns under that name has to land on it.
void verify_fold_lands_on_a_picture_without_self_ref() {
  docv1::Document document = sample_document();
  document.mutable_pictures(0)->clear_self_ref();
  const std::vector<grparse::ChartCandidate> candidates =
      grparse::chart_derender_candidates(document);
  require(candidates.size() == 1 && candidates[0].self_ref == "#/pictures/0",
          "an unnamed picture is sent under its positional self_ref");
  enrichv1::ItemAnnotation annotation;
  annotation.set_self_ref(candidates[0].self_ref);
  annotation.set_model("fake-vlm");
  *annotation.mutable_chart_table() = canned_table("Revenue by region");
  require(grparse::fold_chart_table(annotation, "", &document),
          "a table returned under the positional self_ref folds onto the unnamed picture");
  require(document.pictures(0).meta().tabular_chart().created_by() == "fake-vlm",
          "the fold landed on the picture that was sent");
  require(grparse::chart_derender_candidates(document).empty(),
          "the folded picture is no longer a candidate");
}

grparse::ChartExtractionPreset all_outputs_preset() {
  grparse::ChartExtractionPreset preset;
  preset.id = "charts";
  preset.model = "chart-vlm";
  preset.vlm_endpoint = "http://charts.test:9000";
  preset.chart2summary = true;
  preset.chart2code = true;
  preset.use_natural_language_prompts = true;
  return preset;
}

// The resolved preset travels as ChartExtractionOptions, and the table,
// summary and code that come back land in meta.tabular_chart,
// meta.description and meta.code, each created_by the model, with one
// GenerationSource naming the model and the preset's endpoint.
void verify_chart_outputs_reach_enrich_and_fold_into_meta() {
  FakeEnrichService fake(FakeMode::kOutputs);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  grparse::ChartDerenderOptions options = options_for(server.target());
  options.chart_extraction = all_outputs_preset();
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(server.channel(), options, &document);
  require(report.candidates == 1 && report.derendered == 1 && report.skipped == 0 &&
              report.chart_tables == 1 && report.chart_summaries == 1 && report.chart_codes == 1,
          "one chart, three outputs folded");
  require(report.warnings.empty(), "a complete answer has no warnings");
  const FakeEnrichService::Seen seen = fake.seen();
  require(seen.options.has_chart_extraction(), "the preset travels as ChartExtractionOptions");
  const enrichv1::ChartExtractionOptions& chart = seen.options.chart_extraction();
  require(chart.has_csv() && chart.csv() && chart.summary() && chart.code() &&
              chart.natural_language_prompts() && chart.model() == "chart-vlm" &&
              chart.vlm_endpoint() == "http://charts.test:9000",
          "outputs, prompt dialect, model and endpoint all reach enrich");
  require(seen.options.vlm_endpoint() == "http://vlm.test:8085",
          "the request endpoint stays for the other jobs");
  const docv1::PictureItem& picture = document.pictures(0);
  require(picture.meta().tabular_chart().created_by() == "chart-vlm" &&
              picture.meta().tabular_chart().chart_data().table_cells_size() == 6,
          "the table lands in meta.tabular_chart");
  require(picture.meta().description().text() == "Revenue is higher in the North." &&
              picture.meta().description().created_by() == "chart-vlm",
          "the summary lands in meta.description, attributed to the model");
  require(picture.meta().code().text() == "import matplotlib.pyplot as plt" &&
              picture.meta().code().language() == docv1::CODE_LANGUAGE_LABEL_PYTHON &&
              picture.meta().code().created_by() == "chart-vlm",
          "the code lands in meta.code as Python, attributed to the model");
  require(picture.source_size() == 1 && picture.source(0).generation().model() == "chart-vlm" &&
              picture.source(0).generation().endpoint() == "http://charts.test:9000",
          "one GenerationSource names the model and the chart endpoint");
}

// A summary the model could not produce is skipped on its own: the table
// and the code still land, the chart counts as derendered, and the warning
// names the output.
void verify_one_failed_output_keeps_the_others() {
  FakeEnrichService fake(FakeMode::kSummarySkipped);
  ServerFixture server(&fake);
  docv1::Document document = sample_document();
  grparse::ChartDerenderOptions options = options_for(server.target());
  options.chart_extraction = all_outputs_preset();
  const grparse::ChartDerenderReport report =
      grparse::derender_charts(server.channel(), options, &document);
  require(report.derendered == 1 && report.skipped == 0 && report.chart_tables == 1 &&
              report.chart_summaries == 0 && report.chart_codes == 1,
          "the chart is derendered without its summary");
  require(report.warnings.size() == 1 && report.warnings[0].contains("CHART_OUTPUT_SUMMARY") &&
              report.warnings[0].contains("empty summary"),
          "the warning names the skipped output: " +
              (report.warnings.empty() ? std::string("no warning") : report.warnings[0]));
  require(!document.pictures(0).meta().has_description() &&
              document.pictures(0).meta().code().text() == "import matplotlib.pyplot as plt",
          "no description, code present");
}

// The fold refuses to overwrite: a picture that already has a description
// or code keeps it.
void verify_summary_and_code_fold_once() {
  docv1::Document document = sample_document();
  enrichv1::ItemAnnotation summary;
  summary.set_self_ref("#/pictures/0");
  summary.set_model("chart-vlm");
  summary.mutable_chart_summary()->set_text("first");
  require(grparse::fold_chart_summary(summary, "", &document), "a summary folds");
  summary.mutable_chart_summary()->set_text("second");
  require(!grparse::fold_chart_summary(summary, "", &document) &&
              document.pictures(0).meta().description().text() == "first",
          "a second summary does not overwrite");
  enrichv1::ItemAnnotation code;
  code.set_self_ref("#/pictures/0");
  code.set_model("chart-vlm");
  code.mutable_chart_code()->set_text("plot()");
  require(grparse::fold_chart_code(code, "", &document) &&
              document.pictures(0).meta().code().language() == docv1::CODE_LANGUAGE_LABEL_PYTHON,
          "code folds as Python even when the peer leaves the language unset");
  code.mutable_chart_code()->set_text("other()");
  require(!grparse::fold_chart_code(code, "", &document) &&
              document.pictures(0).meta().code().text() == "plot()",
          "a second code does not overwrite");
  enrichv1::ItemAnnotation empty = code;
  empty.set_self_ref("#/pictures/1");
  empty.mutable_chart_code()->clear_text();
  require(!grparse::fold_chart_code(empty, "", &document), "empty code folds nothing");
  require(document.pictures(0).source_size() == 1,
          "the same model and endpoint is one GenerationSource");
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
  // The data log on, so the header-value check covers its lines too.
  setenv("GRPARSE_DATA_LOG", "on", 1);
  return grparse_test::run_test_main("chart-derender-test", "all checks passed", {
      verify_candidates_need_a_chart_verdict_pixels_and_no_typed_table,
      verify_fold_attributes_the_table_to_the_model,
      verify_fold_picture_description_and_code,
      verify_leg_dials_folds_and_counts,
      verify_picture_description_engine_fields_reach_enrich,
      verify_picture_description_class_filters,
      verify_cancel_hook_ends_the_leg,
      verify_answers_are_read_while_images_go_out,
      verify_skip_events_and_empty_tables_count_as_skipped,
      verify_deadline_bounds_the_leg_and_never_fails_the_document,
      verify_unreachable_peer_is_a_warning,
      verify_duplicate_tables_fold_once_and_never_go_negative,
      verify_fold_lands_on_a_picture_without_self_ref,
      verify_off_by_default_dials_nothing,
      verify_chart_outputs_reach_enrich_and_fold_into_meta,
      verify_one_failed_output_keeps_the_others,
      verify_summary_and_code_fold_once,
      verify_picture_description_api_call_reaches_enrich,
  });
}
