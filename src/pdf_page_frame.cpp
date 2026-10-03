#include "grparse/pdf_page_frame.h"

#include <algorithm>
#include <optional>

namespace grparse {
namespace {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

bool valid(const pdfv1::BoundingBox& box) { return box.x1() > box.x0() && box.y1() > box.y0(); }

}  // namespace

PdfPageFrame::PdfPageFrame(const pdfv1::PageInfo& info)
    : rotation_(((info.rotation_degrees() % 360) + 360) % 360) {
  const bool quarter_turn = rotation_ == 90 || rotation_ == 270;
  // Unrotated extent: the visible box, which is the CropBox clipped to the
  // MediaBox (a CropBox reaching past the MediaBox is legal, and renderers
  // draw only the overlap), or the MediaBox alone. Without either, the
  // rendered size turned back.
  std::optional<pdfv1::BoundingBox> visible;
  if (valid(info.media_box())) visible = info.media_box();
  if (valid(info.crop_box())) {
    pdfv1::BoundingBox crop = info.crop_box();
    if (visible.has_value()) {
      crop.set_x0(std::max(crop.x0(), visible->x0()));
      crop.set_y0(std::max(crop.y0(), visible->y0()));
      crop.set_x1(std::min(crop.x1(), visible->x1()));
      crop.set_y1(std::min(crop.y1(), visible->y1()));
    }
    if (valid(crop)) visible = crop;
  }
  if (visible.has_value()) {
    origin_x_ = visible->x0();
    origin_y_ = visible->y0();
    width_ = visible->x1() - visible->x0();
    height_ = visible->y1() - visible->y0();
  } else {
    width_ = quarter_turn ? info.height_pts() : info.width_pts();
    height_ = quarter_turn ? info.width_pts() : info.height_pts();
  }
  // Geometry in the contract frame is already shifted by the CropBox
  // origin (the MediaBox's when the CropBox is missing, as the contract
  // makes them equal then), so the visible corner moves by the same amount.
  if (info.page_space() == pdfv1::PAGE_SPACE_CROP_BOX) {
    if (valid(info.crop_box())) {
      origin_x_ -= info.crop_box().x0();
      origin_y_ -= info.crop_box().y0();
    } else if (valid(info.media_box())) {
      origin_x_ -= info.media_box().x0();
      origin_y_ -= info.media_box().y0();
    }
  }
}

std::array<double, 4> PdfPageFrame::place(const pdfv1::BoundingBox& box) const {
  const auto a = to_display(box.x0(), box.y0());
  const auto b = to_display(box.x1(), box.y1());
  return {std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[0], b[0]),
          std::max(a[1], b[1])};
}

std::array<double, 2> PdfPageFrame::to_display(double x, double y) const {
  const double u = x - origin_x_;
  const double down = height_ - (y - origin_y_);
  switch (rotation_) {
    case 90:
      return {height_ - down, u};
    case 180:
      return {width_ - u, height_ - down};
    case 270:
      return {down, width_ - u};
    default:
      return {u, down};
  }
}

}  // namespace grparse
