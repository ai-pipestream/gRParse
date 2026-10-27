#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <grpcpp/grpcpp.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/vlm/v1/vlm_convert.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_previews.h"
#include "grparse/vlm_convert.h"
#include "support/check.h"
#include "support/fake_pdf_backend.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;

class FakeVlmConvertService final : public vlmv1::VlmConvertService::Service {
 public:
  grpc::Status ConvertPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<vlmv1::ConvertPagesResponse, vlmv1::ConvertPagesRequest>*
          stream) override {
    vlmv1::ConvertPagesRequest request;
    Seen seen;
    bool first = true;
    while (stream->Read(&request)) {
      if (first) {
        seen.options_first = request.has_options();
        first = false;
      }
      if (request.has_options()) {
        seen.options = request.options();
      } else if (request.has_page_image()) {
        seen.page_nos.push_back(request.page_image().page_no());
        seen.page_bytes.push_back(request.page_image().png().size());
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_ = seen;
    }
    ++calls_;
    for (uint32_t page_no : seen.page_nos) {
      vlmv1::ConvertPagesResponse event;
      event.mutable_page_started()->set_page_no(page_no);
      stream->Write(event);
      event.Clear();
      auto* page = event.mutable_page_document();
      page->set_page_no(page_no);
      auto* text = page->mutable_document()->add_texts()->mutable_text();
      text->mutable_base()->set_self_ref("#/texts/" + std::to_string(page_no - 1));
      text->mutable_base()->set_text("page-" + std::to_string(page_no));
      // The peer names the generation with the provider's verbatim stop
      // reason only, the way a producer older than VlmStopReason does.
      auto* generation = text->mutable_base()->add_source()->mutable_generation();
      generation->set_model("granite-docling");
      generation->set_finish_reason("length");
      (*page->mutable_document()->mutable_pages())[page_no].set_page_no(page_no);
      auto* warning = page->add_warnings();
      warning->set_code(vlmv1::PAGE_WARNING_CODE_TABLE_TRUNCATED);
      warning->set_message("kept 500 of 90000 rows");
      warning->set_ref("#/tables/0");
      page->add_warnings()->set_code(static_cast<vlmv1::PageWarningCode>(77));
      stream->Write(event);
    }
    vlmv1::ConvertPagesResponse done;
    done.mutable_complete()->set_pages_started(seen.page_nos.size());
    done.mutable_complete()->set_pages_ok(seen.page_nos.size());
    stream->Write(done);
    return grpc::Status::OK;
  }

  struct Seen {
    bool options_first = false;
    vlmv1::ConvertOptions options;
    std::vector<uint32_t> page_nos;
    std::vector<size_t> page_bytes;
  };

  Seen seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }
  int calls() const { return calls_.load(); }

 private:
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
      throw std::runtime_error("fake vlm-convert server failed to start");
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

void verify_apply_maps_granite_docling_and_raw_api_url() {
  parsev1::ConvertDocumentOptions convert;
  convert.set_vlm_pipeline_model(parsev1::VLM_MODEL_TYPE_GRANITEDOCLING);
  convert.set_render_scale(2.0);
  convert.set_abort_on_error(true);
  convert.mutable_page_range()->set_start(1);
  convert.mutable_page_range()->set_end(2);
  grparse::VlmConvertOptions options;
  grparse::apply_vlm_convert_options(convert, &options);
  require(options.preset == vlmv1::VLM_PRESET_GRANITE_DOCLING, "granite-docling preset");
  require(options.render_dpi == 144.0, "render_scale 2.0 is 144 DPI");
  require(options.abort_on_error && options.page_range.has_value() &&
              options.page_range->first == 1 && options.page_range->second == 2,
          "abort and page_range copy");

  parsev1::ConvertDocumentOptions api;
  api.mutable_vlm_pipeline_model_api()->set_url("http://vlm.example:8080/v1");
  grparse::VlmConvertOptions api_options;
  grparse::apply_vlm_convert_options(api, &api_options);
  require(api_options.endpoint == "http://vlm.example:8080/v1", "http api url is endpoint");

  parsev1::ConvertDocumentOptions local;
  local.mutable_vlm_pipeline_model_local()->set_repo_id("ibm-granite/granite-vision");
  local.mutable_vlm_pipeline_model_local()->set_scale(1.5);
  grparse::VlmConvertOptions local_options;
  grparse::apply_vlm_convert_options(local, &local_options);
  require(local_options.preset == vlmv1::VLM_PRESET_RAW &&
              local_options.preset_raw == "ibm-granite/granite-vision" &&
              local_options.scale == 1.5,
          "typed local model maps repo_id and scale");
}

