# Model files

Place these files in this directory before starting the server.

## OCR (required)

RapidOcrOnnx-compatible files:

- `ch_PP-OCRv3_det_infer.onnx`
- `ch_ppocr_mobile_v2.0_cls_infer.onnx`
- `ch_PP-OCRv3_rec_infer.onnx`
- `ppocr_keys_v1.txt`

The model set and dictionary naming are documented by [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx#模型下载). The dictionary is included in that repository; download the three ONNX model files from the corresponding RapidOCR model release and keep them matched to this dictionary.

## Layout (optional; enables region labels)

- `layout_publaynet.onnx` — PicoDet layout detector from PaddleDetection's
  PP-StructureV2, trained on PubLayNet; ONNX export published by
  [RapidLayout](https://github.com/RapidAI/RapidLayout).

  ```bash
  curl -L -o layout_publaynet.onnx \
    https://github.com/RapidAI/RapidLayout/releases/download/v0.0.0/layout_publaynet.onnx
  ```

  sha256: `958aa6dcef1cc1a542d0a513b5976a3d5edbcc37d76460ec1e9f126358e4d100`

  License: Apache-2.0 (PaddleDetection model, RapidLayout packaging; PubLayNet
  dataset is CDLA-Permissive). Label map, index = model class id:
  `0=text, 1=title, 2=list, 3=table, 4=figure`.

  Preprocess: resize to 608x800 (no aspect preservation), scale 1/255,
  normalize mean `[0.485, 0.456, 0.406]` / std `[0.229, 0.224, 0.225]` applied
  in the image's loaded channel order, NCHW. Postprocess: PicoDet DFL decode
  (strides 8/16/32/64, 8 bins/side), per-class confidence 0.5, class-wise hard
  NMS at IoU 0.5. Constants mirror RapidLayout's `pp` handler, which is the
  reference `layout-engine-test` goldens were generated with.

  When this file is absent the server runs without layout labels; when present
  it is pooled per inference worker on the configured execution provider
  (CUDA, OpenVINO, or CPU), like the OCR sessions.

## Table structure (optional; enables cell spans and header rows)

- `slanet_plus.onnx` — SLANet-plus table structure recognition from
  PaddleOCR's PP-StructureV3 line, ONNX export published by
  [RapidTable](https://github.com/RapidAI/RapidTable) on ModelScope.

  ```bash
  curl -L -o slanet_plus.onnx \
    https://www.modelscope.cn/models/RapidAI/RapidTable/resolve/v2.0.0/slanet-plus.onnx
  ```

  sha256: `d57a942af6a2f57d6a4a0372573c696a2379bf5857c45e2ac69993f3b334514b`

  License: Apache-2.0 (PaddleOCR model, RapidTable packaging). The token
  vocabulary (HTML structure tags plus colspan/rowspan attributes up to 20)
  is embedded in the model metadata under `character`.

  Preprocess: longest side to 488 preserving aspect (truncating), scale
  1/255, normalize mean `[0.485, 0.456, 0.406]` / std `[0.229, 0.224, 0.225]`
  in the image's loaded channel order, zero-pad to 488x488 top-left, NCHW.
  The export runs its decode loop in-graph and emits per-step token
  probabilities `[1, S, 50]` and cell corner boxes `[1, S, 8]`; postprocess is
  argmax to `eos`, cell boxes on `<td` tokens rescaled by the original crop
  size and the pad ratio. Constants mirror RapidTable's `PPTableStructurer`,
  the reference `table-structure-engine-test` goldens were generated with.

  Requires layout (that is what finds table regions); runs only on table
  crops, never full pages. When absent, tables fall back to the geometry
  word-to-cell grid.
