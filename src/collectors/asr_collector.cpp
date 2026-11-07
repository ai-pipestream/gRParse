#include "grparse/document_collectors.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "collector_support.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse {
namespace {

asrv1::Task map_task(parsev1::AsrTask task) {
  switch (task) {
    case parsev1::ASR_TASK_TRANSLATE:
      return asrv1::TASK_TRANSLATE;
    case parsev1::ASR_TASK_TRANSCRIBE:
      return asrv1::TASK_TRANSCRIBE;
    default:
      return static_cast<asrv1::Task>(static_cast<int>(task));
  }
}

asrv1::AsrModelType map_model_type(parsev1::AsrModelType type) {
  return static_cast<asrv1::AsrModelType>(static_cast<int>(type));
}

asrv1::AsrEngineKind map_engine(parsev1::AsrEngineKind engine) {
  return static_cast<asrv1::AsrEngineKind>(static_cast<int>(engine));
}

asrv1::AsrModelFamily map_family(parsev1::AsrModelFamily family) {
  return static_cast<asrv1::AsrModelFamily>(static_cast<int>(family));
}

void apply_asr_pipeline_options(const parsev1::AsrPipelineOptions& in,
                                asrv1::TranscribeOptions* out) {
  if (in.has_model() && !in.model().empty()) out->set_model(in.model());
  if (in.has_language()) out->set_language(in.language());
  if (in.has_task()) out->set_task(map_task(in.task()));
  if (in.has_word_timestamps()) out->set_word_timestamps(in.word_timestamps());
  if (in.has_emit_keyframes()) out->set_emit_keyframes(in.emit_keyframes());
  if (in.has_keyframe_interval_seconds()) {
    out->set_keyframe_interval_seconds(in.keyframe_interval_seconds());
  }
  if (in.has_diarize()) out->set_diarize(in.diarize());
  if (in.has_model_type()) out->set_model_type(map_model_type(in.model_type()));
  if (in.has_model_type_raw()) out->set_model_type_raw(in.model_type_raw());
  if (in.has_engine()) out->set_engine(map_engine(in.engine()));
  if (in.has_transformers()) {
    const auto& src = in.transformers();
    auto* dst = out->mutable_transformers();
    if (src.has_revision()) dst->set_revision(src.revision());
    if (src.has_model_family()) dst->set_model_family(map_family(src.model_family()));
    if (src.has_model_family_raw()) dst->set_model_family_raw(src.model_family_raw());
    if (src.has_prompt()) dst->set_prompt(src.prompt());
    if (src.has_language()) dst->set_language(src.language());
    if (src.has_task()) dst->set_task(map_task(src.task()));
    if (src.has_task_raw()) dst->set_task_raw(src.task_raw());
  }
  if (in.has_mlx_whisper()) {
    *out->mutable_mlx_whisper() = [&] {
      asrv1::InlineAsrMlxWhisperOptions dst;
      const auto& src = in.mlx_whisper();
      if (src.has_condition_on_previous_text()) {
        dst.set_condition_on_previous_text(src.condition_on_previous_text());
      }
      if (src.has_hallucination_silence_threshold()) {
        dst.set_hallucination_silence_threshold(src.hallucination_silence_threshold());
      }
      if (src.has_initial_prompt()) dst.set_initial_prompt(src.initial_prompt());
      return dst;
    }();
  }
  if (in.has_native_whisper()) {
    auto* dst = out->mutable_native_whisper();
    if (in.native_whisper().has_condition_on_previous_text()) {
      dst->set_condition_on_previous_text(in.native_whisper().condition_on_previous_text());
    }
  }
  if (in.has_remote()) *out->mutable_remote() = in.remote();
}

}  // namespace

CollectorOutcome collect_asr_document(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& model,
    const std::string& filename, const std::string& bytes,
    const std::optional<parsev1::AsrPipelineOptions>& asr_options,
    CollectorDeadline inbound_deadline, CollectorCancelled cancelled) {
  auto stub = asrv1::AsrService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kAsrDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->Transcribe(&context);

  asrv1::TranscribeRequest request;
  auto* options = request.mutable_options();
  // An explicit request selector must reach ASR without the deployment model:
  // grpc-asr gives a nonempty model name precedence over a preset.
  const bool request_selector = asr_options &&
      ((asr_options->has_model() && !asr_options->model().empty()) ||
       asr_options->model_type() != parsev1::ASR_MODEL_TYPE_UNSPECIFIED ||
       !asr_options->model_type_raw().empty() ||
       asr_options->has_remote());
  if (!request_selector) options->set_model(model);
  options->set_emit_document(true);
  options->set_filename(filename);
  static const bool diarize_default = [] {
    const char* value = std::getenv("GRPARSE_ASR_DIARIZE");
    return value == nullptr || std::string_view(value) != "0";
  }();
  options->set_diarize(diarize_default);
  if (asr_options.has_value()) apply_asr_pipeline_options(*asr_options, options);

  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](asrv1::TranscribeRequest& frame, size_t offset, size_t length, bool /*last*/) {
        frame.mutable_chunk()->set_data(bytes.data() + offset, length);
      });
  return drain_stream<asrv1::TranscribeResponse>(
      "asr", *stream, upload,
      [](const asrv1::TranscribeResponse& event, std::vector<std::string>&) {
        return event.has_complete();
      });
}

}  // namespace grparse