// Every docling 2.142.1 ResponseFormat has a member and a name; the five the
// collector maps reach it from local, api, and the custom config's spec, and
// the raw docling name stands in for an unset typed field.
void verify_response_formats_cover_docling_and_reach_the_collector() {
  const std::pair<parsev1::ResponseFormat, const char*> docling[] = {
      {parsev1::RESPONSE_FORMAT_DOCTAGS, "doctags"},
      {parsev1::RESPONSE_FORMAT_DOCLANG, "doclang"},
      {parsev1::RESPONSE_FORMAT_MARKDOWN, "markdown"},
      {parsev1::RESPONSE_FORMAT_DEEPSEEKOCR_MARKDOWN, "deepseekocr_markdown"},
      {parsev1::RESPONSE_FORMAT_UNLIMITED_OCR_MARKDOWN, "unlimited_ocr_markdown"},
      {parsev1::RESPONSE_FORMAT_HTML, "html"},
      {parsev1::RESPONSE_FORMAT_OTSL, "otsl"},
      {parsev1::RESPONSE_FORMAT_PLAINTEXT, "plaintext"},
      {parsev1::RESPONSE_FORMAT_CHANDRA_HTML, "chandra_html"},
      {parsev1::RESPONSE_FORMAT_DOTS_JSON, "dots_json"},
      {parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V2, "nemotron_parse_v2"},
      {parsev1::RESPONSE_FORMAT_NEMOTRON_PARSE_V3_5, "nemotron_parse_v3_5"},
      {parsev1::RESPONSE_FORMAT_MINERU2, "mineru2"},
  };
  require(parsev1::ResponseFormat_descriptor()->value_count() == 14,
          "ResponseFormat is UNSPECIFIED plus docling's 13 members");
  for (const auto& [format, name] : docling) {
    require(grparse::response_format_name(format) == std::string_view(name) &&
                grparse::response_format_from_name(name) == format,
            std::string("docling name round-trips: ") + name);
  }
  require(!grparse::response_format_name(parsev1::RESPONSE_FORMAT_UNSPECIFIED).has_value() &&
              !grparse::response_format_from_name("DOCTAGS").has_value(),
          "UNSPECIFIED has no name and names are docling's lowercase values");
  require(grparse::vlm_collector_response_format(parsev1::RESPONSE_FORMAT_OTSL) ==
                  vlmv1::RESPONSE_FORMAT_OTSL &&
              !grparse::vlm_collector_response_format(parsev1::RESPONSE_FORMAT_DOCLANG)
                   .has_value(),
          "the collector maps the five it reads and nothing else");

  parsev1::ConvertDocumentOptions local;
  local.mutable_vlm_pipeline_model_local()->set_repo_id("ibm-granite/granite-docling-258M");
  local.mutable_vlm_pipeline_model_local()->set_response_format(parsev1::RESPONSE_FORMAT_DOCTAGS);
  grparse::VlmConvertOptions local_options;
  grparse::apply_vlm_convert_options(local, &local_options);
  require(local_options.response_format == vlmv1::RESPONSE_FORMAT_DOCTAGS,
          "the local model's typed format reaches the collector");

  parsev1::ConvertDocumentOptions api;
  api.mutable_vlm_pipeline_model_api()->set_url("http://vlm.example:8080/v1");
  api.mutable_vlm_pipeline_model_api()->set_response_format_raw("markdown");
  grparse::VlmConvertOptions api_options;
  grparse::apply_vlm_convert_options(api, &api_options);
  require(api_options.response_format == vlmv1::RESPONSE_FORMAT_MARKDOWN,
          "the api model's raw docling name reaches the collector");

  parsev1::ConvertDocumentOptions custom;
  auto* spec = custom.mutable_vlm_pipeline_custom_config()->mutable_model_spec();
  spec->set_name("granite_docling");
  spec->set_default_repo_id("ibm-granite/granite-docling-258M");
  spec->set_response_format(parsev1::RESPONSE_FORMAT_HTML);
  grparse::VlmConvertOptions custom_options;
  grparse::apply_vlm_convert_options(custom, &custom_options);
  require(custom_options.response_format == vlmv1::RESPONSE_FORMAT_HTML,
          "the custom config's spec format reaches the collector");

  parsev1::ConvertDocumentOptions none;
  none.set_vlm_pipeline_model(parsev1::VLM_MODEL_TYPE_GRANITEDOCLING);
  grparse::VlmConvertOptions none_options;
  grparse::apply_vlm_convert_options(none, &none_options);
  require(none_options.response_format == vlmv1::RESPONSE_FORMAT_UNSPECIFIED,
          "no format named leaves the preset's own");
}

