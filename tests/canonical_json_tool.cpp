// Development utility behind the canonical JSON validation harness
// (scripts/validate_canonical_json.py): loads a serialized
// ai.pipestream.document.v1.Document, binary by default or protobuf-JSON
// with --json, and writes the canonical JSON rendering to stdout or -o.
// Not installed and not part of the serving image.
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"

namespace {

int usage(const char* program) {
  std::println(stderr,
               "usage: {} [--json] <input|-> [-o <output>]\n"
               "  reads a binary Document (default) or protobuf-JSON (--json)\n"
               "  and writes its canonical JSON rendering",
               program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool as_json = false;
  std::string input_path;
  std::string output_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      as_json = true;
    } else if (arg == "-o") {
      if (++i >= argc) return usage(argv[0]);
      output_path = argv[i];
    } else if (input_path.empty()) {
      input_path = arg;
    } else {
      return usage(argv[0]);
    }
  }
  if (input_path.empty()) return usage(argv[0]);

  std::string data;
  if (input_path == "-") {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    data = buffer.str();
  } else {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
      std::println(stderr, "error: cannot read {}", input_path);
      return 2;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    data = buffer.str();
  }

  ai::pipestream::document::v1::Document document;
  if (as_json) {
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = true;
    const auto status =
        google::protobuf::util::JsonStringToMessage(data, &document, options);
    if (!status.ok()) {
      std::println(stderr, "error: protobuf-JSON parse failed: {}",
                   status.ToString());
      return 1;
    }
  } else if (!document.ParseFromString(data)) {
    std::println(stderr, "error: binary Document parse failed");
    return 1;
  }

  std::string rendered;
  try {
    rendered = grparse::render_canonical_json(document);
  } catch (const std::exception& error) {
    std::println(stderr, "error: rendering failed: {}", error.what());
    return 1;
  }

  if (output_path.empty()) {
    std::cout << rendered;
    return std::cout ? 0 : 1;
  }
  std::ofstream out(output_path, std::ios::binary);
  out << rendered;
  if (!out) {
    std::println(stderr, "error: cannot write {}", output_path);
    return 1;
  }
  return 0;
}
