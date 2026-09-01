#!/usr/bin/env bash
# Generate the rotated-scan fixtures from two-column.pdf: grayscale rasters
# with no text layer, turned so the parser has to find the page's
# orientation before it can read it.
#
#   eval/scorecard/fixtures/rotated_scan.sh [out_dir]
#
#   rotated-scan.pdf        page 1, turned 90 degrees clockwise
#   rotated-scan-180.pdf    page 1, turned 180 degrees
#   rotated-scan-mixed.pdf  pages 1 to 3: page 1 upright, page 2 turned 90
#                           degrees clockwise, page 3 turned 270 (90
#                           counter-clockwise)
#
# Requires two-column.pdf in out_dir (run two_column_pdf.py first), pdftoppm
# (poppler) and ImageMagick convert. 150 dpi grayscale keeps the files small;
# ImageMagick's -rotate is clockwise for positive angles. Every output is a
# pure function of two-column.pdf and this script.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="${1:-$here/../../../tests/golden/corpus}"
out_dir="$(cd "$out_dir" && pwd)"
source_pdf="$out_dir/two-column.pdf"
[ -f "$source_pdf" ] || { echo "missing $source_pdf; run two_column_pdf.py first" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# page_png <page number> <rotation degrees clockwise> <output png>
page_png() {
  pdftoppm -f "$1" -l "$1" -r 150 -gray -png "$source_pdf" "$work/p$1"
  local rendered
  rendered="$(ls "$work"/p"$1"*.png | head -n 1)"
  convert "$rendered" -rotate "$2" -strip "$3"
}

# wrap_pdf <output pdf> <png>...
wrap_pdf() {
  local out="$1"
  shift
  convert "$@" -units PixelsPerInch -density 150 -compress Zip "$out"
  ls -l "$out"
}

page_png 1 90 "$work/p1-90.png"
wrap_pdf "$out_dir/rotated-scan.pdf" "$work/p1-90.png"

page_png 1 180 "$work/p1-180.png"
wrap_pdf "$out_dir/rotated-scan-180.pdf" "$work/p1-180.png"

page_png 1 0 "$work/p1-0.png"
page_png 2 90 "$work/p2-90.png"
page_png 3 270 "$work/p3-270.png"
wrap_pdf "$out_dir/rotated-scan-mixed.pdf" "$work/p1-0.png" "$work/p2-90.png" "$work/p3-270.png"
