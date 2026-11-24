#!/usr/bin/env bash
# Render the paper set used to calibrate and evaluate layout_800x608 and figure_224.
#
# Usage: render_pages.sh SAMPLE_PDF_DIR MORE_PDF_DIR OUT_DIR [EXTRA_COUNT]
#
# SAMPLE_PDF_DIR holds the papers that have a reference JSON (gRParse CPU
# export). Sorted by path (C locale) they become acm001..acmNNN: 001-060 are
# calibration, 061 and up are held out for evaluation. MORE_PDF_DIR is a larger
# set; its first EXTRA_COUNT papers (default 120) whose file name is not in the
# sample become ext001.. and are calibration only. Pages render at 200 dpi, the
# server's default raster, to OUT_DIR/{calib,eval}/<id>-<page>.png, and
# OUT_DIR/docs.tsv records id, split, sha256 and source path.
set -euo pipefail
sample=$1 more=$2 out=$3 extra=${4:-120}
mkdir -p "$out/calib" "$out/eval"
export LC_ALL=C
: > "$out/docs.tsv"
n=0
while IFS= read -r pdf; do
  n=$((n + 1))
  split=calib; [ "$n" -gt 60 ] && split=eval
  printf 'acm%03d\t%s\t%s\t%s\n' "$n" "$split" "$(sha256sum "$pdf" | cut -d' ' -f1)" "$pdf" >> "$out/docs.tsv"
done < <(find "$sample" -name '*.pdf' | sort)
find "$sample" -name '*.pdf' -printf '%f\n' | sort > "$out/.sample-names"
n=0
while IFS= read -r pdf; do
  grep -qxF "$(basename "$pdf")" "$out/.sample-names" && continue
  n=$((n + 1)); [ "$n" -gt "$extra" ] && break
  printf 'ext%03d\tcalib\t%s\t%s\n' "$n" "$(sha256sum "$pdf" | cut -d' ' -f1)" "$pdf" >> "$out/docs.tsv"
done < <(find "$more" -name '*.pdf' | sort)
rm -f "$out/.sample-names"
tr '\t' '\n' < "$out/docs.tsv" | xargs -d '\n' -n 4 -P "$(nproc)" \
  sh -c 'pdftoppm -r 200 -png "$4" "$0/$2/$1"' "$out"
echo "rendered $(wc -l < "$out/docs.tsv") papers: $(ls "$out/calib" | wc -l) calib pages, $(ls "$out/eval" | wc -l) eval pages"
