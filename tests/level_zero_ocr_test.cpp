// The three RapidOCR nets on the Level Zero graph runtime against ONNX
// Runtime's CPU provider, in process, on inputs cut from a real image: the
// float module must reproduce the reference within float noise, and the
// recognizer's reduced output must name the reference's best class at every
// time step, in float and in half.  Needs the models and an Intel GPU (skip
// 77 without either).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"
#include "levelzero/device.h"
#include "levelzero/graph_runtime.h"
#include "support/check.h"

namespace {

namespace fs = std::filesystem;
namespace lz = grparse::levelzero;
using grparse_test::require;

struct Reference {
  std::vector<int64_t> shape;
  std::vector<float> values;
};

Reference run_reference(Ort::Session& session, const std::vector<float>& input, const std::vector<int64_t>& shape) {
  Ort::AllocatorWithDefaultOptions allocator;
  const std::string in = session.GetInputNameAllocated(0, allocator).get();
  const std::string out = session.GetOutputNameAllocated(0, allocator).get();
  const char* in_name = in.c_str();
  const char* out_name = out.c_str();
  const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()), input.size(),
                                                      shape.data(), shape.size());
  auto outputs = session.Run(Ort::RunOptions{nullptr}, &in_name, &tensor, 1, &out_name, 1);
  Reference reference;
  reference.shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
  size_t count = 1;
  for (const int64_t dim : reference.shape) count *= static_cast<size_t>(dim);
  const float* data = outputs[0].GetTensorData<float>();
  reference.values.assign(data, data + count);
  return reference;
}

// `count` images of height x width cut from `image` at different offsets,
// normalized as the nets' inputs are ((pixel / 255 - 0.5) / 0.5), NCHW.
std::vector<float> crops(const cv::Mat& image, int count, int height, int width) {
  std::vector<float> out(static_cast<size_t>(count) * 3 * height * width);
  for (int n = 0; n < count; ++n) {
    cv::Mat crop;
    const int band = std::max(8, image.rows / (count + 1));
    cv::resize(image(cv::Rect(0, std::min(n * band, image.rows - band), image.cols, band)), crop,
               cv::Size(width, height));
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < 3; ++c) {
          out[((static_cast<size_t>(n) * 3 + c) * height + y) * width + x] =
              (static_cast<float>(crop.at<cv::Vec3b>(y, x)[c]) / 255.0F - 0.5F) / 0.5F;
        }
      }
    }
  }
  return out;
}

float max_difference(const Reference& want, const lz::graph::HostTensor& got, const std::string& what) {
  require(got.shape == want.shape && got.count() == want.values.size(), what + ": output shape differs");
  float worst = 0.0F;
  for (size_t i = 0; i < want.values.size(); ++i) worst = std::max(worst, std::abs(want.values[i] - got.floats()[i]));
  return worst;
}

}  // namespace

int main() {
  const char* models_env = std::getenv("GRPARSE_TEST_MODELS_DIR");
  const char* data_env = std::getenv("GRPARSE_TEST_DATA_DIR");
  const fs::path models = models_env ? models_env : "models";
  const fs::path det = models / "ch_PP-OCRv3_det_infer.onnx", cls = models / "ch_ppocr_mobile_v2.0_cls_infer.onnx",
                 rec = models / "ch_PP-OCRv3_rec_infer.onnx";
  for (const fs::path& model : {det, cls, rec}) {
    if (fs::is_regular_file(model)) continue;
    std::println("level-zero-ocr-test: skipped, no {}", model.string());
    return grparse_test::missing_model_exit_code();
  }
  const cv::Mat image = cv::imread((fs::path(data_env ? data_env : "tests/data") / "span_table.png").string(),
                                   cv::IMREAD_COLOR);
  if (image.empty()) {
    std::println(stderr, "level-zero-ocr-test: cannot read tests/data/span_table.png");
    return EXIT_FAILURE;
  }
  lz::Device* device = nullptr;
  try {
    device = &lz::Device::shared(0);
  } catch (const std::exception& error) {
    std::println("level-zero-ocr-test: skipped, no Level Zero GPU ({})", error.what());
    return 77;
  }
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "level-zero-ocr-test");
  const auto reference_for = [&](const fs::path& model, const std::vector<float>& input,
                                 const std::vector<int64_t>& shape) {
    Ort::Session session = grparse::make_cpu_session(env, model, grparse::kIntraOpProcessDefault);
    return run_reference(session, input, shape);
  };
  return grparse_test::run_test_main(
      "level-zero-ocr-test", "the native OCR nets match ONNX Runtime on CPU",
      {[&] {
         // Detection at two sizes through one model: the second shape needs nothing rebuilt.
         lz::graph::GraphModel model(*device, det);
         for (const auto& [h, w] : {std::pair{320, 480}, std::pair{352, 288}}) {
           const std::vector<int64_t> shape{1, 3, h, w};
           const std::vector<float> input = crops(image, 1, h, w);
           const float worst = max_difference(reference_for(det, input, shape), model.run(input.data(), shape)[0],
                                              std::format("detection {}x{}", h, w));
           require(worst < 1e-3F, std::format("detection {}x{}: max difference {}", h, w, worst));
         }
       },
       [&] {
         lz::graph::GraphModel model(*device, cls);
         const std::vector<int64_t> shape{5, 3, 48, 192};
         const std::vector<float> input = crops(image, 5, 48, 192);
         const float worst =
             max_difference(reference_for(cls, input, shape), model.run(input.data(), shape)[0], "direction");
         require(worst < 1e-4F, std::format("direction: max difference {}", worst));
       },
       [&] {
         // Recognition: the full scores in float, then the reduced output in float and in half.
         const std::vector<int64_t> shape{3, 3, 48, 416};
         const std::vector<float> input = crops(image, 3, 48, 416);
         const Reference want = reference_for(rec, input, shape);
         const size_t classes = static_cast<size_t>(want.shape[2]);
         const size_t steps = want.values.size() / classes;
         {
           lz::graph::GraphModel model(*device, rec);
           const float worst = max_difference(want, model.run(input.data(), shape)[0], "recognition");
           require(worst < 1e-4F, std::format("recognition: max difference {}", worst));
         }
         for (const bool fp16 : {false, true}) {
           lz::graph::GraphModel model(*device, rec, lz::graph::GraphOptions{.fp16 = fp16, .reduce_rows = true});
           const lz::graph::HostTensor got = model.run(input.data(), shape)[0];
           require(got.count() == steps * 4, "the reduced output holds four floats a time step");
           size_t agree = 0;
           for (size_t step = 0; step < steps; ++step) {
             const float* row = want.values.data() + step * classes;
             const size_t best = static_cast<size_t>(std::max_element(row, row + classes) - row);
             const size_t best_but_last = static_cast<size_t>(std::max_element(row, row + classes - 1) - row);
             const float* reduced = got.floats() + step * 4;
             agree += static_cast<size_t>(reduced[0]) == best && static_cast<size_t>(reduced[2]) == best_but_last;
             if (!fp16) {
               require(std::abs(reduced[1] - row[best]) < 1e-4F && std::abs(reduced[3] - row[best_but_last]) < 1e-4F,
                       std::format("recognition, reduced: score differs at step {}", step));
             }
           }
           // Float must name every step's class; half may lose a near tie.
           require(fp16 ? agree * 100 >= steps * 98 : agree == steps,
                   std::format("recognition, reduced, {}: {} of {} steps name the reference's class",
                               fp16 ? "half" : "float", agree, steps));
         }
       }});
}
