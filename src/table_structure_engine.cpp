#include "grparse/table_structure_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"

#ifdef GRPARSE_EMBED_OPENVINO
#include <openvino/c/openvino.h>
#endif

namespace grparse {
namespace {

// Model geometry and normalization mirror RapidTable's PPTableStructurer for
// the slanet-plus export, the reference this implementation is golden-tested
// against.  The export runs its decode loop inside the graph: one pass emits
// per-step structure token probabilities and cell corner boxes.
constexpr int kInputSize = 488;
// Reference preprocessing applies these to the image's channel order as
// loaded (BGR); replicated verbatim so goldens match.
constexpr std::array<float, 3> kMean = {0.485F, 0.456F, 0.406F};
constexpr std::array<float, 3> kStd = {0.229F, 0.224F, 0.225F};

// Only direct GPU selectors use the normalized native table graph. Other
// OpenVINO devices (including NPU) retain the strict ORT provider path.
bool is_openvino_gpu_device(std::string_view device) {
  return device == "GPU" ||
         (device.starts_with("GPU.") && device.size() > 4 &&
          std::all_of(device.begin() + 4, device.end(),
                      [](unsigned char c) { return c >= '0' && c <= '9'; }));
}

struct TableOutput {
  std::vector<int64_t> shape;
  std::vector<float> values;
};

#ifdef GRPARSE_EMBED_OPENVINO
void check_table_ov(ov_status_e status) {
  if (status != OK) {
    const char* detail = ov_get_last_err_msg();
    throw std::runtime_error(std::string("Native OpenVINO table operation failed: ") +
                             (detail == nullptr ? "unknown runtime error" : detail));
  }
}

template <typename T, void (*Free)(T*)>
using OvHandle = std::unique_ptr<T, decltype(Free)>;

// Use the ABI-neutral C API, like embeddings. ORT's nested Loop partitioning
// cannot execute this model on OpenVINO without exporting nested fused nodes.
class NativeTableBackend {
 public:
  NativeTableBackend(const std::filesystem::path& path, std::string device)
      : device_(std::move(device)) {
    if (!is_openvino_gpu_device(device_)) {
      throw std::invalid_argument("Native OpenVINO tables require an explicit GPU device");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("OpenVINO table model is missing: " + path.string());
    model_bytes_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (model_bytes_.empty() || file.bad()) {
      throw std::runtime_error("Cannot read OpenVINO table model: " + path.string());
    }
    OvCompileGate gate;
    ov_core_t* core = nullptr;
    check_table_ov(ov_core_create(&core));
    core_.reset(core);
    // Eager compilation preserves startup failure semantics. This instance is
    // consumed by the first call; every subsequent call gets a fresh model.
    const auto execution_devices = compile();
    std::println(stderr,
                 "gRParse table structure: native OpenVINO ({}) using {} "
                 "(fresh compilation per recognition)",
                 execution_devices, path.string());
  }

  std::array<TableOutput, 2> infer(std::vector<float>& values) {
    if (!compiled_) compile();
    // Release even after an exception: a compiled Loop retains decoder state
    // across requests in the shipped runtime. Fresh requests alone are unsafe.
    auto compiled = std::move(compiled_);
    ov_infer_request_t* raw_request = nullptr;
    check_table_ov(ov_compiled_model_create_infer_request(compiled.get(), &raw_request));
    OvHandle<ov_infer_request_t, ov_infer_request_free> request(raw_request, ov_infer_request_free);
    std::array<int64_t, 4> dims{1, 3, kInputSize, kInputSize};
    const ov_shape_t shape{4, dims.data()};
    ov_tensor_t* raw_input = nullptr;
    check_table_ov(ov_tensor_create_from_host_ptr(F32, shape, values.data(), &raw_input));
    OvHandle<ov_tensor_t, ov_tensor_free> input(raw_input, ov_tensor_free);
    check_table_ov(ov_infer_request_set_tensor(request.get(), "x", input.get()));
    check_table_ov(ov_infer_request_infer(request.get()));

    ov_profiling_info_list_t profile{};
    check_table_ov(ov_infer_request_get_profiling_info(request.get(), &profile));
    std::string cpu_node;
    for (size_t i = 0; i < profile.size; ++i) {
      const auto& info = profile.profiling_infos[i];
      const std::string execution = info.exec_type == nullptr ? "" : info.exec_type;
      const std::string type = info.node_type == nullptr ? "" : info.node_type;
      // The GPU plugin's Loop/If implementations orchestrate GPU body networks
      // on the host. Their CPU marker describes control, not CPU tensor math.
      if (info.status == ov_profiling_info_t::EXECUTED &&
          execution.find("CPU") != std::string::npos && type != "Loop" && type != "If") {
        cpu_node = info.node_name == nullptr ? type : info.node_name;
        if (cpu_node.empty()) cpu_node = "unnamed CPU node";
        break;
      }
    }
    ov_profiling_info_list_free(&profile);
    if (!cpu_node.empty()) {
      throw std::runtime_error("OpenVINO table executed a numerical node on CPU: " + cpu_node);
    }

    std::array<TableOutput, 2> outputs;
    for (size_t i = 0; i < outputs.size(); ++i) {
      ov_tensor_t* raw_output = nullptr;
      check_table_ov(ov_infer_request_get_output_tensor_by_index(request.get(), i, &raw_output));
      OvHandle<ov_tensor_t, ov_tensor_free> output(raw_output, ov_tensor_free);
      ov_element_type_e type = UNDEFINED;
      check_table_ov(ov_tensor_get_element_type(output.get(), &type));
      ov_shape_t actual{};
      check_table_ov(ov_tensor_get_shape(output.get(), &actual));
      const bool valid = type == F32 && actual.rank == 3 && actual.dims[0] == 1 &&
                         actual.dims[1] > 0 && actual.dims[1] <= 501 &&
                         (actual.dims[2] == 8 || actual.dims[2] == 50);
      if (valid) outputs[i].shape.assign(actual.dims, actual.dims + actual.rank);
      ov_shape_free(&actual);
      if (!valid) throw std::runtime_error("OpenVINO table output shape or precision mismatch");
      void* raw = nullptr;
      check_table_ov(ov_tensor_data(output.get(), &raw));
      const auto* data = static_cast<const float*>(raw);
      const size_t count = static_cast<size_t>(outputs[i].shape[1] * outputs[i].shape[2]);
      outputs[i].values.assign(data, data + count);
    }
    return outputs;
  }

 private:
  std::string compile() {
    ov_model_t* raw_model = nullptr;
    check_table_ov(ov_core_read_model_from_memory_buffer(
        core_.get(), model_bytes_.data(), model_bytes_.size(), nullptr, &raw_model));
    OvHandle<ov_model_t, ov_model_free> model(raw_model, ov_model_free);
    ov_compiled_model_t* raw_compiled = nullptr;
    check_table_ov(ov_core_compile_model(core_.get(), model.get(), device_.c_str(), 6,
        &raw_compiled, "INFERENCE_PRECISION_HINT", "f32", "PERFORMANCE_HINT", "LATENCY",
        "PERF_COUNT", "YES"));
    OvHandle<ov_compiled_model_t, ov_compiled_model_free> compiled(raw_compiled, ov_compiled_model_free);
    char* raw_devices = nullptr;
    check_table_ov(ov_compiled_model_get_property(compiled.get(), "EXECUTION_DEVICES", &raw_devices));
    const std::string devices = raw_devices == nullptr ? "" : raw_devices;
    ov_free(raw_devices);
    const bool generic_gpu = device_ == "GPU" && devices.starts_with("GPU.") &&
        devices.size() > 4 && std::all_of(devices.begin() + 4, devices.end(),
                                        [](unsigned char c) { return c >= '0' && c <= '9'; });
    if (devices != device_ && !generic_gpu) {
      throw std::runtime_error("OpenVINO table execution device mismatch: " + devices);
    }
    compiled_ = std::move(compiled);
    return devices;
  }

  std::string device_;
  std::string model_bytes_;
  OvHandle<ov_core_t, ov_core_free> core_{nullptr, ov_core_free};
  OvHandle<ov_compiled_model_t, ov_compiled_model_free> compiled_{nullptr, ov_compiled_model_free};
};
#endif

bool starts_with(const std::string& value, const char* prefix) {
  return value.starts_with(prefix);
}

int parse_span(const std::string& token) {
  // Tokens look like ` colspan="12"`; the number sits between the quotes.
  const size_t open = token.find('"');
  const size_t close = token.rfind('"');
  if (open == std::string::npos || close <= open + 1) return 1;
  const int value = std::atoi(token.substr(open + 1, close - open - 1).c_str());
  // The vocabulary tops out at 20; anything else is a decode artifact.
  return std::clamp(value, 1, 20);
}

bool is_cell_open(const std::string& token) {
  return token == "<td" || token == "<td>" || token == "<td></td>";
}

}  // namespace

TableStructure structure_from_tokens(const std::vector<std::string>& tokens,
                                     const std::vector<AxisAlignedBox>& cell_boxes) {
  TableStructure out;
  std::vector<std::vector<bool>> occupied;
  int row = -1;
  size_t box_index = 0;
  bool in_cell = false;
  bool in_thead = false;
  int col_span = 1;
  int row_span = 1;

  const auto place_cell = [&](int cspan, int rspan) {
    if (row < 0) row = 0;  // tolerate a missing leading <tr>
    if (static_cast<size_t>(row) >= occupied.size()) occupied.resize(static_cast<size_t>(row) + 1);
    const auto is_free = [&](size_t r, size_t c) {
      return r >= occupied.size() || c >= occupied[r].size() || !occupied[r][c];
    };
    size_t col = 0;
    for (; col < 1000; ++col) {
      bool fits = true;
      for (size_t offset = 0; offset < static_cast<size_t>(cspan); ++offset) {
        if (!is_free(static_cast<size_t>(row), col + offset)) {
          fits = false;
          break;
        }
      }
      if (fits) break;
    }
    const size_t needed_rows = static_cast<size_t>(row) + static_cast<size_t>(rspan);
    if (occupied.size() < needed_rows) occupied.resize(needed_rows);
    for (size_t r = static_cast<size_t>(row); r < needed_rows; ++r) {
      if (occupied[r].size() < col + static_cast<size_t>(cspan)) {
        occupied[r].resize(col + static_cast<size_t>(cspan), false);
      }
      for (size_t c = col; c < col + static_cast<size_t>(cspan); ++c) occupied[r][c] = true;
    }
    StructuredCell cell;
    cell.row = row;
    cell.col = static_cast<int>(col);
    cell.row_span = rspan;
    cell.col_span = cspan;
    cell.header = in_thead;
    if (box_index < cell_boxes.size()) {
      const AxisAlignedBox& box = cell_boxes[box_index];
      cell.left = box.left;
      cell.top = box.top;
      cell.right = box.right;
      cell.bottom = box.bottom;
    }
    ++box_index;
    out.cells.push_back(cell);
  };

  for (const std::string& token : tokens) {
    if (token == "<thead>") {
      in_thead = true;
    } else if (token == "</thead>") {
      in_thead = false;
    } else if (token == "<tr>") {
      ++row;
    } else if (token == "<td></td>") {
      place_cell(1, 1);
    } else if (token == "<td" || token == "<td>") {
      in_cell = true;
      col_span = 1;
      row_span = 1;
    } else if (in_cell && starts_with(token, " colspan=")) {
      col_span = parse_span(token);
    } else if (in_cell && starts_with(token, " rowspan=")) {
      row_span = parse_span(token);
    } else if (in_cell && token == "</td>") {
      place_cell(col_span, row_span);
      in_cell = false;
    }
    // ">", <tbody> wrappers, </tr>, and <table> framing carry no placement
    // information.
  }

  out.rows = static_cast<int>(occupied.size());
  size_t widest = 0;
  for (const auto& columns : occupied) widest = std::max(widest, columns.size());
  out.cols = static_cast<int>(widest);
  return out;
}

class TableStructureEngine::Impl {
 public:
  explicit Impl(const std::filesystem::path& model_path)
      : env_(ORT_LOGGING_LEVEL_ERROR, "grparse-table-structure") {
    if (!std::filesystem::exists(model_path)) {
      throw std::runtime_error("Table structure model is missing: " + model_path.string());
    }
    std::string vocabulary;
    const auto selection = ort_ep_selection();
    if (selection.ep == OrtEp::kOpenVino && is_openvino_gpu_device(selection.openvino_device)) {
#ifdef GRPARSE_EMBED_OPENVINO
      const auto directory = model_path.parent_path();
      const auto tokens_path = directory / "slanet_plus_openvino.tokens";
      std::ifstream tokens(tokens_path, std::ios::binary);
      if (!tokens) throw std::runtime_error("OpenVINO table vocabulary is missing: " + tokens_path.string());
      vocabulary.assign(std::istreambuf_iterator<char>(tokens), std::istreambuf_iterator<char>());
      if (vocabulary.empty() || tokens.bad()) {
        throw std::runtime_error("Cannot read OpenVINO table vocabulary: " + tokens_path.string());
      }
      native_ = std::make_unique<NativeTableBackend>(
          directory / "slanet_plus_openvino.onnx", selection.openvino_device);
#else
      throw std::runtime_error("Native OpenVINO table support is not built");
#endif
    } else {
      session_ = make_session(env_, model_path, "table structure");

      Ort::AllocatorWithDefaultOptions allocator;
      input_name_ = session_.GetInputNameAllocated(0, allocator).get();
      if (session_.GetOutputCount() != 2) {
        throw std::runtime_error("Table structure model has an unexpected output count");
      }
      for (size_t index = 0; index < 2; ++index) {
        output_names_.push_back(session_.GetOutputNameAllocated(index, allocator).get());
      }
      for (const auto& name : output_names_) output_name_pointers_.push_back(name.c_str());

      // The token vocabulary rides in the model metadata, one token per line,
      // exactly as the reference reads it.
      Ort::ModelMetadata metadata = session_.GetModelMetadata();
      const auto characters = metadata.LookupCustomMetadataMapAllocated("character", allocator);
      if (characters == nullptr) {
        throw std::runtime_error("Table structure model metadata lacks its token vocabulary");
      }
      vocabulary = characters.get();
    }
    std::istringstream lines(vocabulary);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) vocab_.push_back(line);
    }
    // Reference merge_no_span_structure: plain cells become one token.
    if (std::ranges::find(vocab_, "<td></td>") == vocab_.end()) {
      vocab_.push_back("<td></td>");
    }
    std::erase(vocab_, "<td>");
    vocab_.insert(vocab_.begin(), "sos");
    vocab_.push_back("eos");
  }

