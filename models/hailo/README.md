# Hailo-10H HEFs for the RapidOCR models

Scripts that turn gRParse's OCR models (models/MANIFEST: `ch_PP-OCRv3_det_infer.onnx`,
`ch_PP-OCRv3_rec_infer.onnx`, `ch_ppocr_mobile_v2.0_cls_infer.onnx`) into Hailo-10H
HEFs with the Hailo Dataflow Compiler (DFC) 5.4.0. Nothing here is wired into the
server yet; the HEFs are build outputs, not checked in.

| Variant | Input (uint8 NHWC, BGR) | Source model |
|---|---|---|
| `det_960` | 960x960x3 | det |
| `det_640` | 640x640x3 | det |
| `rec_48x320` | 48x320x3 | rec |
| `rec_48x640` | 48x640x3 | rec |
| `cls_48x192` | 48x192x3 | cls |

Normalization runs on the chip (model script), so the runtime feeds the same uint8
pixels it would hand to RapidOcrOnnx before its mean/std step.

## Steps

The DFC needs Python 3.10-3.12 and system graphviz (`pygraphviz` builds against
`libcgraph`), so the steps run in a container with the wheel installed
(`FROM python:3.12`, `apt-get install graphviz graphviz-dev build-essential pkg-config
libgl1`, `pip install hailo_dataflow_compiler-5.4.0-py3-none-linux_x86_64.whl`). Run as
your own uid; the `hailo` CLI wants to write `/usr/local/bin/hailo_autocompletion`
on first use, so these scripts call the Python API (`ClientRunner`) instead.

```bash
scripts/fetch-models.sh --dir WORK/models                         # by sha256
python fix_shapes.py WORK/models WORK/fixed                       # pin input shapes (onnxsim)
python parse.py WORK/fixed/det_960.onnx WORK/har/det_960.har      # per variant
pdftoppm -r 200 -png tests/golden/corpus/X.pdf WORK/pages/X       # per corpus PDF, plus fixture PNGs
python3 fetch_lines.py WORK/pages WORK/grparse_lines              # gRParse CPU OCR line boxes (host, grpcurl)
python prep_calib.py WORK/pages WORK/models WORK/calib WORK/grparse_lines
python build_hef.py det_960 WORK/har WORK/calib WORK/out          # optimize + compile, per variant
python eval_emulator.py det_960 WORK/out WORK/calib WORK/fixed WORK/models
```

## What each model needed

- **det**: the RSE-FPN SE blocks end in global average pools over the whole feature
  map. At 240x240 and 120x120 the DFC refuses them ("Shift delta in
  det_960/avgpool1/avgpool_op is larger than 2 (2.64), cannot quantize").
  `build_hef.py` adds `global_avgpool_reduction` for those layers, splitting each into
  a pool with kernel `spatial/k` followed by a k x k global pool.
- **rec**: the SVTR graph already has layers named `normalization1..5` (its
  LayerNorms), so the input normalization is named `input_normalization1`.
- **cls**: nothing extra.

## Runtime constraints a server integration must handle

- det runs on a fixed square tile: pages need tiling or letterboxing, with boxes mapped
  back.
- rec runs at fixed widths. At 200 dpi only a small share of real lines fit 320 or 640
  after scaling to height 48, so long lines must be split into windows and the CTC
  outputs joined.
- cls pads with white and crops at 192, the same as RapidOcrOnnx `adjustTargetImg`.
- A HEF built by DFC 5.4.0 loads and runs on HailoRT 5.1.1 (checked on a Pi 5 +
  Hailo-10H).
