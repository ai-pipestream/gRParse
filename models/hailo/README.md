# Hailo-10H HEFs for the image models

Scripts that turn gRParse's models (models/MANIFEST) into Hailo-10H HEFs with
the Hailo Dataflow Compiler (DFC) 5.4.0, and that record what the compiler
answers for the models it cannot take. The server side is `src/hailo/`
(`GRPARSE_ORT_EP=hailo`, README "Hailo-10H"). The HEFs are build outputs: they
are not checked in and not published yet.

## What the compiler takes

`probe_parse.py` pins a model's inputs and asks the DFC parser for
`hw_arch=hailo10h`; its JSON is the evidence for the placement table in
`src/hailo/hailo_placement.cpp`.

| Model (models/MANIFEST) | Parser verdict | Placement |
|---|---|---|
| `ch_PP-OCRv3_det_infer.onnx` | parses (after the pooling split below) | device, `det_640.hef` |
| `ch_ppocr_mobile_v2.0_cls_infer.onnx` | parses | device, `cls_48x192.hef` |
| `ch_PP-OCRv3_rec_infer.onnx` | parses | device, `rec_48x320.hef` |
| `layout_publaynet.onnx` | parses, 144 layers | device, `layout_800x608.hef` |
| `figure_classifier.onnx` | parses, 142 layers | device, `figure_224.hef` |
| `layout_heron.onnx` | refused: `GridSample operation is unsupported` (18 nodes), `GatherElements operation is unsupported` (3), `TopK operation is unsupported`, `Reduce max is only supported on the features axis`, `Slice on axis 4 is not supported` | CPU pin |
| `slanet_plus.onnx` | the parser fails with `IndexError: list index out of range` in `onnx_graph.py is_empty_slice`, on the decoder's `Loop` | CPU pin |

For `layout_heron.onnx` the parser recommends cutting the graph at 22 end
nodes. That cut parses (262 layers, 445 of 1024 ONNX nodes: the backbone and
the encoder) and leaves 579 nodes to the host, the whole query decoder
(91 MatMul, 18 GridSample, 18 LayerNormalization, TopK, GatherElements). A
host half would need a second runtime for most of the model, so the model
stays whole on CPU.

## HEFs