  TableStructure recognize(const cv::Mat& crop) {
    // The first inference on a fresh session compiles the provider's kernels;
    // serialize it process-wide (see grparse_session_ep.h).
    std::optional<OvCompileGate> compile_gate;
    // Native Loop state requires recompilation per call (~0.4s with a warm
    // kernel cache on B70), so every native invocation takes the JIT gate.
    bool needs_compile = !warmed_;
#ifdef GRPARSE_EMBED_OPENVINO
    needs_compile = needs_compile || native_ != nullptr;
#endif
    if (needs_compile) compile_gate.emplace();
    if (crop.empty() || crop.type() != CV_8UC3) {
      throw std::invalid_argument("Table structure expects a non-empty BGR crop");
    }
    const int height = crop.rows;
    const int width = crop.cols;

    // Preprocess: longest side to 488 (aspect preserved, truncating like the
    // reference), normalize, zero-pad to 488x488 top-left, HWC -> CHW.
    const float ratio =
        static_cast<float>(kInputSize) / static_cast<float>(std::max(height, width));
    const int resize_h = std::max(1, static_cast<int>(static_cast<float>(height) * ratio));
    const int resize_w = std::max(1, static_cast<int>(static_cast<float>(width) * ratio));
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(resize_w, resize_h));

    std::vector<float> tensor(static_cast<size_t>(3) * kInputSize * kInputSize, 0.0F);
    const size_t plane = static_cast<size_t>(kInputSize) * kInputSize;
    for (int row = 0; row < resize_h; ++row) {
      const cv::Vec3b* pixels = resized.ptr<cv::Vec3b>(row);
      for (int column = 0; column < resize_w; ++column) {
        const size_t offset = static_cast<size_t>(row) * kInputSize + column;
        for (int channel = 0; channel < 3; ++channel) {
          tensor[plane * static_cast<size_t>(channel) + offset] =
              (static_cast<float>(pixels[column][channel]) / 255.0F - kMean[channel]) /
              kStd[channel];
        }
      }
    }

