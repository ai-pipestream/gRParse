#pragma once

// The SLANet+ structure decoder on an Intel GPU through Level Zero, with no
// OpenVINO and no ONNX Runtime: src/levelzero/slanet_decoder.cl compiled to
// SPIR-V at build time (ocloc), every decode step of a table in one
// cooperative launch.  The weights come from slanet_plus_step.onnx, the same
// file the TensorRT path runs.
//
// One decoder per process and device: tables that callers hand it while a
// launch is running go out together in the next launch, so concurrent table
// crops share the GPU instead of queueing one by one.

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

// One table's decode: per step the structure logits (kVocab) and the eight
// sigmoid corner coordinates, up to and including the eos step.
struct DecodedTable {
  int steps = 0;
  std::vector<float> logits;
  std::vector<float> location;
};

class TableDecoder {
 public:
  // The process's decoder for the GPU `gpu_index` (the n of GPU.<n>, 0 for
  // GPU), built on first use from `step_model`.  A missing Level Zero
  // driver, an absent device, a kernel the driver will not build or weights
  // of the wrong shape throw, naming the cause; nothing else is built in its
  // place.
  static TableDecoder& shared(const std::filesystem::path& step_model, int gpu_index);

  // Decodes one table from its encoder features ([kPositions][kChannels]).
  // Thread-safe.  A device failure throws InferenceDeviceError.
  DecodedTable decode(std::span<const float> features);

  // "Level Zero GPU <n> (<device name>)".
  const std::string& description() const;

  TableDecoder(const std::filesystem::path& step_model, int gpu_index);
  ~TableDecoder();
  TableDecoder(const TableDecoder&) = delete;
  TableDecoder& operator=(const TableDecoder&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// The decoder's weights from the step model, re-laid out for the kernel:
// every matrix output-major, w_ih rows padded, each part 64-byte aligned.
// `offsets` gets each part's start, in the kernel's argument order.  Public
// for tests.
std::vector<float> pack_decoder_weights(const std::filesystem::path& step_model,
                                        std::vector<size_t>* offsets);

}  // namespace grparse::levelzero
