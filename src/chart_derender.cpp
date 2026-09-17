#include "grparse/chart_derender.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ai/pipestream/enrich/v1/enrich_service.grpc.pb.h"
#include "grparse/base64.h"
#include "grparse/data_totals.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace enrichv1 = ai::pipestream::enrich::v1;

namespace {

// The classes the enrich service itself treats as charts (its ItemSelector
// matches the top prediction exactly, lowercased), so a picture sent here
// is one it will select rather than skip.
bool chart_class(std::string_view name) {
  return name == "bar_chart" || name == "line_chart" || name == "pie_chart";
}

// The classifier's top verdict: the wire annotation first (the CV path
// writes both), the meta prediction list otherwise.
std::string top_class(const docv1::PictureItem& picture) {
  for (const docv1::PictureAnnotation& annotation : picture.annotations()) {
    if (annotation.has_classification() &&
        annotation.classification().predicted_classes_size() > 0) {
      return annotation.classification().predicted_classes(0).class_name();
    }
  }
  if (picture.has_meta() && picture.meta().has_classification() &&
      picture.meta().classification().predictions_size() > 0) {
    return picture.meta().classification().predictions(0).class_name();
  }
  return std::string();
}

bool has_tabular_chart(const docv1::PictureItem& picture) {
  return std::ranges::any_of(picture.annotations(), [](const docv1::PictureAnnotation& one) {
    return one.has_tabular_chart();
  });
}

// The picture a wire self_ref names: the one that carries that self_ref, or,
// for a picture the arena never named (chart_derender_candidates sends those
// under "#/pictures/<index>"), the one at that index. Null when neither.
docv1::PictureItem* picture_for(const std::string& self_ref, docv1::Document* document) {
  for (docv1::PictureItem& picture : *document->mutable_pictures()) {
    if (!picture.self_ref().empty() && picture.self_ref() == self_ref) return &picture;
  }
  constexpr std::string_view kPrefix = "#/pictures/";
  if (!self_ref.starts_with(kPrefix)) return nullptr;
  const std::string_view digits(self_ref.data() + kPrefix.size(),
                                self_ref.size() - kPrefix.size());
  int index = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
  if (error != std::errc() || end != digits.data() + digits.size()) return nullptr;
  if (index < 0 || index >= document->pictures_size()) return nullptr;
  docv1::PictureItem* picture = document->mutable_pictures(index);
  return picture->self_ref().empty() ? picture : nullptr;
}

// Splits "data:<mimetype>;base64,<payload>" into decoded bytes; false for
// any other URI (an external reference is not pixels this side holds).
bool decode_data_uri(const std::string& uri, std::string* mimetype, std::string* bytes) {
  constexpr std::string_view kScheme = "data:";
  constexpr std::string_view kBase64 = ";base64,";
  if (!uri.starts_with(kScheme)) return false;
  const size_t marker = uri.find(kBase64);
  if (marker == std::string::npos) return false;
  *mimetype = uri.substr(kScheme.size(), marker - kScheme.size());
  *bytes = decode_base64(uri.substr(marker + kBase64.size()));
  return !bytes->empty();
}

int cells_extent(const docv1::TableData& data, bool rows) {
  int extent = 0;
  for (const docv1::TableCell& cell : data.table_cells()) {
    extent = std::max(extent, rows ? cell.end_row_offset_idx() : cell.end_col_offset_idx());
    extent = std::max(extent, 1 + (rows ? cell.start_row_offset_idx()
                                        : cell.start_col_offset_idx()));
  }
  return extent;
}

// The table as the wire carries it, with the grid extent stated even when
// the peer left num_rows and num_cols at zero.
docv1::TableData normalized_table(const docv1::TableData& table) {
  docv1::TableData data = table;
  if (data.num_rows() == 0) data.set_num_rows(cells_extent(data, true));
  if (data.num_cols() == 0) data.set_num_cols(cells_extent(data, false));
  return data;
}

bool empty_table(const docv1::TableData& table) {
  if (!table.table_cells().empty()) return false;
  return std::ranges::all_of(table.grid(), [](const docv1::TableRow& row) {
    return row.cells().empty();
  });
}

// grpc++ has no name table for status codes; the leg reports the ones a
// peer can realistically answer with and the number for anything else.
std::string status_code_name(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
    case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
    case grpc::StatusCode::CANCELLED: return "CANCELLED";
    case grpc::StatusCode::INTERNAL: return "INTERNAL";
    default: return "status " + std::to_string(static_cast<int>(code));
  }
}

std::string skip_reason_text(const enrichv1::ItemSkipped& skipped) {
  std::string text = enrichv1::SkipReason_Name(skipped.reason());
  if (!skipped.detail().empty()) text += ": " + skipped.detail();
  return text;
}

}  // namespace

