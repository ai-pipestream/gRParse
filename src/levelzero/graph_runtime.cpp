#include "levelzero/graph_runtime.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "grparse_inference_model.h"

namespace grparse::levelzero {
// graph_kernels.cl as SPIR-V, generated at build time (cmake/LevelZero.cmake).
extern const unsigned char kGraphKernelsSpirv[];
extern const size_t kGraphKernelsSpirvSize;
}  // namespace grparse::levelzero

namespace grparse::levelzero::graph {
namespace {

using Shape = std::vector<int64_t>;

size_t numel(const Shape& shape) {
  return std::accumulate(shape.begin(), shape.end(), size_t{1}, [](size_t a, int64_t d) { return a * size_t(d); });
}

std::string shown(const Shape& shape) {
  std::string out = "[";
  for (size_t i = 0; i < shape.size(); ++i) out += (i ? "," : "") + std::to_string(shape[i]);
  return out + "]";
}

// Kernel argument structs, laid out as graph_kernels.cl declares them.
struct NdArgs {
  int rank = 0;
  int dims[6] = {};
  int a[6] = {};
  int b[6] = {};
};
struct GemmArgs {
  int m, n, k, a_g, a_m, a_k, b_g, b_k, b_n, c_g, c_m, c_n, bias_on, act;
  float alpha, beta;
};
struct ConvArgs {
  int n, ci, ih, iw, co, oh, ow, kh, kw, sh, sw, ph, pw, groups, act;
  float alpha, beta;
};

uint32_t groups_for(size_t count, uint32_t group) { return uint32_t((count + group - 1) / group); }

}  // namespace

struct GraphModel::Kernels {
  ze_kernel_handle_t gemm, conv_gemm, conv_depthwise, conv_direct, conv_transpose, pool2d, row_mean, layer_norm, softmax_rows,
      binary, unary, gather_nd, copy_blocks, resize_nearest, global_average_pool, channel_scale, binary_same;
};

// A value during one call: its shape, and either host integers or a place
// in device memory (the weight block or the arena).
struct GraphModel::Value {
  Shape shape;
  bool host = false;
  std::vector<int64_t> ints;
  bool in_weights = false;
  size_t offset = 0;  // floats into the weight block or the arena
  bool live = false;
};

GraphModel::GraphModel(Device& device, const std::filesystem::path& model)
    : device_(device), ir_(lower_onnx(model)) {
  last_use_.assign(ir_.values.size(), 0);
  for (size_t index = 0; index < ir_.nodes.size(); ++index) {
    for (int input : ir_.nodes[index].inputs) {
      if (input >= 0) last_use_[size_t(input)] = index;
    }
  }
  for (int output : ir_.outputs) last_use_[size_t(output)] = SIZE_MAX;
  ze_module_handle_t module = device_.module(kGraphKernelsSpirv, kGraphKernelsSpirvSize, "graph");
  const auto k = [&](const char* name, uint32_t x, uint32_t y) { return device_.kernel(module, name, x, y, 1); };
  kernels_ = std::make_unique<Kernels>(Kernels{
      k("gemm", 256, 1), k("conv_gemm", 256, 1), k("conv_depthwise", 16, 4), k("conv_direct", 16, 4), k("conv_transpose", 16, 4),
      k("pool2d", 16, 4), k("row_mean", 256, 1), k("layer_norm", 256, 1), k("softmax_rows", 256, 1),
      k("binary", 256, 1), k("unary", 256, 1), k("gather_nd", 256, 1), k("copy_blocks", 256, 1),
      k("resize_nearest", 16, 4), k("global_average_pool", 256, 1), k("channel_scale", 16, 16),
      k("binary_same", 256, 1)});
  weights_ = static_cast<float*>(device_.allocate(std::max<size_t>(ir_.weights.size(), 16) * sizeof(float)));
  device_.upload(weights_, ir_.weights.data(), ir_.weights.size() * sizeof(float));
  auto queue_desc = ze_desc<ze_command_queue_desc_t>(ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC);
  queue_desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
  ze_check(zeCommandQueueCreate(device_.context(), device_.device(), &queue_desc, &queue_), "command queue");
  auto list_desc = ze_desc<ze_command_list_desc_t>(ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC);
  ze_check(zeCommandListCreate(device_.context(), device_.device(), &list_desc, &list_), "command list");
  auto fence_desc = ze_desc<ze_fence_desc_t>(ZE_STRUCTURE_TYPE_FENCE_DESC);
  ze_check(zeFenceCreate(queue_, &fence_desc, &fence_), "fence");
}

GraphModel::~GraphModel() {
  if (fence_ != nullptr) zeFenceDestroy(fence_);
  if (list_ != nullptr) zeCommandListDestroy(list_);
  if (queue_ != nullptr) zeCommandQueueDestroy(queue_);
}

std::vector<HostTensor> GraphModel::run(const float* input, const std::vector<int64_t>& shape) {
  std::lock_guard lock(mutex_);
  std::vector<HostTensor> outputs;
  record(input, shape, &outputs);
  execute();
  return outputs;
}

std::vector<HostTensor> GraphModel::run_profiled(const float* input, const std::vector<int64_t>& shape,
                                                 std::vector<double>* node_microseconds) {
  std::lock_guard lock(mutex_);
  std::vector<HostTensor> outputs;
  profiling_ = true;
  profile_nodes_.clear();
  try {
    record(input, shape, &outputs);
  } catch (...) {
    profiling_ = false;
    throw;
  }
  profiling_ = false;
  execute();
  auto properties = ze_desc<ze_device_properties_t>(ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES);
  ze_check(zeDeviceGetProperties(device_.device(), &properties), "device properties");
  node_microseconds->assign(ir_.nodes.size(), 0.0);
  for (size_t i = 0; i < profile_nodes_.size(); ++i) {
    ze_kernel_timestamp_result_t stamp{};
    ze_check(zeEventQueryKernelTimestamp(profile_events_[i], &stamp), "timestamp");
    (*node_microseconds)[profile_nodes_[i]] +=
        double(stamp.context.kernelEnd - stamp.context.kernelStart) * double(properties.timerResolution) / 1000.0;
    ze_check(zeEventHostReset(profile_events_[i]), "event reset");
  }
  return outputs;
}

void GraphModel::execute() {
  const auto step = [&](ze_result_t result, const char* what) {
    if (result != ZE_RESULT_SUCCESS) {
      throw InferenceDeviceError(std::format("{}: {} failed ({})", device_.description(), what, ze_name(result)));
    }
  };
  step(zeCommandQueueExecuteCommandLists(queue_, 1, &list_, fence_), "graph submission");
  step(zeFenceHostSynchronize(fence_, UINT64_MAX), "graph run");
  step(zeFenceReset(fence_), "fence reset");
}

void GraphModel::record(const float* input, const std::vector<int64_t>& input_shape,
                        std::vector<HostTensor>* outputs) {
  std::vector<Value> values(ir_.values.size());
  for (size_t id = 0; id < ir_.values.size(); ++id) {
    const IrValue& v = ir_.values[id];
    if (!v.constant) continue;
    values[id].shape = v.shape;
    values[id].host = v.host;
    values[id].ints = v.ints;
    values[id].in_weights = !v.host;
    values[id].offset = v.weights;
  }
  // Arena placement: first fit over freed blocks, freed after the last read.
  std::map<size_t, size_t> free_blocks;  // offset -> floats
  std::map<size_t, int> holders;         // offset -> values (a tensor and its views) holding it
  size_t arena_end = 0;
  const auto place = [&](Value& v) {
    const size_t want = (numel(v.shape) + 15) / 16 * 16;
    for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
      if (it->second < want) continue;
      v.offset = it->first;
      if (it->second > want) free_blocks[it->first + want] = it->second - want;
      free_blocks.erase(it);
      v.live = true;
      holders[v.offset] = 1;
      return;
    }
    v.offset = arena_end;
    arena_end += want;
    v.live = true;
    holders[v.offset] = 1;
  };
  const auto release = [&](Value& v) {
    if (!v.live) return;
    v.live = false;
    if (--holders[v.offset] == 0) free_blocks[v.offset] = (numel(v.shape) + 15) / 16 * 16;
  };
  std::vector<std::pair<size_t, std::function<void(float*)>>> launches;
  const auto ptr = [this](const Value& v, float* arena) { return (v.in_weights ? weights_ : arena) + v.offset; };
  const auto args = [](ze_kernel_handle_t kernel, std::initializer_list<std::pair<size_t, const void*>> list) {
    uint32_t index = 0;
    for (const auto& [size, value] : list) ze_check(zeKernelSetArgumentValue(kernel, index++, size, value), "kernel argument");
  };
  const auto launch = [this](ze_kernel_handle_t kernel, ze_group_count_t groups) {
    ze_event_handle_t signal = nullptr;
    if (profiling_) {
      const size_t at = profile_nodes_.size();
      if (at == profile_events_.size()) {
        if (profile_pool_ == nullptr) {
          auto pool = ze_desc<ze_event_pool_desc_t>(ZE_STRUCTURE_TYPE_EVENT_POOL_DESC);
          pool.flags = ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP | ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
          pool.count = 4096;
          ze_device_handle_t device = device_.device();
          ze_check(zeEventPoolCreate(device_.context(), &pool, 1, &device, &profile_pool_), "event pool");
        }
        auto desc = ze_desc<ze_event_desc_t>(ZE_STRUCTURE_TYPE_EVENT_DESC);
        desc.index = uint32_t(at);
        desc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
        ze_event_handle_t event = nullptr;
        ze_check(zeEventCreate(profile_pool_, &desc, &event), "event");
        profile_events_.push_back(event);
      }
      signal = profile_events_[at];
      profile_nodes_.push_back(current_node_);
    }
    ze_check(zeCommandListAppendLaunchKernel(list_, kernel, &groups, signal, 0, nullptr), "graph launch");
    ze_check(zeCommandListAppendBarrier(list_, nullptr, 0, nullptr), "graph barrier");
  };
  const auto P = [](const auto& value) { return std::pair<size_t, const void*>{sizeof(value), &value}; };