void verify_convert_rasters_dials_and_merges() {
  FakeVlmConvertService fake;
  ServerFixture server(&fake);
  auto bytes = std::make_shared<const std::string>("fake-pdf-bytes");
  // Inject a page source by converting through a channel; convert_vlm_pages
  // opens via open_in_memory_document, so use a real tiny PNG raster instead.
  cv::Mat image(16, 12, CV_8UC3, cv::Scalar(1, 2, 3));
  std::vector<unsigned char> png;
  require(cv::imencode(".png", image, png, grparse::kPngEncodeParams), "encode fixture");
  bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(png.data()),
                                             png.size());

  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(5000);
  options.preset = vlmv1::VLM_PRESET_GRANITE_DOCLING;
  options.endpoint = "http://vlm.test:9";
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const grparse::VlmConvertReport report =
      grparse::convert_vlm_pages(server.channel(), options, bytes, /*pdf=*/false, &document);
  require(report.success && report.pages_sent == 1 && report.pages_ok == 1,
          "one raster page converts: " + report.error);
  require(document.texts_size() == 1 && document.texts(0).text().base().text() == "page-1",
          "page document merges into the target");
  {
    const auto& base = document.texts(0).text().base();
    const docv1::GenerationSource* generation = nullptr;
    for (const auto& source : base.source()) {
      if (source.has_generation()) generation = &source.generation();
    }
    require(generation != nullptr && generation->model() == "granite-docling",
            "the fragment's GenerationSource survives the merge");
    require(generation->stop_reason() == docv1::VLM_STOP_REASON_LENGTH,
            "a merged VLM fragment carries a typed stop reason");
    require(generation->finish_reason() == "length", "finish_reason keeps the verbatim value");
  }
  require(report.warnings.size() == 2 &&
              report.warnings[0] ==
                  "vlm convert: page 1: PAGE_WARNING_CODE_TABLE_TRUNCATED: kept 500 of 90000 "
                  "rows (#/tables/0)" &&
              report.warnings[1] == "vlm convert: page 1: page warning 77",
          "the fragment's cap warnings reach the report by code name");
  const FakeVlmConvertService::Seen seen = fake.seen();
  require(seen.options_first && seen.options.preset() == vlmv1::VLM_PRESET_GRANITE_DOCLING &&
              seen.options.endpoint() == "http://vlm.test:9" && seen.page_nos.size() == 1 &&
              seen.page_nos[0] == 1 && seen.page_bytes[0] > 0,
          "options lead and one full-page PNG is sent");
}