std::vector<ChartCandidate> chart_derender_candidates(const docv1::Document& document) {
  std::vector<ChartCandidate> candidates;
  for (int index = 0; index < document.pictures_size(); index++) {
    const docv1::PictureItem& picture = document.pictures(index);
    const bool verdict = chart_class(top_class(picture)) ||
                         picture.label() == docv1::DOC_ITEM_LABEL_CHART;
    if (!verdict || has_tabular_chart(picture) || !picture.has_image()) continue;
    ChartCandidate candidate;
    if (!decode_data_uri(picture.image().uri(), &candidate.mimetype, &candidate.bytes)) continue;
    candidate.picture_index = index;
    candidate.self_ref = picture.self_ref().empty()
                             ? "#/pictures/" + std::to_string(index)
                             : picture.self_ref();
    if (candidate.mimetype.empty()) candidate.mimetype = picture.image().mimetype();
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

docv1::Document chart_derender_request_document(
    const docv1::Document& document, const std::vector<ChartCandidate>& candidates) {
  docv1::Document request;
  request.set_schema_name(document.schema_name());
  request.set_version(document.version());
  request.set_name(document.name());
  request.mutable_body()->set_self_ref("#/body");
  request.mutable_furniture()->set_self_ref("#/furniture");
  for (const ChartCandidate& candidate : candidates) {
    docv1::PictureItem* picture = request.add_pictures();
    *picture = document.pictures(candidate.picture_index);
    picture->set_self_ref(candidate.self_ref);
    // The pixels travel as ItemImage; the reference keeps its size only.
    picture->mutable_image()->clear_uri();
    request.mutable_body()->add_children()->set_ref(candidate.self_ref);
  }
  return request;
}

bool fold_chart_table(const enrichv1::ItemAnnotation& annotation, const std::string& endpoint,
                      docv1::Document* document) {
  if (!annotation.has_chart_table() || empty_table(annotation.chart_table().table())) {
    return false;
  }
  docv1::PictureItem* target = picture_for(annotation.self_ref(), document);
  // A picture that already carries a table (an office chart, or an earlier
  // answer for the same picture) keeps it: the fold is applied once.
  if (target == nullptr || has_tabular_chart(*target)) return false;
  const enrichv1::ChartTable& chart = annotation.chart_table();
  const docv1::TableData data = normalized_table(chart.table());

  docv1::PictureTabularChartData* tabular =
      target->add_annotations()->mutable_tabular_chart();
  tabular->set_kind("tabular_chart_data");
  tabular->set_title(chart.title());
  *tabular->mutable_chart_data() = data;

  // Meta is the export contract; the model that produced the table is its
  // created_by, so a reader can tell a derendered table from a live one.
  docv1::TabularChartMetaField* meta = target->mutable_meta()->mutable_tabular_chart();
  if (!annotation.model().empty()) meta->set_created_by(annotation.model());
  if (!chart.title().empty()) meta->set_title(chart.title());
  *meta->mutable_chart_data() = data;

  docv1::GenerationSource* generation = target->add_source()->mutable_generation();
  generation->set_model(annotation.model());
  if (!endpoint.empty()) generation->set_endpoint(endpoint);
  return true;
}

bool fold_picture_description(const enrichv1::ItemAnnotation& annotation,
                              const std::string& endpoint, docv1::Document* document) {
  if (!annotation.has_description() || annotation.description().text().empty()) return false;
  docv1::PictureItem* target = picture_for(annotation.self_ref(), document);
  if (target == nullptr) return false;
  if (target->has_meta() && target->meta().has_description() &&
      !target->meta().description().text().empty()) {
    return false;
  }
  docv1::DescriptionMetaField* meta = target->mutable_meta()->mutable_description();
  meta->set_text(annotation.description().text());
  if (!annotation.model().empty()) meta->set_created_by(annotation.model());
  auto* note = target->add_annotations()->mutable_description();
  note->set_kind("description");
  note->set_text(annotation.description().text());
  docv1::GenerationSource* generation = target->add_source()->mutable_generation();
  generation->set_model(annotation.model());
  if (!endpoint.empty()) generation->set_endpoint(endpoint);
  return true;
}

docv1::BaseTextItem* text_item_for(const std::string& self_ref, docv1::Document* document) {
  for (docv1::BaseTextItem& item : *document->mutable_texts()) {
    if (item.has_code() && item.code().self_ref() == self_ref) return &item;
    if (item.has_formula() && item.formula().base().self_ref() == self_ref) return &item;
    if (item.has_text() && item.text().base().self_ref() == self_ref) return &item;
    if (item.has_section_header() && item.section_header().base().self_ref() == self_ref) {
      return &item;
    }
  }
  constexpr std::string_view kPrefix = "#/texts/";
  if (!self_ref.starts_with(kPrefix)) return nullptr;
  const std::string_view digits(self_ref.data() + kPrefix.size(),
                                self_ref.size() - kPrefix.size());
  int index = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
  if (error != std::errc() || end != digits.data() + digits.size()) return nullptr;
  if (index < 0 || index >= document->texts_size()) return nullptr;
  return document->mutable_texts(index);
}

bool fold_code_annotation(const enrichv1::ItemAnnotation& annotation,
                          docv1::Document* document) {
  if (!annotation.has_code() || annotation.code().text().empty()) return false;
  docv1::BaseTextItem* item = text_item_for(annotation.self_ref(), document);
  if (item == nullptr || !item->has_code()) return false;
  item->mutable_code()->set_text(annotation.code().text());
  if (annotation.code().language() != docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED) {
    item->mutable_code()->set_code_language(annotation.code().language());
  }
  if (!annotation.code().language_raw().empty()) {
    item->mutable_code()->set_code_language_raw(annotation.code().language_raw());
  }
  return true;
}

bool fold_formula_annotation(const enrichv1::ItemAnnotation& annotation,
                             docv1::Document* document) {
  if (!annotation.has_formula() || annotation.formula().text().empty()) return false;
  docv1::BaseTextItem* item = text_item_for(annotation.self_ref(), document);
  if (item == nullptr || !item->has_formula()) return false;
  item->mutable_formula()->mutable_base()->set_text(annotation.formula().text());
  return true;
}

// Pictures that still need a description: inline pixels, no meta description.
std::vector<ChartCandidate> picture_description_candidates(const docv1::Document& document) {
  std::vector<ChartCandidate> candidates;
  for (int index = 0; index < document.pictures_size(); index++) {
    const docv1::PictureItem& picture = document.pictures(index);
    if (!picture.has_image()) continue;
    if (picture.has_meta() && picture.meta().has_description() &&
        !picture.meta().description().text().empty()) {
      continue;
    }
    ChartCandidate candidate;
    if (!decode_data_uri(picture.image().uri(), &candidate.mimetype, &candidate.bytes)) continue;
    candidate.picture_index = index;
    candidate.self_ref = picture.self_ref().empty()
                             ? "#/pictures/" + std::to_string(index)
                             : picture.self_ref();
    if (candidate.mimetype.empty()) candidate.mimetype = picture.image().mimetype();
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

ChartDerenderReport derender_charts(const std::shared_ptr<grpc::Channel>& channel,
                                    const ChartDerenderOptions& options,
                                    docv1::Document* document,
                                    CollectorDeadline inbound_deadline) {
  ChartDerenderReport report;
  if (!options.any_job()) return report;

  std::vector<ChartCandidate> chart_candidates;
  if (options.do_chart_extraction) {
    chart_candidates = chart_derender_candidates(*document);
  }
  std::vector<ChartCandidate> describe_candidates;
  if (options.do_picture_description) {
    describe_candidates = picture_description_candidates(*document);
  }
  // Deduplicate ItemImage uploads by self_ref (a chart may also be described).
  std::vector<ChartCandidate> images;
  std::set<std::string> image_refs;
  const auto add_images = [&](const std::vector<ChartCandidate>& list) {
    for (const ChartCandidate& one : list) {
      if (image_refs.insert(one.self_ref).second) images.push_back(one);
    }
  };
  add_images(chart_candidates);
  add_images(describe_candidates);

  const bool needs_full_document =
      options.do_picture_description || options.do_code_enrichment ||
      options.do_formula_enrichment;
  report.candidates = static_cast<int>(chart_candidates.size());
  if (chart_candidates.empty() && describe_candidates.empty() &&
      !options.do_code_enrichment && !options.do_formula_enrichment) {
    return report;
  }

  const auto count_skipped = [&report](int count) {
    report.skipped += count;
    data_counters().chart_derender_skipped.fetch_add(static_cast<uint64_t>(count),
                                                     std::memory_order_relaxed);
  };
  if (channel == nullptr || !options.enabled()) {
    report.warnings.push_back("document enrich: enrich service is not configured "
                              "(GRPARSE_ENRICH_TARGET)");
    count_skipped(std::max(1, report.candidates));
    return report;
  }

  auto stub = enrichv1::EnrichService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, options.timeout));
  context.set_wait_for_ready(false);
  auto stream = stub->EnrichDocument(&context);

  enrichv1::EnrichDocumentRequest frame;
  enrichv1::EnrichOptions* request_options = frame.mutable_options();
  request_options->set_do_chart_extraction(options.do_chart_extraction);
  request_options->set_do_picture_description(options.do_picture_description);
  request_options->set_do_code_enrichment(options.do_code_enrichment);
  request_options->set_do_formula_enrichment(options.do_formula_enrichment);
  if (options.picture_description_area_threshold != 0.0) {
    request_options->set_picture_description_area_threshold(
        options.picture_description_area_threshold);
  }
  if (!options.picture_description_preset_raw.empty()) {
    request_options->set_picture_description_preset_raw(options.picture_description_preset_raw);
  }
  if (!options.code_formula_preset_raw.empty()) {
    request_options->set_code_formula_preset_raw(options.code_formula_preset_raw);
  }
  const auto seconds = std::chrono::ceil<std::chrono::seconds>(options.timeout).count();
  request_options->set_timeout_seconds(static_cast<uint32_t>(std::max<long long>(1, seconds)));
  if (!options.vlm_endpoint.empty()) request_options->set_vlm_endpoint(options.vlm_endpoint);
  if (options.concurrency != 0) request_options->set_concurrency(options.concurrency);
  bool written = stream->Write(frame);
  for (const ChartCandidate& candidate : images) {
    if (!written) break;
    frame.Clear();
    enrichv1::ItemImage* image = frame.mutable_image();
    image->set_self_ref(candidate.self_ref);
    image->set_mimetype(candidate.mimetype);
    image->set_data(candidate.bytes);
    written = stream->Write(frame);
  }
  if (written) {
    frame.Clear();
    enrichv1::DocumentChunk* chunk = frame.mutable_chunk();
    if (needs_full_document) {
      // Full document so code/formula/description selectors see every item;
      // picture uris stay (ItemImage still supplies bytes when stripped peers
      // prefer crops, and inline data URIs remain readable).
      docv1::Document request_doc = *document;
      for (docv1::PictureItem& picture : *request_doc.mutable_pictures()) {
        if (picture.has_image()) picture.mutable_image()->clear_uri();
      }
      chunk->set_data(request_doc.SerializeAsString());
    } else {
      chunk->set_data(
          chart_derender_request_document(*document, chart_candidates).SerializeAsString());
    }
    chunk->set_complete(true);
    written = stream->Write(frame);
  }
  stream->WritesDone();

  int folded = 0;
  int skipped_events = 0;
  std::set<std::string> answered;
  enrichv1::EnrichDocumentResponse event;
  while (stream->Read(&event)) {
    if (event.has_annotation()) {
      const enrichv1::ItemAnnotation& annotation = event.annotation();
      if (annotation.has_chart_table()) {
        if (answered.contains(annotation.self_ref() + "#chart")) {
          report.warnings.push_back("chart derender: " + annotation.self_ref() +
                                    " returned a duplicate table, ignored");
        } else if (fold_chart_table(annotation, options.vlm_endpoint, document)) {
          ++folded;
          answered.insert(annotation.self_ref() + "#chart");
          data_log("chart " + annotation.self_ref() + " derendered by " + annotation.model() +
                   " (" + std::to_string(annotation.chart_table().table().num_rows()) + "x" +
                   std::to_string(annotation.chart_table().table().num_cols()) + ")");
        } else {
          ++skipped_events;
          report.warnings.push_back("chart derender: " + annotation.self_ref() +
                                    " returned an empty table");
        }
      } else if (annotation.has_description()) {
        if (fold_picture_description(annotation, options.vlm_endpoint, document)) {
          ++report.pictures_described;
          answered.insert(annotation.self_ref() + "#desc");
        } else {
          ++skipped_events;
        }
      } else if (annotation.has_code()) {
        if (fold_code_annotation(annotation, document)) {
          ++report.codes_enriched;
        } else {
          ++skipped_events;
        }
      } else if (annotation.has_formula()) {
        if (fold_formula_annotation(annotation, document)) {
          ++report.formulas_enriched;
        } else {
          ++skipped_events;
        }
      }
    } else if (event.has_skipped()) {
      ++skipped_events;
      report.warnings.push_back("document enrich: " + event.skipped().self_ref() +
                                " skipped (" + skip_reason_text(event.skipped()) + ")");
    }
    event.Clear();
  }
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    report.warnings.push_back("document enrich: enrich service " +
                              status_code_name(status.error_code()) +
                              (status.error_message().empty() ? "" : ": " + status.error_message()));
  }
  report.derendered = folded;
  data_counters().charts_derendered.fetch_add(static_cast<uint64_t>(folded),
                                              std::memory_order_relaxed);
  const int unanswered = std::max(0, report.candidates - folded);
  count_skipped(unanswered);
  if (unanswered > skipped_events && status.ok() && options.do_chart_extraction) {
    report.warnings.push_back("chart derender: " + std::to_string(unanswered - skipped_events) +
                              " chart(s) received no event before the stream ended");
  }
  return report;
}

}  // namespace grparse
