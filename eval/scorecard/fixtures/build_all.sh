#!/usr/bin/env bash
# Regenerate every generated scorecard fixture into tests/golden/corpus.
#
#   eval/scorecard/fixtures/build_all.sh [out_dir]
#
# Needs uv, LibreOffice (soffice), poppler (pdftoppm) and ImageMagick
# (convert). The three grpc-pdf-inspector PDFs (long-text, mixed,
# scanned-image) are copied, not generated here; see corpus.json notes.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="${1:-$here/../../../tests/golden/corpus}"
mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"

uv run python "$here/two_column_pdf.py" "$out_dir"
uv run --with python-pptx python "$here/pptx_notes.py" "$out_dir"
uv run --with python-docx python "$here/docx_figures.py" "$out_dir"
uv run --with openpyxl python "$here/xlsx_sixty_sheets.py" "$out_dir"
uv run --with python-docx python "$here/docx_form.py" "$out_dir"
bash "$here/rotated_scan.sh" "$out_dir"
# Chart fixtures (chart_data.py is the shared data set): the workbook also
# paints its charts to PNG for eval/chart_derender, so matplotlib rides along.
uv run --with openpyxl --with matplotlib python "$here/xlsx_charts.py" "$out_dir" "$here/../../chart_derender/renders"
uv run --with python-pptx python "$here/pptx_charts.py" "$out_dir"
