#include "grparse/office_fold/value_convert.h"

#include <charconv>
#include <cstdio>
#include <map>
#include <system_error>

namespace grparse::office_fold {

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size()
      && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int32_t clamp32(long long value) {
  if (value < 0) return 0;
  if (value > INT32_MAX) return INT32_MAX;
  return static_cast<int32_t>(value);
}

std::string page_image_mime(officev1::PageImageFormat format) {
  switch (format) {
    case officev1::PAGE_IMAGE_FORMAT_JPEG: return "image/jpeg";
    case officev1::PAGE_IMAGE_FORMAT_WEBP: return "image/webp";
    case officev1::PAGE_IMAGE_FORMAT_SVG: return "image/svg+xml";
    default: return "image/png";
  }
}

void set_instant(int64_t epoch_ms, google::protobuf::Timestamp* out) {
  int64_t seconds = epoch_ms / 1000;
  int64_t millis = epoch_ms % 1000;
  if (millis < 0) {
    millis += 1000;
    seconds -= 1;
  }
  out->set_seconds(seconds);
  out->set_nanos(static_cast<int32_t>(millis * 1000000));
}

std::string hex_color_always(uint32_t color_rgb) {
  char text[8];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x", (color_rgb >> 16) & 0xff,
                (color_rgb >> 8) & 0xff, color_rgb & 0xff);
  return text;
}

std::string hex_color(uint32_t color_rgb) {
  if (color_rgb == 0) return std::string();
  return hex_color_always(color_rgb);
}

docv1::Script script_for(int escapement) {
  if (escapement > 0) return docv1::SCRIPT_SUPER;
  if (escapement < 0) return docv1::SCRIPT_SUB;
  return docv1::SCRIPT_UNSPECIFIED;
}

std::string mime_for_extension(const std::string& extension) {
  static const std::map<std::string, std::string> kMimes = {
      {"doc", "application/msword"},
      {"docx", "application/vnd.openxmlformats-officedocument"
               ".wordprocessingml.document"},
      {"xls", "application/vnd.ms-excel"},
      {"xlsx", "application/vnd.openxmlformats-officedocument"
               ".spreadsheetml.sheet"},
      {"ppt", "application/vnd.ms-powerpoint"},
      {"pptx", "application/vnd.openxmlformats-officedocument"
               ".presentationml.presentation"},
      {"odt", "application/vnd.oasis.opendocument.text"},
      {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"odp", "application/vnd.oasis.opendocument.presentation"},
      {"odg", "application/vnd.oasis.opendocument.graphics"},
      {"fodt", "application/vnd.oasis.opendocument.text"},
      {"fods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"fodp", "application/vnd.oasis.opendocument.presentation"},
      {"fodg", "application/vnd.oasis.opendocument.graphics"},
      {"rtf", "application/rtf"},
      {"csv", "text/csv"},
      {"txt", "text/plain"},
      {"html", "text/html"},
      {"pdf", "application/pdf"},
  };
  auto found = kMimes.find(extension);
  return found != kMimes.end() ? found->second : "application/octet-stream";
}

std::string double_text(double value) {
  // The plain form of to_chars is the shortest spelling that reads back as
  // the same double, in fixed notation unless the exponent form is shorter.
  // "%g" kept six significant digits, so 1234567.89 was spelled
  // "1.23457e+06" in the text every export and chunk reads.
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc()) return std::to_string(value);
  return std::string(buffer, end);
}

}  // namespace grparse::office_fold