    std::array<TableOutput, 2> outputs;
#ifdef GRPARSE_EMBED_OPENVINO
    if (native_) {
      outputs = native_->infer(tensor);
    } else
#endif
    {
      const std::array<int64_t, 4> input_shape = {1, 3, kInputSize, kInputSize};
      Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                         input_shape.data(), input_shape.size());
      const char* input_names[] = {input_name_.c_str()};
      auto ort_outputs = session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                                      output_name_pointers_.data(), output_name_pointers_.size());
      for (size_t i = 0; i < outputs.size(); ++i) {
        const auto info = ort_outputs[i].GetTensorTypeAndShapeInfo();
        outputs[i].shape = info.GetShape();
        const float* data = ort_outputs[i].GetTensorData<float>();
        outputs[i].values.assign(data, data + info.GetElementCount());
      }
    }

    // Identify boxes/probabilities by trailing dimension, preserving the
    // original reference decoder for both native and ORT execution.
    for (const auto& output : outputs) {
      if (output.shape.size() != 3 || output.shape[0] != 1 || output.shape[1] <= 0) {
        throw std::runtime_error("Table structure model output rank or batch mismatch");
      }
    }
    const auto shape_of = [&](size_t index) { return outputs[index].shape; };
    size_t box_output = 0;
    size_t prob_output = 1;
    if (shape_of(0).back() != 8) std::swap(box_output, prob_output);
    const auto box_shape = shape_of(box_output);
    const auto prob_shape = shape_of(prob_output);
    if (box_shape.back() != 8 ||
        prob_shape.back() != static_cast<int64_t>(vocab_.size()) ||
        box_shape[1] != prob_shape[1]) {
      throw std::runtime_error("Table structure model output shape mismatch");
    }
    const float* boxes = outputs[box_output].values.data();
    const float* probs = outputs[prob_output].values.data();
    const int64_t steps = prob_shape[1];
    const auto vocab_size = static_cast<int64_t>(vocab_.size());
    const size_t eos_index = vocab_.size() - 1;

    // Reference rescale for slanet-plus: normalized corners scale by the
    // original crop size, then by the pad-to-square ratio.
    const float rescale_w =
        static_cast<float>(kInputSize) / (static_cast<float>(width) * ratio);
    const float rescale_h =
        static_cast<float>(kInputSize) / (static_cast<float>(height) * ratio);

    std::vector<std::string> tokens;
    std::vector<AxisAlignedBox> cell_boxes;
    double score_total = 0.0;
    size_t score_count = 0;
    for (int64_t step = 0; step < steps; ++step) {
      const float* step_probs = probs + step * vocab_size;
      int64_t best = 0;
      for (int64_t index = 1; index < vocab_size; ++index) {
        if (step_probs[index] > step_probs[best]) best = index;
      }
      if (step > 0 && static_cast<size_t>(best) == eos_index) break;
      if (best == 0 || static_cast<size_t>(best) == eos_index) continue;  // sos/eos
      const std::string& token = vocab_[static_cast<size_t>(best)];
      if (is_cell_open(token)) {
        const float* corners = boxes + step * 8;
        float min_x = corners[0] * static_cast<float>(width) * rescale_w;
        float max_x = min_x;
        float min_y = corners[1] * static_cast<float>(height) * rescale_h;
        float max_y = min_y;
        for (int point = 1; point < 4; ++point) {
          const float x = corners[point * 2] * static_cast<float>(width) * rescale_w;
          const float y = corners[point * 2 + 1] * static_cast<float>(height) * rescale_h;
          min_x = std::min(min_x, x);
          max_x = std::max(max_x, x);
          min_y = std::min(min_y, y);
          max_y = std::max(max_y, y);
        }
        AxisAlignedBox box;
        box.left = std::clamp(static_cast<int>(std::lround(min_x)), 0, width);
        box.top = std::clamp(static_cast<int>(std::lround(min_y)), 0, height);
        box.right = std::clamp(static_cast<int>(std::lround(max_x)), 0, width);
        box.bottom = std::clamp(static_cast<int>(std::lround(max_y)), 0, height);
        cell_boxes.push_back(box);
      }
      tokens.push_back(token);
      score_total += step_probs[best];
      ++score_count;
    }

    TableStructure structure = structure_from_tokens(tokens, cell_boxes);
    structure.score =
        score_count == 0 ? 0.0F : static_cast<float>(score_total / static_cast<double>(score_count));
    warmed_ = true;
    return structure;
  }

 private:
