#pragma once

#include <array>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

namespace grparse {

// Maps a PdfBackendService page's geometry into the top-left frame of the
// page as rendered. The contract's boxes are PDF points, bottom-left
// origin, before the page's /Rotate; the rendered page is the CropBox
// (clipped to the MediaBox) with /Rotate applied, so a box shifts to the
// visible box's corner, flips to a top-left origin, and turns clockwise
// with the page.
//
// PageInfo.page_space names the frame the backend measured in:
// PAGE_SPACE_CROP_BOX geometry is already relative to the CropBox origin,
// PAGE_SPACE_USER is unshifted user space, and PAGE_SPACE_UNSPECIFIED (a
// backend released before the field existed) is read as PAGE_SPACE_USER.
// Either way the same page lands in the same place.
class PdfPageFrame {
 public:
  explicit PdfPageFrame(const ai::protomolt::parse::pdf::v1::PageInfo& info);

  // Width and height of the rendered page, in points.
  double display_width() const { return rotation_ % 180 == 0 ? width_ : height_; }
  double display_height() const { return rotation_ % 180 == 0 ? height_ : width_; }

  // The axis-aligned box in the rendered top-left frame: {left, top, right,
  // bottom} in points.
  std::array<double, 4> place(const ai::protomolt::parse::pdf::v1::BoundingBox& box) const;

 private:
  std::array<double, 2> to_display(double x, double y) const;

  int rotation_;
  // The visible box's bottom-left corner in the backend's frame.
  double origin_x_ = 0.0;
  double origin_y_ = 0.0;
  double width_ = 0.0;
  double height_ = 0.0;
};

}  // namespace grparse
