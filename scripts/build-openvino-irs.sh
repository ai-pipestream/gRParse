#!/usr/bin/env bash
# Builds the openvino flavor's IR (models/MANIFEST group openvino) from the
# original model files, with the pinned converter toolchain.
#
#   scripts/build-openvino-irs.sh <models dir>
#
# Reads, all verified by scripts/fetch-models.sh before this runs:
#   <dir>/openvino-src/*.tar                 PaddleOCR inference tarballs
#   <dir>/openvino-src/figure_classifier/    HF checkpoint (config, weights)
#   <dir>/layout_heron.onnx, <dir>/slanet_plus.onnx
# and writes <dir>/openvino/*.xml|.bin|.vocab with the converters in
# models/openvino/.  The converters are deterministic for a fixed toolchain,
# so fetch-models.sh checks every output against its manifest sha256.
#
# Environment:
#   FETCH_MODELS_OPENVINO_PYTHON  command prefix with openvino, onnx, numpy,
#                                 torch, transformers and safetensors
#                                 importable; default is a uv run with the
#                                 versions the manifest's hashes were made by.
set -euo pipefail
dir=${1:?models dir}
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
converters=$repo_root/models/openvino
if [ -n "${FETCH_MODELS_OPENVINO_PYTHON:-}" ]; then
  python=$FETCH_MODELS_OPENVINO_PYTHON
else
  command -v uv >/dev/null 2>&1 || { echo "uv not found; set FETCH_MODELS_OPENVINO_PYTHON" >&2; exit 1; }
  python="uv run --no-project --python 3.12 --index https://download.pytorch.org/whl/cpu
    --index-strategy unsafe-best-match --with openvino==2025.4.1 --with onnx==1.23.2
    --with numpy==2.3.5 --with torch==2.8.0 --with transformers==4.56.1
    --with safetensors==0.8.0 python -I"
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for net in ch_PP-OCRv3_det_infer ch_PP-OCRv3_rec_infer ch_ppocr_mobile_v2.0_cls_infer; do
  tar -xf "$dir/openvino-src/$net.tar" -C "$work"
done
mkdir -p "$dir/openvino"
# shellcheck disable=SC2086  # the prefix is a command line on purpose
(
  cd "$work"
  $python "$converters/convert_paddle_ocr.py" "$work" "$dir/openvino"
  $python "$converters/convert_heron_docling.py" "$dir/layout_heron.onnx" "$dir/openvino/layout_heron.xml"
  $python "$converters/convert_figure_classifier.py" "$dir/openvino-src/figure_classifier" \
    "$dir/openvino/figure_classifier.xml"
  $python "$converters/convert_slanet.py" "$dir/slanet_plus.onnx" "$dir/openvino"
) >"$work/convert.log" 2>&1 || { tail -20 "$work/convert.log" >&2; exit 1; }
