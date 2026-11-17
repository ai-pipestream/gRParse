#include "hailo_metrics.h"

#include <sstream>

namespace grparse::hailo {

std::string render_hailo_metrics(const std::vector<ModelFrames>& frames) {
  std::ostringstream out;
  if (!frames.empty()) {
    out << "# HELP grparse_hailo_frames_total Frames each model ran on the Hailo device "
           "(detector tiles, line crops, recognizer windows, pages, figure crops).\n"
           "# TYPE grparse_hailo_frames_total counter\n";
    for (const ModelFrames& model : frames) {
      out << "grparse_hailo_frames_total{model=\"" << model.model << "\"} " << model.frames << '\n';
    }
    out << "# HELP grparse_hailo_model_reloads_total Times a model was loaded onto the device "
           "again after the device aborted it; the call that hit it was run again on the device.\n"
           "# TYPE grparse_hailo_model_reloads_total counter\n";
    for (const ModelFrames& model : frames) {
      out << "grparse_hailo_model_reloads_total{model=\"" << model.model << "\"} " << model.reloads
          << '\n';
    }
  }
  return out.str();
}

}  // namespace grparse::hailo