  Value& in = values[size_t(ir_.input)];
  in.shape = input_shape;
  place(in);
  const size_t input_offset = in.offset;
  for (size_t index = 0; index < ir_.nodes.size(); ++index) {
    const IrNode& node = ir_.nodes[index];
    const auto operand = [&](size_t i) -> Value& { return values[size_t(node.inputs[i])]; };
    Value& out = values[size_t(node.output)];
    const auto fail = [&](const std::string& why) {
      throw std::invalid_argument(std::format("graph node {} ({}): {}", node.name, node.onnx_op, why));
    };
    const auto host_ints = [&](size_t i) -> const std::vector<int64_t>& {
      if (i >= node.inputs.size() || node.inputs[i] < 0 || !operand(i).host) fail("an input is not a host integer");
      return operand(i).ints;
    };
    const auto view = [&](Value& source, Shape shape) {
      out.shape = std::move(shape);
      out.in_weights = source.in_weights;
      out.offset = source.offset;
      out.host = source.host;
      out.ints = source.ints;
      if (!source.in_weights && !source.host && source.live) {
        // The view holds the source's block too: it is free once both are released.
        out.live = true;
        ++holders[source.offset];
      }
    };
    switch (node.op) {
      case IrOp::kHost: {
        const std::string& op = node.onnx_op;
        Value& a = operand(0);
        if (op == "Shape") {
          out.host = true;
          out.ints = a.shape;
          out.shape = {int64_t(a.shape.size())};
        } else if (op == "Cast") {
          view(a, a.shape);
        } else if (op == "Gather") {
          const auto& idx = host_ints(1);
          out.host = true;
          for (int64_t i : idx) out.ints.push_back(a.ints.at(size_t(i < 0 ? i + int64_t(a.ints.size()) : i)));
          out.shape = operand(1).shape;
        } else if (op == "Squeeze" || op == "Unsqueeze") {
          std::vector<int64_t> axes = node.ints;
          if (node.inputs.size() > 1) axes = host_ints(1);
          Shape shape = a.shape;
          const int64_t rank = int64_t(shape.size()) + (op == "Unsqueeze" ? int64_t(axes.size()) : 0);
          for (int64_t& axis : axes) axis = axis < 0 ? axis + rank : axis;
          std::sort(axes.begin(), axes.end());
          if (op == "Squeeze") {
            if (axes.empty()) std::erase(shape, 1);
            for (auto it = axes.rbegin(); it != axes.rend(); ++it) shape.erase(shape.begin() + *it);
          } else {
            for (int64_t axis : axes) shape.insert(shape.begin() + axis, 1);
          }
          view(a, shape);
        } else if (op == "Reshape") {
          const auto& target = host_ints(1);
          Shape shape(target.begin(), target.end());
          int infer = -1;
          size_t known = 1;
          for (size_t d = 0; d < shape.size(); ++d) {
            if (shape[d] == 0) shape[d] = a.shape.at(d);
            if (shape[d] == -1) {
              infer = int(d);
            } else {
              known *= size_t(shape[d]);
            }
          }
          if (infer >= 0) shape[size_t(infer)] = int64_t(numel(a.shape) / std::max<size_t>(known, 1));
          if (numel(shape) != numel(a.shape)) fail("reshape of " + shown(a.shape) + " to " + shown(shape));
          view(a, shape);
        } else if (op == "Slice") {
          const auto& starts = host_ints(1);
          const auto& ends = host_ints(2);
          std::vector<int64_t> axes, steps;
          if (node.inputs.size() > 3 && node.inputs[3] >= 0) axes = host_ints(3);
          if (node.inputs.size() > 4 && node.inputs[4] >= 0) steps = host_ints(4);
          if (axes.empty()) {
            for (size_t i = 0; i < starts.size(); ++i) axes.push_back(int64_t(i));
          }
          if (steps.empty()) steps.assign(starts.size(), 1);
          const Shape& dims = a.host ? Shape{int64_t(a.ints.size())} : a.shape;
          Shape begin(dims.size(), 0), step(dims.size(), 1), shape = dims;
          for (size_t i = 0; i < axes.size(); ++i) {
            const size_t axis = size_t(axes[i] < 0 ? axes[i] + int64_t(dims.size()) : axes[i]);
            const int64_t n = dims[axis];
            if (steps[i] != 1) fail("a slice step other than 1");
            const auto clamp = [n](int64_t at) { return std::clamp(at < 0 ? at + n : at, int64_t{0}, n); };
            begin[axis] = clamp(starts[i]);
            shape[axis] = std::max<int64_t>(clamp(ends[i]) - begin[axis], 0);
          }
          if (a.host) {
            out.host = true;
            out.ints.assign(a.ints.begin() + begin[0], a.ints.begin() + begin[0] + shape[0]);
            out.shape = shape;
            break;
          }
          out.shape = shape;
          place(out);
          NdArgs nd{.rank = int(shape.size())};
          int64_t stride = 1, start = 0;
          for (int d = int(shape.size()) - 1; d >= 0; --d) {
            nd.dims[d] = int(shape[size_t(d)]);
            nd.a[d] = int(stride);
            start += begin[size_t(d)] * stride;
            stride *= dims[size_t(d)];
          }
          nd.b[0] = int(start);
          const int count = int(numel(shape));
          launches.emplace_back(index, [=, this, &a_ref = a, &o_ref = out](float* arena) {
            const float* src = ptr(a_ref, arena);
            float* dst = ptr(o_ref, arena);
            args(kernels_->gather_nd, {P(src), P(dst), P(nd), P(count)});
            launch(kernels_->gather_nd, {groups_for(size_t(count), 256), 1, 1});
          });
        } else if (op == "Concat") {
          bool all_host = true;
          for (int id : node.inputs) all_host = all_host && values[size_t(id)].host;
          if (all_host) {
            out.host = true;
            for (int id : node.inputs) {
              const auto& v = values[size_t(id)].ints;
              out.ints.insert(out.ints.end(), v.begin(), v.end());
            }
            out.shape = {int64_t(out.ints.size())};
            break;
          }
          Shape shape = a.shape;
          const size_t axis = size_t(node.axis < 0 ? node.axis + int(shape.size()) : node.axis);
          shape[axis] = 0;
          for (int id : node.inputs) shape[axis] += values[size_t(id)].shape.at(axis);
          out.shape = shape;
          place(out);
          const size_t outer = numel(Shape(shape.begin(), shape.begin() + int64_t(axis)));
          const size_t out_inner = numel(Shape(shape.begin() + int64_t(axis), shape.end()));
          size_t offset = 0;
          for (int id : node.inputs) {
            Value& part = values[size_t(id)];
            const size_t inner = numel(Shape(part.shape.begin() + int64_t(axis), part.shape.end()));
            const int o = int(outer), i = int(inner), s = int(out_inner);
            launches.emplace_back(index, [=, this, &p_ref = part, &o_ref = out](float* arena) {
              const float* src = ptr(p_ref, arena);
              float* dst = ptr(o_ref, arena) + offset;
              args(kernels_->copy_blocks, {P(src), P(dst), P(o), P(i), P(s)});
              launch(kernels_->copy_blocks, {groups_for(size_t(o) * size_t(i), 256), 1, 1});
            });
            offset += inner;
          }
        } else {
          fail("an op the runtime does not run");
        }
        break;
      }
      case IrOp::kView: {
        Value& a = operand(0);
        if (node.onnx_op == "Flatten") {
          const size_t axis = size_t(node.axis < 0 ? node.axis + int(a.shape.size()) : node.axis);
          view(a, {int64_t(numel(Shape(a.shape.begin(), a.shape.begin() + int64_t(axis)))),
                   int64_t(numel(Shape(a.shape.begin() + int64_t(axis), a.shape.end())))});
        } else {
          view(a, a.shape);
        }
        break;
      }
      default: {
        // Device operations write a fresh output buffer.
        std::function<void(float*)> body;
        Value& a = operand(0);
        if (a.host) fail("a device operation on a host value");
        switch (node.op) {
          case IrOp::kConv:
          case IrOp::kConvTranspose:
          case IrOp::kAveragePool:
          case IrOp::kMaxPool: {
            if (a.shape.size() != 4) fail("a 4-D input, got " + shown(a.shape));
            ConvArgs c{};
            c.n = int(a.shape[0]);
            c.ci = int(a.shape[1]);
            c.ih = int(a.shape[2]);
            c.iw = int(a.shape[3]);
            c.kh = node.kernel_h;
            c.kw = node.kernel_w;
            c.sh = node.stride_h;
            c.sw = node.stride_w;
            c.ph = node.pad_h;
            c.pw = node.pad_w;
            c.groups = node.groups;
            c.act = int(node.activation);
            c.alpha = node.alpha;
            c.beta = node.beta;
            const bool pool = node.op == IrOp::kAveragePool || node.op == IrOp::kMaxPool;
            c.co = pool ? c.ci : node.out_channels;
            if (node.op == IrOp::kConvTranspose) {
              c.oh = c.ih * c.sh;
              c.ow = c.iw * c.sw;
            } else {
              const auto extent = [&](int size, int k, int s, int p) {
                const int span = size + 2 * p - k;
                return (node.ceil_mode ? (span + s - 1) / s : span / s) + 1;
              };
              c.oh = extent(c.ih, c.kh, c.sh, c.ph);
              c.ow = extent(c.iw, c.kw, c.sw, c.pw);
            }
            out.shape = {c.n, c.co, c.oh, c.ow};
            place(out);
            const size_t w = node.weights, b = node.bias;
            const bool pointwise = node.op == IrOp::kConv && c.kh == 1 && c.kw == 1 && c.sh == 1 && c.sw == 1 &&
                                   c.ph == 0 && c.pw == 0 && c.groups == 1;
            const bool depthwise = node.op == IrOp::kConv && c.groups == c.ci && c.groups == c.co;
            const int max_pool = node.op == IrOp::kMaxPool, include_pad = node.count_include_pad;
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* src = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              const float* wp = weights_ + w;
              const float* bp = weights_ + b;
              const ze_group_count_t spatial{groups_for(size_t(c.ow), 16), groups_for(size_t(c.oh), 4),
                                             uint32_t(c.n * c.co)};
              if (pool) {
                args(kernels_->pool2d, {P(src), P(dst), P(c), P(max_pool), P(include_pad)});
                launch(kernels_->pool2d, spatial);
              } else if (pointwise) {
                const int hw = c.ih * c.iw;
                GemmArgs g{c.co, hw, c.ci, 0, c.ci, 1, c.ci * hw, hw, 1, c.co * hw, hw, 1, 1, c.act, c.alpha, c.beta};
                args(kernels_->gemm, {P(wp), P(src), P(dst), P(bp), P(g)});
                launch(kernels_->gemm, {groups_for(size_t(hw), 64), groups_for(size_t(c.co), 64), uint32_t(c.n)});
              } else if (depthwise) {
                args(kernels_->conv_depthwise, {P(src), P(dst), P(wp), P(bp), P(c)});
                // Eight outputs of a row per work item (DW_R in kernels_mobile.cl).
                const size_t items = size_t(c.oh) * size_t((c.ow + 7) / 8);
                launch(kernels_->conv_depthwise, {groups_for(items, 16), groups_for(size_t(c.n * c.co), 4), 1});
              } else if (node.op == IrOp::kConvTranspose) {
                args(kernels_->conv_transpose, {P(src), P(dst), P(wp), P(bp), P(c)});
                launch(kernels_->conv_transpose, spatial);
              } else if (c.groups == 1) {
                args(kernels_->conv_gemm, {P(src), P(dst), P(wp), P(bp), P(c)});
                launch(kernels_->conv_gemm, {groups_for(size_t(c.oh) * size_t(c.ow), 64), groups_for(size_t(c.co), 64),
                                             uint32_t(c.n)});
              } else {
                args(kernels_->conv_direct, {P(src), P(dst), P(wp), P(bp), P(c)});
                launch(kernels_->conv_direct, {groups_for(size_t(c.ow), 16), groups_for(size_t(c.oh), 4),
                                               uint32_t(c.n * ((c.co + 7) / 8))});
              }
            };
            break;
          }
          case IrOp::kMatMulConst:
          case IrOp::kMatMul: {
            const Value& bv = node.op == IrOp::kMatMul ? operand(1) : a;
            const int k = int(a.shape.back());
            const int n = node.op == IrOp::kMatMulConst ? node.n : int(bv.shape.back());
            if (node.op == IrOp::kMatMulConst && k != node.k) fail("a MatMul over " + shown(a.shape));
            Shape shape = a.shape;
            shape.back() = n;
            out.shape = shape;
            place(out);
            const int m = int(a.shape.size() >= 2 ? a.shape[a.shape.size() - 2] : 1);
            int groups = 1;
            for (size_t d = 0; d + 2 < a.shape.size(); ++d) groups *= int(a.shape[d]);
            GemmArgs g{};
            if (node.op == IrOp::kMatMulConst) {
              g = {m * groups, n, k, 0, k, 1, 0, n, 1, 0, n, 1, 2, int(node.activation), node.alpha, node.beta};
              groups = 1;
            } else {
              if (bv.shape.size() != a.shape.size() || bv.shape[bv.shape.size() - 2] != k) {
                fail("a MatMul of " + shown(a.shape) + " by " + shown(bv.shape));
              }
              g = {m, n, k, m * k, k, 1, k * n, n, 1, m * n, n, 1, 0, 0, 0.0F, 0.0F};
            }
            const size_t w = node.weights, b = node.bias;
            const bool constant = node.op == IrOp::kMatMulConst;
            body = [=, this, &a_ref = a, &b_ref = bv, &o_ref = out](float* arena) {
              const float* left = ptr(a_ref, arena);
              const float* right = constant ? weights_ + w : ptr(b_ref, arena);
              float* dst = ptr(o_ref, arena);
              const float* bp = weights_ + b;
              args(kernels_->gemm, {P(left), P(right), P(dst), P(bp), P(g)});
              launch(kernels_->gemm, {groups_for(size_t(g.n), 64), groups_for(size_t(g.m), 64), uint32_t(groups)});
            };
            break;
          }
          case IrOp::kLayerNorm:
          case IrOp::kReduceMean:
          case IrOp::kSoftmax: {
            const int cols = int(a.shape.back());
            const int rows = int(numel(a.shape) / size_t(cols));
            if (node.op == IrOp::kReduceMean) {
              if (node.ints.size() != 1 || (node.ints[0] != -1 && node.ints[0] != int64_t(a.shape.size()) - 1)) {
                fail("a ReduceMean over the last axis");
              }
              out.shape = a.shape;
              out.shape.back() = 1;
            } else {
              if (node.op == IrOp::kSoftmax && node.axis != -1 && node.axis != int(a.shape.size()) - 1) {
                fail("a softmax over the last axis");
              }
              out.shape = a.shape;
            }
            place(out);
            const IrOp op = node.op;
            const size_t w = node.weights, b = node.bias;
            const float eps = node.alpha;
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* src = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              if (op == IrOp::kSoftmax) {
                args(kernels_->softmax_rows, {P(src), P(dst), P(cols)});
                launch(kernels_->softmax_rows, {uint32_t(rows), 1, 1});
              } else if (op == IrOp::kLayerNorm) {
                const float* gp = weights_ + w;
                const float* bp = weights_ + b;
                args(kernels_->layer_norm, {P(src), P(dst), P(gp), P(bp), P(rows), P(cols), P(eps)});
                launch(kernels_->layer_norm, {groups_for(size_t(rows), 16), 1, 1});
              } else {
                args(kernels_->row_mean, {P(src), P(dst), P(rows), P(cols)});
                launch(kernels_->row_mean, {groups_for(size_t(rows), 16), 1, 1});
              }
            };
            break;
          }
          case IrOp::kBinary: {
            const Value& bv = operand(1);
            const size_t rank = std::max(a.shape.size(), bv.shape.size());
            if (rank > 6) fail("more than six dimensions");
            Shape shape(rank, 1);
            NdArgs nd{.rank = int(rank)};
            const auto aligned = [&](const Shape& s, size_t d) {
              return d + s.size() >= rank ? s[d + s.size() - rank] : int64_t{1};
            };
            for (size_t d = 0; d < rank; ++d) {
              const int64_t x = aligned(a.shape, d), y = aligned(bv.shape, d);
              if (x != y && x != 1 && y != 1) fail("broadcast of " + shown(a.shape) + " with " + shown(bv.shape));
              shape[d] = std::max(x, y);
            }
            int64_t sa = 1, sb = 1;
            for (int d = int(rank) - 1; d >= 0; --d) {
              nd.dims[d] = int(shape[size_t(d)]);
              const int64_t x = aligned(a.shape, size_t(d)), y = aligned(bv.shape, size_t(d));
              nd.a[d] = x == 1 ? 0 : int(sa);
              nd.b[d] = y == 1 ? 0 : int(sb);
              sa *= x;
              sb *= y;
            }
            out.shape = shape;
            place(out);
            const int count = int(numel(shape)), op = int(node.binary);
            // The two shapes the mobile blocks are made of get their own
            // kernels: a residual add, and a per-channel gate on a feature map.
            const bool add_or_mul = node.binary == BinaryOp::kAdd || node.binary == BinaryOp::kMul;
            const bool same = add_or_mul && a.shape == bv.shape;
            const bool gate = node.binary == BinaryOp::kMul && a.shape.size() == 4 && bv.shape.size() == 4 &&
                              bv.shape[0] == a.shape[0] && bv.shape[1] == a.shape[1] && bv.shape[2] == 1 &&
                              bv.shape[3] == 1;
            const int planes = gate ? int(a.shape[0] * a.shape[1]) : 0, hw = gate ? int(a.shape[2] * a.shape[3]) : 0;
            body = [=, this, &a_ref = a, &b_ref = bv, &o_ref = out](float* arena) {
              const float* x = ptr(a_ref, arena);
              const float* y = ptr(b_ref, arena);
              float* dst = ptr(o_ref, arena);
              if (same) {
                args(kernels_->binary_same, {P(x), P(y), P(dst), P(count), P(op)});
                launch(kernels_->binary_same, {groups_for(size_t(count), 256), 1, 1});
                return;
              }
              if (gate) {
                args(kernels_->channel_scale, {P(x), P(y), P(dst), P(planes), P(hw)});
                launch(kernels_->channel_scale, {groups_for(size_t(hw), 16), groups_for(size_t(planes), 16), 1});
                return;
              }
              args(kernels_->binary, {P(x), P(y), P(dst), P(nd), P(count), P(op)});
              launch(kernels_->binary, {groups_for(size_t(count), 256), 1, 1});
            };
            break;
          }
          case IrOp::kUnary: {
            out.shape = a.shape;
            place(out);
            const int count = int(numel(a.shape)), op = int(node.unary);
            const float alpha = node.alpha, beta = node.beta;
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* x = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              args(kernels_->unary, {P(x), P(dst), P(count), P(op), P(alpha), P(beta)});
              launch(kernels_->unary, {groups_for(size_t(count), 256), 1, 1});
            };
            break;
          }
          case IrOp::kGlobalAveragePool: {
            out.shape = {a.shape[0], a.shape[1], 1, 1};
            place(out);
            const int planes = int(a.shape[0] * a.shape[1]), hw = int(a.shape[2] * a.shape[3]);
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* x = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              args(kernels_->global_average_pool, {P(x), P(dst), P(planes), P(hw)});
              launch(kernels_->global_average_pool, {groups_for(size_t(planes), 16), 1, 1});
            };
            break;
          }
          case IrOp::kResize: {
            float scale_h = 0, scale_w = 0;
            int oh = 0, ow = 0;
            const int ih = int(a.shape[2]), iw = int(a.shape[3]);
            if (node.inputs.size() > 3 && node.inputs[3] >= 0 && operand(3).host && !operand(3).ints.empty()) {
              oh = int(operand(3).ints[2]);
              ow = int(operand(3).ints[3]);
              scale_h = float(oh) / float(ih);
              scale_w = float(ow) / float(iw);
            } else {
              const IrValue& scales = ir_.values[size_t(node.inputs.at(2))];
              if (!scales.constant || scales.host || scales.shape != Shape{4}) fail("a resize by constant scales");
              scale_h = ir_.weights[scales.weights + 2];
              scale_w = ir_.weights[scales.weights + 3];
              oh = int(float(ih) * scale_h);
              ow = int(float(iw) * scale_w);
            }
            out.shape = {a.shape[0], a.shape[1], oh, ow};
            place(out);
            const uint32_t planes = uint32_t(a.shape[0] * a.shape[1]);
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* x = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              args(kernels_->resize_nearest, {P(x), P(dst), P(ih), P(iw), P(oh), P(ow), P(scale_h), P(scale_w)});
              launch(kernels_->resize_nearest, {groups_for(size_t(ow), 16), groups_for(size_t(oh), 4), planes});
            };
            break;
          }
          case IrOp::kTranspose: {
            const Shape& dims = a.shape;
            Shape perm = node.ints;
            if (perm.empty()) {
              for (size_t d = dims.size(); d-- > 0;) perm.push_back(int64_t(d));
            }
            if (perm.size() != dims.size() || dims.size() > 6) fail("a transpose of " + shown(dims));
            Shape in_strides(dims.size(), 1);
            for (int d = int(dims.size()) - 2; d >= 0; --d) in_strides[size_t(d)] = in_strides[size_t(d) + 1] * dims[size_t(d) + 1];
            Shape shape(dims.size());
            NdArgs nd{.rank = int(dims.size())};
            for (size_t d = 0; d < dims.size(); ++d) {
              shape[d] = dims[size_t(perm[d])];
              nd.dims[d] = int(shape[d]);
              nd.a[d] = int(in_strides[size_t(perm[d])]);
            }
            out.shape = shape;
            place(out);
            const int count = int(numel(shape));
            body = [=, this, &a_ref = a, &o_ref = out](float* arena) {
              const float* x = ptr(a_ref, arena);
              float* dst = ptr(o_ref, arena);
              args(kernels_->gather_nd, {P(x), P(dst), P(nd), P(count)});
              launch(kernels_->gather_nd, {groups_for(size_t(count), 256), 1, 1});
            };
            break;
          }
          default:
            fail("an operation the runtime does not run");
        }
        launches.emplace_back(index, std::move(body));
        break;
      }
    }
    for (int input : node.inputs) {
      if (input >= 0 && last_use_[size_t(input)] == index) release(values[size_t(input)]);
    }
  }
  // Grow the arena to this call's peak, then record.
  if (arena_end > arena_floats_) {
    if (arena_ != nullptr) zeMemFree(device_.context(), arena_);
    arena_floats_ = arena_end + arena_end / 2;
    arena_ = static_cast<float*>(device_.allocate(arena_floats_ * sizeof(float)));
  }
  ze_check(zeCommandListReset(list_), "command list reset");
  ze_check(zeCommandListAppendMemoryCopy(list_, arena_ + input_offset, input, numel(input_shape) * sizeof(float),
                                         nullptr, 0, nullptr),
           "input upload");
  ze_check(zeCommandListAppendBarrier(list_, nullptr, 0, nullptr), "graph barrier");
  for (const auto& [node_index, body] : launches) {
    current_node_ = node_index;
    body(arena_);
  }
  outputs->reserve(ir_.outputs.size());
  for (int id : ir_.outputs) {
    const Value& v = values[size_t(id)];
    HostTensor& t = outputs->emplace_back();
    t.shape = v.shape;
    t.data.resize(numel(v.shape));
    ze_check(zeCommandListAppendMemoryCopy(list_, t.data.data(), ptr(v, arena_), t.data.size() * sizeof(float),
                                           nullptr, 0, nullptr),
             "output readback");
  }
  ze_check(zeCommandListClose(list_), "command list close");
}

}  // namespace grparse::levelzero::graph