// docling 2.140 VlmStopReason: every upstream value name and OpenAI "stop"
// classify; anything else stays UNSPECIFIED with no guess.
void verify_vlm_stop_reason_classifies_like_docling() {
  const std::vector<std::pair<std::string, docv1::VlmStopReason>> cases{
      {"length", docv1::VLM_STOP_REASON_LENGTH},
      {"stop_sequence", docv1::VLM_STOP_REASON_STOP_SEQUENCE},
      {"end_of_sequence", docv1::VLM_STOP_REASON_END_OF_SEQUENCE},
      {"stop", docv1::VLM_STOP_REASON_END_OF_SEQUENCE},
      {"content_filter", docv1::VLM_STOP_REASON_CONTENT_FILTERED},
      {"inference_error", docv1::VLM_STOP_REASON_INFERENCE_ERROR},
      {"unspecified", docv1::VLM_STOP_REASON_UNSPECIFIED},
      {"tool_calls", docv1::VLM_STOP_REASON_UNSPECIFIED},
      {"LENGTH", docv1::VLM_STOP_REASON_UNSPECIFIED},
      {"", docv1::VLM_STOP_REASON_UNSPECIFIED},
  };
  for (const auto& [raw, expected] : cases) {
    require(grparse::vlm_stop_reason(raw) == expected,
            "finish_reason '" + raw + "' classifies as " + docv1::VlmStopReason_Name(expected));
  }
}

// The fold reaches every GenerationSource in the document (texts, nested
// table cells' owners, pictures), keeps a producer's own stop_reason, and
// leaves a generation without finish_reason unset.
void verify_type_generation_stop_reasons_walks_the_document() {
  docv1::Document document;
  auto* text = document.add_texts()->mutable_text();
  text->mutable_base()->add_source()->mutable_generation()->set_finish_reason("content_filter");
  text->mutable_base()->add_source()->mutable_collector()->set_collector("vlm-convert");
  auto* picture = document.add_pictures();
  auto* produced = picture->add_source()->mutable_generation();
  produced->set_finish_reason("length");
  produced->set_stop_reason(docv1::VLM_STOP_REASON_STOP_SEQUENCE);
  picture->add_source()->mutable_generation()->set_model("no-finish-reason");
  auto* table = document.add_tables();
  table->add_source()->mutable_generation()->set_finish_reason("stop");

  grparse::type_generation_stop_reasons(&document);

  require(document.texts(0).text().base().source(0).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_CONTENT_FILTERED,
          "a text item's generation is typed");
  require(document.texts(0).text().base().source(1).has_collector(),
          "non-generation sources are untouched");
  require(document.pictures(0).source(0).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_STOP_SEQUENCE,
          "a producer's own stop_reason is kept, not re-derived");
  require(document.pictures(0).source(0).generation().finish_reason() == "length",
          "finish_reason is never rewritten");
  require(document.pictures(0).source(1).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_UNSPECIFIED,
          "no finish_reason, no stop_reason");
  require(document.tables(0).source(0).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_END_OF_SEQUENCE,
          "a table's generation is typed");

  docv1::Document wire;
  require(wire.ParseFromString(document.SerializeAsString()), "parse serialized document");
  require(wire.tables(0).source(0).generation().stop_reason() ==
              docv1::VLM_STOP_REASON_END_OF_SEQUENCE,
          "stop_reason survives the wire");
}

// True when `message`, or any message reachable from it through its fields
// (map entries included), is GenerationSource.
bool reaches_generation_source(const google::protobuf::Descriptor* message,
                               std::set<const google::protobuf::Descriptor*>* seen) {
  if (message == docv1::GenerationSource::descriptor()) return true;
  if (!seen->insert(message).second) return false;
  for (int index = 0; index < message->field_count(); ++index) {
    const google::protobuf::FieldDescriptor* field = message->field(index);
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
        reaches_generation_source(field->message_type(), seen)) {
      return true;
    }
  }
  return false;
}

