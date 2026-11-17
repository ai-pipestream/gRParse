#include "grparse_ocr_shapes.h"

#include <map>
#include <mutex>
#include <tuple>

namespace grparse {
namespace {

// net, height, width, batch: the order ocr_input_shapes() returns.
using ShapeKey = std::tuple<OcrNet, int, int, int>;

std::mutex ledger_mutex;
std::map<ShapeKey, uint64_t> ledger;

}  // namespace

std::string_view ocr_net_label(OcrNet net) {
  switch (net) {
    case OcrNet::kDetection: return "det";
    case OcrNet::kDirection: return "cls";
    case OcrNet::kRecognition: break;
  }
  return "rec";
}

void note_ocr_input_shape(OcrNet net, int64_t batch, int64_t height, int64_t width) {
  const ShapeKey key{net, static_cast<int>(height), static_cast<int>(width),
                     static_cast<int>(batch)};
  std::lock_guard<std::mutex> lock(ledger_mutex);
  ++ledger[key];
}

std::vector<OcrInputShape> ocr_input_shapes() {
  std::lock_guard<std::mutex> lock(ledger_mutex);
  std::vector<OcrInputShape> shapes;
  shapes.reserve(ledger.size());
  for (const auto& [key, runs] : ledger) {
    const auto& [net, height, width, batch] = key;
    shapes.push_back({.net = net, .batch = batch, .height = height, .width = width, .runs = runs});
  }
  return shapes;
}

size_t ocr_input_shape_count(OcrNet net) {
  std::lock_guard<std::mutex> lock(ledger_mutex);
  size_t count = 0;
  for (const auto& [key, runs] : ledger) {
    if (std::get<OcrNet>(key) == net) ++count;
  }
  return count;
}

void reset_ocr_input_shapes() {
  std::lock_guard<std::mutex> lock(ledger_mutex);
  ledger.clear();
}

}  // namespace grparse
