// The Level Zero table decoder against a double-precision reference of the
// SLAHead step written here from the step model's own weights (the graph
// scripts/split_slanet.py builds), on seeded encoder features.  The reference
// is fed the decoder's own previous tokens, so the two cannot drift apart:
// every step's logits and boxes must agree within float noise, and the token
// must agree wherever the reference's top two are not a near tie (random
// features make near ties that real tables, see the acm-table receipts in
// README, do not).  Tables decoded
// concurrently, which share launches, must come back bit-identical to the
// same tables decoded alone.  Needs the models and an Intel GPU (skip 77
// without either).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <print>
#include <random>
#include <thread>
#include <vector>

#include "levelzero/onnx_initializers.h"
#include "levelzero/table_decoder.h"
#include "support/check.h"

namespace {

namespace fs = std::filesystem;
namespace lz = grparse::levelzero;
using grparse_test::require;

constexpr int P = lz::kPositions, C = lz::kChannels, H = 256, V = lz::kVocab;

struct Reference {
  std::vector<int> tokens;
  std::vector<double> logits, location;
};

// Matrices as the model stores them: [in][out].
Reference reference_decode(const std::map<std::string, lz::OnnxTensor>& w, const std::vector<float>& f,
                           const std::vector<int>& forced) {
  const auto& i2h = w.at("i2h").values;
  const auto mat = [&](const char* name, const std::vector<double>& x, int in, int out, const char* bias) {
    const auto& m = w.at(name).values;
    std::vector<double> y(out, 0.0);
    for (int o = 0; o < out; ++o) {
      double a = bias ? w.at(bias).values[o] : 0.0;
      for (int i = 0; i < in; ++i) a += x[i] * m[size_t(i) * out + o];
      y[o] = a;
    }
    return y;
  };
  std::vector<double> keys(size_t(P) * H);
  for (int p = 0; p < P; ++p)
    for (int j = 0; j < H; ++j) {
      double a = 0;
      for (int c = 0; c < C; ++c) a += double(f[size_t(p) * C + c]) * i2h[size_t(c) * H + j];
      keys[size_t(p) * H + j] = a;
    }
  const auto sig = [](double x) { return 1.0 / (1.0 + std::exp(-x)); };
  std::vector<double> hidden(H, 0.0);
  int previous = 0;
  Reference out;
  for (size_t step = 0; step < forced.size(); ++step) {
    const auto q = mat("h2h_w", hidden, H, H, "h2h_b");
    std::vector<double> energy(P);
    for (int p = 0; p < P; ++p) {
      double a = 0;
      for (int j = 0; j < H; ++j) a += std::tanh(keys[size_t(p) * H + j] + q[j]) * w.at("score").values[j];
      energy[p] = a;
    }
    const double top = *std::max_element(energy.begin(), energy.end());
    double total = 0;
    for (double& e : energy) total += (e = std::exp(e - top));
    std::vector<double> cell(C + V, 0.0);
    for (int c = 0; c < C; ++c)
      for (int p = 0; p < P; ++p) cell[c] += energy[p] / total * f[size_t(p) * C + c];
    cell[C + previous] = 1.0;
    const auto gi = mat("w_ih", cell, C + V, 3 * H, "b_ih");
    const auto gh = mat("w_hh", hidden, H, 3 * H, "b_hh");
    for (int x = 0; x < H; ++x) {
      const double r = sig(gi[x] + gh[x]), z = sig(gi[H + x] + gh[H + x]);
      const double n = std::tanh(gi[2 * H + x] + r * gh[2 * H + x]);
      hidden[x] = (1 - z) * n + z * hidden[x];
    }
    const auto logits = mat("s2_w", mat("s1_w", hidden, H, H, "s1_b"), H, V, "s2_b");
    const auto location = mat("l2_w", mat("l1_w", hidden, H, H, "l1_b"), H, 8, "l2_b");
    out.logits.insert(out.logits.end(), logits.begin(), logits.end());
    for (double l : location) out.location.push_back(sig(l));
    out.tokens.push_back(int(std::max_element(logits.begin(), logits.end()) - logits.begin()));
    previous = forced[step];
  }
  return out;
}

std::vector<float> seeded_features(unsigned seed) {
  std::mt19937 random(seed);
  std::normal_distribution<float> normal(0.0F, 1.0F);
  std::vector<float> f(size_t(P) * C);
  for (float& value : f) value = normal(random);
  return f;
}

}  // namespace

int main() {
  const char* dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
  const fs::path step = fs::path(dir ? dir : "models") / "slanet_plus_step.onnx";
  if (!fs::is_regular_file(step)) {
    std::println("level-zero-table-decoder-test: skipped, no {}", step.string());
    return grparse_test::missing_model_exit_code();
  }
  lz::TableDecoder* decoder = nullptr;
  try {
    decoder = &lz::TableDecoder::shared(step, 0);
  } catch (const std::exception& error) {
    std::println("level-zero-table-decoder-test: skipped, no Level Zero GPU ({})", error.what());
    return 77;
  }
  const auto weights = lz::read_onnx_initializers(step);
  return grparse_test::run_test_main("level-zero-table-decoder-test", "decoder ok", {[&] {
    std::vector<lz::DecodedTable> alone;
    for (unsigned seed = 1; seed <= 6; ++seed) {
      const auto features = seeded_features(seed);
      const lz::DecodedTable got = decoder->decode(features);
      std::vector<int> tokens;
      for (int s = 0; s < got.steps; ++s) {
        const float* row = got.logits.data() + size_t(s) * V;
        tokens.push_back(int(std::max_element(row, row + V) - row));
      }
      require(got.steps == lz::kMaxSteps || tokens.back() == V - 1, "a decode ends at eos or the step bound");
      const Reference want = reference_decode(weights, features, tokens);
      double logit_error = 0, box_error = 0;
      for (int s = 0; s < got.steps; ++s) {
        const float* row = got.logits.data() + size_t(s) * V;
        std::vector<double> sorted(want.logits.begin() + s * V, want.logits.begin() + (s + 1) * V);
        std::sort(sorted.begin(), sorted.end());
        require(sorted[V - 1] - sorted[V - 2] < 1e-2 || tokens[s] == want.tokens[s],
                "seed " + std::to_string(seed) + ": token differs at step " + std::to_string(s));
        for (int v = 0; v < V; ++v) logit_error = std::max(logit_error, std::abs(row[v] - want.logits[size_t(s) * V + v]));
        for (int k = 0; k < 8; ++k)
          box_error = std::max(box_error, std::abs(got.location[size_t(s) * 8 + k] - want.location[size_t(s) * 8 + k]));
      }
      require(logit_error < 2e-3 && box_error < 1e-4,
              "seed " + std::to_string(seed) + ": logit error " + std::to_string(logit_error) + ", box error " +
                  std::to_string(box_error));
      alone.push_back(got);
    }
    std::vector<lz::DecodedTable> together(alone.size());
    std::vector<std::thread> threads;
    for (size_t index = 0; index < alone.size(); ++index) {
      threads.emplace_back([&, index] { together[index] = decoder->decode(seeded_features(unsigned(index) + 1)); });
    }
    for (std::thread& thread : threads) thread.join();
    for (size_t index = 0; index < alone.size(); ++index) {
      require(together[index].logits == alone[index].logits && together[index].location == alone[index].location,
              "a table decoded beside others must match its lone decode bit for bit");
    }
  }});
}