// Collects every map field reachable from `message` through message fields.
void collect_map_fields(const google::protobuf::Descriptor* message,
                        std::set<const google::protobuf::Descriptor*>* seen,
                        std::vector<const google::protobuf::FieldDescriptor*>* maps) {
  if (!seen->insert(message).second) return;
  for (int index = 0; index < message->field_count(); ++index) {
    const google::protobuf::FieldDescriptor* field = message->field(index);
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) continue;
    if (field->is_map()) maps->push_back(field);
    collect_map_fields(field->message_type(), seen, maps);
  }
}

// type_generation_stop_reasons does not walk map fields, which is only
// sound while no map reachable from Document can hold a GenerationSource.
// A schema change that adds one fails here instead of leaving that
// generation's stop_reason silently unset.
void verify_no_map_in_document_reaches_a_generation_source() {
  std::set<const google::protobuf::Descriptor*> walked;
  require(reaches_generation_source(docv1::Document::descriptor(), &walked),
          "the guard sees the GenerationSource fields the fold does walk");

  std::set<const google::protobuf::Descriptor*> visited;
  std::vector<const google::protobuf::FieldDescriptor*> maps;
  collect_map_fields(docv1::Document::descriptor(), &visited, &maps);
  require(!maps.empty(), "the Document schema has map fields to check");
  for (const google::protobuf::FieldDescriptor* map : maps) {
    const google::protobuf::FieldDescriptor* value = map->message_type()->map_value();
    if (value->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) continue;
    std::set<const google::protobuf::Descriptor*> seen;
    require(!reaches_generation_source(value->message_type(), &seen),
            "map field " + std::string(map->full_name()) +
                " can hold a GenerationSource, which type_generation_stop_reasons skips");
  }
}

// Answers the first page with page_raw, then keeps the stream open until the
// client cancels it (or ten seconds pass), standing in for a peer still busy
// with the rest of the document.
class StallingVlmConvertService final : public vlmv1::VlmConvertService::Service {
 public:
  grpc::Status ConvertPages(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<vlmv1::ConvertPagesResponse, vlmv1::ConvertPagesRequest>*
          stream) override {
    vlmv1::ConvertPagesRequest request;
    while (stream->Read(&request)) {
    }
    vlmv1::ConvertPagesResponse event;
    event.mutable_page_raw()->set_page_no(1);
    event.mutable_page_raw()->set_error("model refused the page");
    stream->Write(event);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!context->IsCancelled() && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return grpc::Status::OK;
  }
};

std::shared_ptr<const std::string> one_page_png() {
  cv::Mat image(16, 12, CV_8UC3, cv::Scalar(1, 2, 3));
  std::vector<unsigned char> png;
  require(cv::imencode(".png", image, png, grparse::kPngEncodeParams), "encode fixture");
  return std::make_shared<const std::string>(reinterpret_cast<const char*>(png.data()),
                                             png.size());
}

// abort_on_error on a page_raw event cancels the call before Finish, so the
// abort returns at once instead of waiting out a peer that is still working.
void verify_abort_cancels_the_stream() {
  StallingVlmConvertService stalling;
  ServerFixture server(&stalling);
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(30000);
  options.abort_on_error = true;
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto started = std::chrono::steady_clock::now();
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      server.channel(), options, one_page_png(), /*pdf=*/false, &document);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(!report.success && report.code == grpc::StatusCode::INTERNAL &&
              report.error.contains("model refused the page"),
          "the page_raw event aborts the convert: " + report.error);
  require(elapsed < std::chrono::seconds(5), "the abort does not wait for the peer to finish");
}

