#pragma once

// SLANet+ table structure on an Intel GPU through Level Zero, with no
// OpenVINO and no ONNX Runtime: the encoder (slanet_plus_encoder.onnx) on the
// graph runtime (graph_runtime.h), the decoder as its own kernel
// (slanet_decoder.cl, weights from slanet_plus_step.onnx) compiled to SPIR-V
// at build time (ocloc) and run as one cooperative launch per batch: every
// decode step of every table in it.
//
// One instance per process and device: tables that callers hand it while a
// batch is running go out together in the next one.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace grparse::levelzero {

inline constexpr int kPositions = 256;  // encoder feature positions
inline constexpr int kChannels = 96;    // encoder feature width
inline constexpr int kVocab = 50;       // structure tokens, sos first and eos last
inline constexpr int kMaxSteps = 501;   // the released loop's bound
inline constexpr int kImageSide = 488;  // the preprocessed input, [3][488][488]

// One table's decode: per step the structure logits (kVocab) and the eight
// sigmoid corner coordinates, up to and including the eos step.
struct DecodedTable {
  int steps = 0;
  std::vector<float> logits;
  std::vector<float> location;
};

class TableStructureGpu {
 public:
  // The process's instance for GPU `gpu_index` (the n of GPU.<n>, 0 for
  // GPU), built on first use from the split model files in `models_dir`.  A
  // missing Level Zero driver, an absent device, a kernel the driver will not
  // build, or a model the lowering does not know throws, naming the cause;
  // nothing else is built in its place.
  static TableStructureGpu& shared(const std::filesystem::path& models_dir, int gpu_index);

  // One preprocessed table image ([3][488][488], normalized) in, its decode
  // out.  Thread-safe.  A device failure throws InferenceDeviceError.
  DecodedTable decode(std::span<const float> image);

  // "Level Zero GPU <n> (<device name>)".
  const std::string& description() const;

  TableStructureGpu(const std::filesystem::path& models_dir, int gpu_index);
  ~TableStructureGpu();
  TableStructureGpu(const TableStructureGpu&) = delete;
  TableStructureGpu& operator=(const TableStructureGpu&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// The decoder's weights from the step model, re-laid out for the kernel:
// every matrix output-major, w_ih rows padded, each part 64-byte aligned.
// `offsets` gets each part's start, in the kernel's argument order.
std::vector<float> pack_decoder_weights(const std::filesystem::path& step_model,
                                        std::vector<size_t>* offsets);

}  // namespace grparse::levelzero
