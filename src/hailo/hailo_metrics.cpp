#include "hailo_metrics.h"

#include <sstream>

namespace grparse::hailo {

std::string render_hailo_metrics(const std::vector<ModelFrames>& frames,
                                 const std::optional<DeviceStats>& stats) {
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
  if (stats) {
    out << "# HELP grparse_hailo_device_ram_bytes The chip's own memory, total and in use, read "
           "once after the models loaded.\n"
           "# TYPE grparse_hailo_device_ram_bytes gauge\n"
        << "grparse_hailo_device_ram_bytes{state=\"total\"} " << stats->ram_total_bytes << '\n'
        << "grparse_hailo_device_ram_bytes{state=\"used\"} " << stats->ram_used_bytes << '\n'
        << "# HELP grparse_hailo_device_temperature_celsius The chip's die temperature at that "
           "reading.\n"
           "# TYPE grparse_hailo_device_temperature_celsius gauge\n"
        << "grparse_hailo_device_temperature_celsius " << stats->temperature_celsius << '\n';
  }
  return out.str();
}

}  // namespace grparse::hailo