// A cancelled request or a passed deadline stops before the next page: nothing
// is rasterized further and the peer is never dialed for it.
void verify_cancel_and_deadline_stop_before_rendering() {
  FakeVlmConvertService fake;
  ServerFixture server(&fake);
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(5000);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");

  const grparse::VlmConvertReport cancelled = grparse::convert_vlm_pages(
      server.channel(), options, one_page_png(), /*pdf=*/false, &document,
      grparse::kNoCollectorDeadline, [] { return true; });
  require(!cancelled.success && cancelled.code == grpc::StatusCode::CANCELLED &&
              cancelled.pages_sent == 0,
          "a cancelled request sends nothing: " + cancelled.error);

  const grparse::VlmConvertReport expired = grparse::convert_vlm_pages(
      server.channel(), options, one_page_png(), /*pdf=*/false, &document,
      std::chrono::system_clock::now() - std::chrono::seconds(1));
  require(!expired.success && expired.code == grpc::StatusCode::DEADLINE_EXCEEDED &&
              expired.pages_sent == 0,
          "a passed deadline sends nothing: " + expired.error);
  require(fake.calls() == 0, "neither stopped request dialed the peer");
}

// A caller that goes away after the pages are sent cancels the stream while
// the client waits on the peer's events, instead of waiting out the peer.
void verify_cancel_after_upload_ends_the_read() {
  StallingVlmConvertService stalling;
  ServerFixture server(&stalling);
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(30000);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto started = std::chrono::steady_clock::now();
  // Still present for the page loop's own check, gone once the read waits.
  const auto gone_at = started + std::chrono::milliseconds(300);
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      server.channel(), options, one_page_png(), /*pdf=*/false, &document,
      grparse::kNoCollectorDeadline,
      [gone_at] { return std::chrono::steady_clock::now() >= gone_at; });
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(report.pages_sent == 1, "the page goes out before the caller leaves");
  require(!report.success && report.code == grpc::StatusCode::CANCELLED,
          "the read ends cancelled: " + report.error);
  require(elapsed < std::chrono::seconds(5), "the read does not wait out the peer");
}

// A peer that answers each page before it reads the next: its answers fill
// this client's receive window, it stops reading, and an upload that sent
// every page before reading anything would stall until the deadline.
class InterleavingVlmConvertService final : public vlmv1::VlmConvertService::Service {
 public:
  grpc::Status ConvertPages(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<vlmv1::ConvertPagesResponse, vlmv1::ConvertPagesRequest>*
          stream) override {
    vlmv1::ConvertPagesRequest request;
    uint32_t answered = 0;
    while (stream->Read(&request)) {
      if (!request.has_page_image()) continue;
      const uint32_t page_no = request.page_image().page_no();
      vlmv1::ConvertPagesResponse event;
      auto* page = event.mutable_page_document();
      page->set_page_no(page_no);
      auto* text = page->mutable_document()->add_texts()->mutable_text();
      text->mutable_base()->set_self_ref("#/texts/0");
      text->mutable_base()->set_text(std::string(2U * 1024U * 1024U, 'x'));
      (*page->mutable_document()->mutable_pages())[page_no].set_page_no(page_no);
      if (!stream->Write(event)) break;
      ++answered;
    }
    vlmv1::ConvertPagesResponse done;
    done.mutable_complete()->set_pages_ok(answered);
    stream->Write(done);
    return grpc::Status::OK;
  }
};

void verify_answers_are_read_while_pages_go_out() {
  InterleavingVlmConvertService interleaving;
  ServerFixture server(&interleaving);
  constexpr int kPages = 12;
  std::vector<cv::Mat> pages;
  cv::RNG noise(7);
  for (int index = 0; index < kPages; ++index) {
    cv::Mat page(700, 700, CV_8UC3);
    noise.fill(page, cv::RNG::UNIFORM, 0, 256);
    pages.push_back(page);
  }
  std::vector<unsigned char> tiff;
  require(cv::imencodemulti(".tiff", pages, tiff), "encode the multi-page fixture");
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(30000);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto started = std::chrono::steady_clock::now();
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      server.channel(), options,
      std::make_shared<const std::string>(reinterpret_cast<const char*>(tiff.data()), tiff.size()),
      /*pdf=*/false, &document);
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(15),
          "the convert finishes long before its deadline");
  require(report.success && report.pages_sent == kPages && report.pages_ok == kPages,
          "every page was answered: " + report.error);
}

