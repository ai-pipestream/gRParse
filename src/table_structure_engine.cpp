#include "grparse/table_structure_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"

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

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
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
    options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    // Same provider decision point as the OCR and layout sessions.
    append_execution_provider(options_, -1);
    session_ = Ort::Session(env_, model_path.c_str(), options_);

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
    std::istringstream lines(characters.get());
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) vocab_.push_back(line);
    }
    // Reference merge_no_span_structure: plain cells become one token.
    if (std::find(vocab_.begin(), vocab_.end(), "<td></td>") == vocab_.end()) {
      vocab_.push_back("<td></td>");
    }
    vocab_.erase(std::remove(vocab_.begin(), vocab_.end(), "<td>"), vocab_.end());
    vocab_.insert(vocab_.begin(), "sos");
    vocab_.push_back("eos");
  }

  TableStructure recognize(const cv::Mat& crop) {
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

    const std::array<int64_t, 4> input_shape = {1, 3, kInputSize, kInputSize};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                       input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name_.c_str()};
    auto outputs = session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                                output_name_pointers_.data(), output_name_pointers_.size());

    // Outputs are (cell corner boxes [1, S, 8], token probs [1, S, vocab]);
    // identify by trailing dimension instead of trusting declaration order.
    const auto shape_of = [&](size_t index) {
      return outputs[index].GetTensorTypeAndShapeInfo().GetShape();
    };
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
    const float* boxes = outputs[box_output].GetTensorData<float>();
    const float* probs = outputs[prob_output].GetTensorData<float>();
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
    return structure;
  }

 private:
  Ort::Env env_;
  Ort::SessionOptions options_;
  Ort::Session session_{nullptr};
  std::string input_name_;
  std::vector<std::string> output_names_;
  std::vector<const char*> output_name_pointers_;
  std::vector<std::string> vocab_;
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
