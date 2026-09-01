// Scalar conversions between the office wire and the document schema: the
// spellings of colors, instants, media types and numbers every fold shares.
// Pure functions, no state.
#pragma once

#include <cstdint>
#include <string>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>

#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

bool ends_with(const std::string& value, const std::string& suffix);

// A character count as the schema's 32-bit range, saturating rather than
// wrapping.
int32_t clamp32(long long value);

std::string base64(const std::string& data);
std::string data_uri(const std::string& mime, const std::string& bytes);

// The media type of a rendered page image. The request selects the encoding
// and the response names it; a producer that predates format selection sends
// PNG and no name.
std::string page_image_mime(officev1::PageImageFormat format);

// The office wire counts instants in epoch milliseconds; the schema wants
// them typed. Negative remainders borrow a second so the nanos stay in
// range for instants before 1970.
void set_instant(int64_t epoch_ms, google::protobuf::Timestamp* out);

// A color as #rrggbb.
std::string hex_color_always(uint32_t color_rgb);

// A run's text color. The office core reports automatic color as 0, so an
// explicit black is indistinguishable from no color at all and both stay
// unset rather than claiming a color the document never declared. A
// highlight has its own sentinel and does not go through here.
std::string hex_color(uint32_t color_rgb);

// The vertical position of a run from its escapement percentage: the office
// core raises superscript above the baseline and lowers subscript below it.
docv1::Script script_for(int escapement);

std::string mime_for_extension(const std::string& extension);

google::protobuf::Value str_value(const std::string& text);
google::protobuf::Value num_value(double number);

// "%g", the spelling the office collector's own tabular projection uses,
// so a series value reads the same whether it arrived typed or projected.
std::string double_text(double value);

}  // namespace grparse::office_fold
