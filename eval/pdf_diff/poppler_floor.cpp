// The poppler leg of the PDF backend differential: replays the exact
// poppler-cpp calls gRParse's in-process path makes (load_from_raw_data,
// text_list(text_list_include_font), page_renderer BGR24 at a DPI,
// quarter-turn orientation handling) and dumps the result for the runner.
//
// Usage: poppler_floor <pdf> <dpi> <outdir>
// Writes JSON to stdout (load status, page inventory, text boxes with font
// info) and one BGR24 PPM per page (bytes reordered to RGB for P6) into
// outdir as page-<index>.ppm.
//
// Build: see build_floor.sh (pkg-config poppler-cpp). This tool links
// GPL-licensed poppler and stays inside eval/, off every release artifact.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

namespace {

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

bool IsQuarterTurn(const poppler::page& page) {
  const auto orientation = page.orientation();
  return orientation == poppler::page::landscape ||
         orientation == poppler::page::seascape;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <pdf> <dpi> <outdir>\n", argv[0]);
    return 2;
  }
  const std::string pdf_path = argv[1];
  const double dpi = std::atof(argv[2]);
  const std::string outdir = argv[3];

  std::ifstream in(pdf_path, std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  std::unique_ptr<poppler::document> doc(
      poppler::document::load_from_raw_data(data.data(), data.size()));
  if (!doc) {
    std::printf("{\"load_ok\": false, \"pages\": []}\n");
    return 0;
  }

  poppler::page_renderer renderer;
  renderer.set_image_format(poppler::image::format_bgr24);

  std::string json = "{\"load_ok\": true, \"pages\": [";
  const int page_count = doc->pages();
  for (int i = 0; i < page_count; ++i) {
    std::unique_ptr<poppler::page> page(doc->create_page(i));
    if (!page) continue;
    const bool quarter_turn = IsQuarterTurn(*page);
    const auto rect = page->page_rect();
    const double width = quarter_turn ? rect.height() : rect.width();
    const double height = quarter_turn ? rect.width() : rect.height();

    if (i > 0) json += ",";
    json += "{\"index\": " + std::to_string(i) +
            ", \"width_pts\": " + std::to_string(width) +
            ", \"height_pts\": " + std::to_string(height) +
            ", \"quarter_turn\": " + (quarter_turn ? "true" : "false") +
            ", \"text\": [";

    const auto boxes = page->text_list(poppler::page::text_list_include_font);
    bool first = true;
    for (const auto& box : boxes) {
      const auto b = box.bbox();
      if (!first) json += ",";
      first = false;
      json += "{\"x\": " + std::to_string(b.x()) +
              ", \"y\": " + std::to_string(b.y()) +
              ", \"w\": " + std::to_string(b.width()) +
              ", \"h\": " + std::to_string(b.height()) + ", \"text\": \"" +
              JsonEscape(box.text().to_utf8().data()) + "\"";
      if (box.has_font_info()) {
        json += ", \"font_name\": \"" + JsonEscape(box.get_font_name()) +
                "\", \"font_size\": " + std::to_string(box.get_font_size());
      }
      json += "}";
    }
    json += "]}";

    const poppler::image image = renderer.render_page(page.get(), dpi, dpi);
    if (image.is_valid()) {
      const std::string ppm_path =
          outdir + "/page-" + std::to_string(i) + ".ppm";
      std::ofstream ppm(ppm_path, std::ios::binary);
      ppm << "P6\n" << image.width() << " " << image.height() << "\n255\n";
      const char* row = image.const_data();
      for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
          const char* px = row + x * 3;
          // BGR24 to the PPM's RGB order.
          ppm.put(px[2]);
          ppm.put(px[1]);
          ppm.put(px[0]);
        }
        row += image.bytes_per_row();
      }
    }
  }
  json += "]}";
  std::printf("%s\n", json.c_str());
  return 0;
}
