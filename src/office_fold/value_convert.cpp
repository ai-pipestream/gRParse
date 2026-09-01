#include "grparse/office_fold/value_convert.h"

#include <cstdio>
#include <map>

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

std::string base64(const std::string& data) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8)
        | static_cast<unsigned char>(data[i + 2]);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back(alphabet[chunk & 63]);
    i += 3;
  }
  if (i + 1 == data.size()) {
    uint32_t chunk = static_cast<unsigned char>(data[i]) << 16;
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.append("==");
  } else if (i + 2 == data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string data_uri(const std::string& mime, const std::string& bytes) {
  return "data:" + mime + ";base64," + base64(bytes);
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

google::protobuf::Value str_value(const std::string& text) {
  google::protobuf::Value value;
  value.set_string_value(text);
  return value;
}

google::protobuf::Value num_value(double number) {
  google::protobuf::Value value;
  value.set_number_value(number);
  return value;
}

std::string double_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%g", value);
  return buffer;
}

}  // namespace grparse::office_fold