Every HEF takes 8-bit NHWC pixels in BGR order; normalization (and the
figure classifier's BGR to RGB swap) is in the model script, so the runtime
hands the chip the pixels it would hand the float model before its own
mean/std step.

| HEF | Input | Recipe | Contexts | Quantized against float, held-out real data |
|---|---|---|---|---|
| `det_640` | 640x640x3 | every layer `a16_w16`, optimization level 0 | 7 | 3 pages: all float boxes found (7/7, 36/36, 98/99), mean box IoU 0.93 to 0.96 |
| `cls_48x192` | 48x192x3 | DFC defaults (8 bit), level 0 | 4 | 240 of 242 line crops agree |
| `rec_48x320` | 48x320x3 | convolutional backbone `a16_w16`, neck and head at the DFC defaults (8 bit), level 2 with fine-tuning | 11 | 121 line windows: character error rate 1.3% against float, 96 lines identical; about one doubled letter in five is read once ("recuring", "paterns") |
| `layout_800x608` | 800x608x3 | every layer `a16_w16`, level 2, 1024 calibration pages | 9 | 234 pages, 3368 float regions: precision 0.965, recall 0.947 (0.995 for regions scoring 0.7 and up), mean matched IoU 0.971 |
| `figure_224` | 224x224x3 | every layer `a16_w16`, level 1, 1024 calibration crops | 9 | 363 crops: top class agrees on 97.5%; every flip has a float top probability of 0.51 or less |

`det_960` and `rec_48x640` also build; `det_960` does not reach the same
agreement (map IoU 0.70 to 0.84) and the server tiles at 640 instead.

What each model needed:

- **det**: the neck's SE blocks end in global average pools over the whole
  feature map. The DFC refuses them ("Shift delta in det_960/avgpool1/avgpool_op
  is larger than 2 (2.64), cannot quantize"); `build_hef.py` adds
  `global_avgpool_reduction` for those layers. With 8-bit layers the map
  over-segments; with every layer at 16 bits and level 0 it matches. Level 2
  breaks the same setup (about 3000 boxes a page).
- **rec**: the graph already has layers named `normalization1..5`, so the
  input normalization is `input_normalization1`. With 8-bit activations in
  the convolutional backbone every line decodes empty; the backbone is
  `a16_w16`. The attention neck stays at the DFC defaults: widening it leaves
  the DFC's own `precision_change` layers in a mode hailo10h rejects.
  What is left is the quantized neck: where the float model puts a blank
  between two equal letters, the quantized one sometimes keeps the letter (in
  4 of 25 such places on 400 calibration windows, twice with the blank not
  even a close second), so a decoding rule cannot bring those back. Ending
  the graph before the softmax with a 16-bit output layer was tried and is
  worse (error rate 8.6%, capitals and short words lost).
- **layout**, **figure**: 8-bit builds are not usable (layout at level 0
  finds 2 regions where float finds 3368; the figure classifier agrees on 85
  to 90%). Both need every layer at 16 bits; `build_hef.py` has the numbers
  per recipe.

## Steps

The DFC needs Python 3.10 to 3.12 and system graphviz, so the steps run in a
container with the wheel installed (`FROM python:3.12`, `apt-get install
graphviz graphviz-dev build-essential pkg-config libgl1`, `pip install
hailo_dataflow_compiler-5.4.0-py3-none-linux_x86_64.whl`), as your own uid;
the scripts call the Python API (`ClientRunner`), since the `hailo` CLI wants
to write under `/usr/local/bin` on first use. The prep scripts for layout and
figure also need `opencv-python-headless` (the resize must be `cv::resize`).

```bash
scripts/fetch-models.sh --dir WORK/models --layout all            # by sha256
python fix_shapes.py WORK/models WORK/fixed                       # pin input shapes (onnxsim)
python parse.py WORK/fixed/det_640.onnx WORK/har/det_640.har      # per variant
pdftoppm -r 200 -png tests/golden/corpus/X.pdf WORK/pages/X       # per corpus PDF, plus fixture PNGs
python3 fetch_lines.py WORK/pages WORK/grparse_lines              # gRParse CPU OCR line boxes (host, grpcurl)
python prep_calib.py WORK/pages WORK/models WORK/calib WORK/grparse_lines
sh render_pages.sh PAPERS_DIR WORK/paper-pages                    # layout and figure: more real pages
python prep_calib_layout.py WORK/paper-pages WORK/pages WORK/calib
python prep_calib_figure.py ...                                   # see its docstring
python build_hef.py det_640 WORK/har WORK/calib WORK/out          # optimize + compile, per variant
python eval_emulator.py det_640 WORK/out WORK/calib WORK/fixed WORK/models   # OCR variants
python eval_layout.py ... ; python eval_figure.py ...             # see their docstrings
python probe_parse.py WORK/models/layout_heron.onnx layout_heron WORK/probe images=1x3x640x640 orig_target_sizes=1x2
```

`det_640` is built with `HAILO_OPT_LEVEL=0`; the other variants take the
level `build_hef.py` names for them.

## Build record

DFC 5.4.0 (`hailo_dataflow_compiler-5.4.0-py3-none-linux_x86_64.whl`, sha256
`d4cdb5419a430e0ca5dc89e73acdd27e6263945268a1a68b72903253e59d0bfc`),
`hw_arch=hailo10h`. A HEF built by DFC 5.4.0 loads and runs on HailoRT 5.1.1.

| HEF | Bytes | sha256 |
|---|---|---|
| `det_640.hef` | 4059136 | `38881a0a58cc58cd7138900d2d6339a99c51329ec05f3713e036fcdf43b8930d` |
| `cls_48x192.hef` | 1224704 | `33d79bc7c3b9821cee5e092cbd22a631eec224b0807a9b9c3a2bc9dd6f7ea261` |
| `rec_48x320.hef` | 13156352 | `b93fa5c5fb2f7ed81b6392beb0ee16c7640eac2344362d9f992f33aa6f213f26` |
| `layout_800x608.hef` | 7716864 | `f4a613cf7e963da054f3c55eb526a83eecc87cb19bc3bc2eb674faedb1e19a8d` |
| `figure_224.hef` | 9961472 | `b7c3338084c743026cb7594996cbe6d6c0120cc53d8919596ad2d29f60b03e9a` |

Calibration data:

- OCR: pages of the repository's own test corpus rendered at 200 dpi (1024
  detector tiles cut at random page scales; 1024 line windows for the
  recognizer and the direction net, cut with gRParse's own CPU OCR line
  boxes). Redistributable, and `prep_calib.py` reproduces the arrays byte for
  byte.
- Layout and figure: the same corpus pages plus pages and picture crops of
  published conference papers, which are not ours to redistribute. A
  calibration set that can be published with the HEFs is still to be chosen.

## Licenses

- HailoRT, the runtime the image links, is MIT (github.com/hailo-ai/hailort,
  `hailort/LICENSE`; its third-party list is `hailort/LICENSE-3RD-PARTY.md`).
  The image builds it from that source. The Debian package of the same
  library on a Raspberry Pi (`h10-hailort`) ships a copyright file that reads
  "License: Proprietary", so the image takes nothing from it.
- The Dataflow Compiler is Hailo's own, under Hailo's license, x86_64 only,
  and is used only to build HEFs. The terms that apply to redistributing its
  outputs are an open question for publishing.
- The chip's firmware has its own license and stays on the host.
- The source models keep their licenses (models/MANIFEST): Apache-2.0 for the
  OCR, layout and table models, MIT for the figure classifier.
