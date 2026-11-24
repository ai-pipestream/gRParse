#pragma once

// An ONNX vision model run natively on an Intel GPU through Level Zero: the
// graph lowered by graph_ir.h, the kernels of graph_kernels.cl.  Shapes are
// worked out per call from the input's shape and handed to the kernels as
// arguments, so an input of any size runs without compiling anything; the
// launches go into one command list per call, executed and waited for once.

#include <level_zero/ze_api.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "levelzero/device.h"
#include "levelzero/graph_ir.h"

namespace grparse::levelzero::graph {

struct HostTensor {
  std::vector<int64_t> shape;
  std::vector<float> data;
};

struct GraphOptions {
  // Activations stored as half and the GEMM-shaped layers run on the matrix
  // engines (XMX), F32 sums; otherwise everything is float.
  bool fp16 = false;
};

class GraphModel {
 public:
  // Lowers the model and uploads its weights to `device`.  Throws naming the
  // cause (an op the lowering does not know, a kernel the driver will not
  // build).
  GraphModel(Device& device, const std::filesystem::path& model, GraphOptions options = {});
  ~GraphModel();
  GraphModel(const GraphModel&) = delete;
  GraphModel& operator=(const GraphModel&) = delete;

  // Runs the graph on one float input of `shape`; returns every graph
  // output.  One call at a time per instance.  A device failure throws
  // InferenceDeviceError; a shape the graph cannot take, std::invalid_argument.
  std::vector<HostTensor> run(const float* input, const std::vector<int64_t>& shape);

  // The same, with each node's GPU time: per IR node, microseconds summed
  // over its launches.  For benchmarks.
  std::vector<HostTensor> run_profiled(const float* input, const std::vector<int64_t>& shape,
                                       std::vector<double>* node_microseconds);

  const IrGraph& ir() const { return ir_; }

 private:
  struct Kernels;
  struct Value;

  // The launches of one input shape, recorded once into a closed command
  // list and executed again on every later call of that shape: kernel
  // arguments and arena addresses are fixed when a launch is appended, so a
  // repeated shape costs no host work beyond the input and output copies.
  struct Plan {
    std::vector<int64_t> shape;
    ze_command_list_handle_t list = nullptr;
    std::vector<std::vector<int64_t>> output_shapes;
    std::vector<size_t> output_floats;  // each output's start in io_out_
    uint64_t used = 0;
  };

  Plan& plan_for(const std::vector<int64_t>& shape);
  void record(const std::vector<int64_t>& shape, Plan* plan);
  void drop_plans();
  std::vector<HostTensor> execute(const Plan& plan, const float* input);
  void build_fp16_weights();

  Device& device_;
  IrGraph ir_;
  std::vector<size_t> last_use_;  // per value, the last node index that reads it
  std::unique_ptr<Kernels> kernels_;
  bool fp16_ = false;
  size_t esize_ = 4;               // bytes per activation element
  float* weights_ = nullptr;       // the IR's weight block, float
  void* weights_real_ = nullptr;   // FP16: the same block as half, index for index
  uint16_t* xmx_ = nullptr;        // FP16: GEMM weights laid out for gemm_xmx
  std::vector<size_t> xmx_offsets_;  // per node, halves into xmx_, or SIZE_MAX
  char* arena_ = nullptr;
  size_t arena_elements_ = 0;
  char* io_in_ = nullptr;          // float staging for the input and the outputs, host memory
  size_t io_in_floats_ = 0;
  char* io_out_ = nullptr;
  size_t io_out_floats_ = 0;
  ze_command_queue_handle_t queue_ = nullptr;
  ze_command_list_handle_t list_ = nullptr;      // the list record() is appending to
  std::vector<Plan> plans_;                      // most recently used shapes, at most kPlans
  uint64_t plan_clock_ = 0;
  ze_fence_handle_t fence_ = nullptr;
  std::mutex mutex_;
  // Profiling: one timestamp event per launch, and the node each belongs to.
  ze_event_pool_handle_t profile_pool_ = nullptr;
  std::vector<ze_event_handle_t> profile_events_;
  std::vector<size_t> profile_nodes_;
  bool profiling_ = false;
  size_t current_node_ = 0;
};

}  // namespace grparse::levelzero::graph