#ifdef GRPARSE_EMBED_OPENVINO
  std::unique_ptr<NativeTableBackend> native_;
#endif
  Ort::Env env_;
  Ort::Session session_{nullptr};
  std::string input_name_;
  std::vector<std::string> output_names_;
  std::vector<const char*> output_name_pointers_;
  std::vector<std::string> vocab_;
  // First inference compiles provider kernels; see the gate in recognize().
  bool warmed_ = false;
};

TableStructureEngine::TableStructureEngine(const std::filesystem::path& model_path)
    : impl_(std::make_unique<Impl>(model_path)) {}

TableStructureEngine::~TableStructureEngine() = default;

TableStructure TableStructureEngine::recognize(const cv::Mat& crop) {
  return impl_->recognize(crop);
}

TableStructureEnginePool::TableStructureEnginePool(const std::filesystem::path& model_path,
                                                   size_t worker_count)
    : engines_(worker_count,
               [model_path] { return std::make_unique<TableStructureEngine>(model_path); }) {
  engines_.prime();
}

TableStructure TableStructureEnginePool::recognize(const cv::Mat& crop) {
  auto lease = engines_.acquire();
  try {
    return lease->recognize(crop);
  } catch (...) {
    // Same policy as OCR: a session that throws mid-inference may be wedged;
    // rebuild it on next acquire instead of reusing it.
    lease.discard();
    throw;
  }
}

}  // namespace grparse