// A render stuck on the PDF backend ends with the request: the page source
// is cancelled once the caller goes, instead of waiting out the backend.
void verify_cancel_reaches_a_stalled_render() {
  grparse_test::ScopedPdfBackend pdf_backend;
  const std::string pdf = "%PDF-vlm-stalled-render";
  pdf_backend.backend().add_document(
      pdf, {grparse_test::text_page({"one"}), grparse_test::text_page({"two"})});
  pdf_backend.backend().stall_renders(std::chrono::seconds(20));
  FakeVlmConvertService fake;
  ServerFixture server(&fake);
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(30000);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto started = std::chrono::steady_clock::now();
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      server.channel(), options, std::make_shared<const std::string>(pdf), /*pdf=*/true,
      &document, grparse::kNoCollectorDeadline, [started] {
        return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300);
      });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the stalled render is cancelled, not waited out");
  require(!report.success && report.code == grpc::StatusCode::CANCELLED,
          "the convert ends cancelled: " + report.error);
}

// The opening Probe runs after the source is tied to the request: a PDF
// backend that hangs while loading the document ends with the caller.
void verify_cancel_reaches_a_hung_open() {
  grparse_test::ScopedPdfBackend pdf_backend;
  const std::string pdf = "%PDF-vlm-hung-open";
  pdf_backend.backend().add_document(pdf, {grparse_test::text_page({"one"})});
  pdf_backend.backend().block_probe(pdf);
  FakeVlmConvertService fake;
  ServerFixture server(&fake);
  grparse::VlmConvertOptions options;
  options.target = server.target();
  options.timeout = std::chrono::milliseconds(30000);
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  const auto started = std::chrono::steady_clock::now();
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      server.channel(), options, std::make_shared<const std::string>(pdf), /*pdf=*/true,
      &document, grparse::kNoCollectorDeadline, [started] {
        return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300);
      });
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "the hung Probe is cancelled, not waited out");
  require(!report.success && report.code == grpc::StatusCode::CANCELLED,
          "the convert ends cancelled: " + report.error);
}

void verify_missing_target_is_failed_precondition() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  auto bytes = std::make_shared<const std::string>("x");
  const grparse::VlmConvertReport report = grparse::convert_vlm_pages(
      nullptr, grparse::VlmConvertOptions{}, bytes, false, &document);
  require(!report.success && report.code == grpc::StatusCode::FAILED_PRECONDITION &&
              report.error.contains("GRPARSE_VLM_CONVERT_TARGET"),
          "unconfigured target names the env var");
}

void verify_endpoints_lazy_channel() {
  grparse::CollectorTargets targets;
  require(!targets.vlm.enabled(), "default off");
  grparse::CollectorEndpoints empty(targets);
  require(!empty.has_vlm() && empty.vlm_channel() == nullptr, "no channel without target");
  targets.vlm.target = "vlm-convert:50058";
  grparse::CollectorEndpoints wired(targets);
  require(wired.has_vlm() && wired.vlm_channel() != nullptr &&
              wired.vlm_channel() == wired.vlm_channel(),
          "configured target gets one lazy channel");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("vlm-convert-test", "all checks passed", {
      verify_apply_maps_granite_docling_and_raw_api_url,
      verify_response_formats_cover_docling_and_reach_the_collector,
      verify_vlm_stop_reason_classifies_like_docling,
      verify_type_generation_stop_reasons_walks_the_document,
      verify_no_map_in_document_reaches_a_generation_source,
      verify_convert_rasters_dials_and_merges,
      verify_missing_target_is_failed_precondition,
      verify_abort_cancels_the_stream,
      verify_cancel_and_deadline_stop_before_rendering,
      verify_cancel_after_upload_ends_the_read,
      verify_answers_are_read_while_pages_go_out,
      verify_cancel_reaches_a_stalled_render,
      verify_cancel_reaches_a_hung_open,
      verify_endpoints_lazy_channel,
  });
}
