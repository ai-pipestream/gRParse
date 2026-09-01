#!/usr/bin/env bash
# Generate rotated-scan.pdf: page 1 of two-column.pdf rendered to a grayscale
# raster, rotated 90 degrees and wrapped back into a PDF with no text layer.
#
#   eval/scorecard/fixtures/rotated_scan.sh [out_dir]
#
# Requires two-column.pdf in out_dir (run two_column_pdf.py first), pdftoppm
# (poppler) and ImageMagick convert. 150 dpi grayscale keeps the file small.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="${1:-$here/../../../tests/golden/corpus}"
out_dir="$(cd "$out_dir" && pwd)"
source_pdf="$out_dir/two-column.pdf"
[ -f "$source_pdf" ] || { echo "missing $source_pdf; run two_column_pdf.py first" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

pdftoppm -f 1 -l 1 -r 150 -gray -png "$source_pdf" "$work/page"
page="$(ls "$work"/page*.png | head -n 1)"
convert "$page" -rotate 90 -strip "$work/rotated.png"
convert "$work/rotated.png" -units PixelsPerInch -density 150 -compress Zip "$out_dir/rotated-scan.pdf"
ls -l "$out_dir/rotated-scan.pdf"
