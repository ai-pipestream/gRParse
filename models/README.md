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

`GRPARSE_LAYOUT_MODEL` selects which detector runs: `heron` (the default) or
`picodet`. Only the selected model's file has to be present; when it is absent
the server disables layout under `GRPARSE_LAYOUT=auto` and fails startup under
`GRPARSE_LAYOUT=on`, naming the selection and the path it looked at.

- `layout_heron.onnx` — 17-label document layout detector, RT-DETR-v2
  architecture, published as ONNX at
  [docling-project/docling-layout-heron-onnx](https://huggingface.co/docling-project/docling-layout-heron-onnx).

  ```bash
  curl -L -o layout_heron.onnx \
    https://huggingface.co/docling-project/docling-layout-heron-onnx/resolve/main/model.onnx
  ```

  sha256: `59c81a3a2923042d85034ffc487f8f47e4854117e879aef89b2b9f728fb4922a`
  (171,220,471 bytes)

  License: Apache-2.0. Label map, index = model class id: `0=caption,
  1=footnote, 2=formula, 3=list_item, 4=page_footer, 5=page_header,
  6=picture, 7=section_header, 8=table, 9=text, 10=title, 11=document_index,
  12=code, 13=checkbox_selected, 14=checkbox_unselected, 15=form,
  16=key_value_region`.

  Graph (opset 18): inputs `images [b,3,640,640]` **uint8** and
  `orig_target_sizes [b,2]` int64 **width first**; outputs `labels [b,300]`
  int64, `boxes [b,300,4]` float already in the original page's pixel space
  (xyxy), and `scores [b,300]` float.

  Preprocess: bilinear resize to 640x640 (no aspect preservation), RGB channel
  order, raw bytes — the graph rescales and normalizes internally, so there is
  no `1/255` and no mean/std here. Postprocess: no anchors and no NMS. A
  detection is dropped below the engine score gate `0.3`, then below its own
  label gate (`0.5` for caption, footnote, formula, list_item, page_footer,
  page_header, picture, table, text; `0.45` for section_header, title, code,
  checkbox_selected, checkbox_unselected, form, key_value_region,
  document_index); `title` is then emitted as `section_header`. Boxes clip to
  the page and the result sorts by confidence, then top, then left. Constants
  mirror the reference pipeline's layout postprocessor. Its wrapper-containment
  and R-tree overlap resolution are not ported, so co-located duplicates (a
  caption and a text over the same box, say) can both survive.

  On the OpenVINO execution provider this session is pinned to single
  precision. The GPU plugin otherwise runs it at half precision, which costs
  real detections (on a measured page a section header at 0.815 and a title at
  0.511 disappear) and drifts boxes by up to 37 pixels; at single precision
  the GPU output matches CPU exactly. Random-input checks do not surface this,
  only real page renders do.

  This is the model the `layout-engine-test` heron golden was generated with.

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
  `0=text, 1=title, 2=list, 3=table, 4=picture`. The reference names class 4
  "figure"; gRParse speaks one region vocabulary across both detectors, and it
  is the document schema's "picture".

  Preprocess: resize to 608x800 (no aspect preservation), scale 1/255,
  normalize mean `[0.485, 0.456, 0.406]` / std `[0.229, 0.224, 0.225]` applied
  in the image's loaded channel order, NCHW. Postprocess: PicoDet DFL decode
  (strides 8/16/32/64, 8 bins/side), per-class confidence 0.5, class-wise hard
  NMS at IoU 0.5. Constants mirror RapidLayout's `pp` handler, which is the
  reference `layout-engine-test` goldens were generated with.

  When neither layout file is present the server runs without layout labels.
  The selected model loads into a single session on the configured execution
  provider (CUDA, OpenVINO, or CPU) and every inference worker shares it;
  unlike the OCR sessions it is not pooled per worker, because the weights are
  large and `Ort::Session::Run` is thread-safe.

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

## Figure classifier (optional; enables picture class annotations)

- `figure_classifier.onnx` — document figure classifier v2.5 (EfficientNet-B0,
  MIT license), published as ONNX at
  [docling-project/DocumentFigureClassifier-v2.5](https://huggingface.co/docling-project/DocumentFigureClassifier-v2.5).

  The published export needs one correction before it can be used, so it is
  downloaded under its own name and patched into place:

  ```bash
  curl -L -o figure_classifier_upstream.onnx \
    https://huggingface.co/docling-project/DocumentFigureClassifier-v2.5/resolve/main/model.onnx
  python ../scripts/patch_figure_classifier.py \
    figure_classifier_upstream.onnx figure_classifier.onnx
  ```

  The download has sha256
  `27ffc48c27ae4e12c99b6f6de0dd730005245e47b70dd0c1339e62cbac3ec4c0`
  (16,940,439 bytes); the corrected file this server loads has sha256
  `8f24abc627f0451aae9a4320af887fba4b6c0a82af0142f4c32c7b40cba27fbc`
  (16,938,738 bytes). Only `figure_classifier.onnx` has to stay on disk.

  **What the patch corrects.** The export's final pooling node is an
  `AveragePool` whose `kernel_shape` is the channel count (`[1280, 1280]`)
  instead of its input's spatial extent (`[7, 7]`). ONNX Runtime's CPU
  provider clamps that and produces the right answer; the OpenVINO execution
  provider rejects the graph and the session never builds. An `AveragePool`
  over the whole spatial extent is exactly `GlobalAveragePool`, so the script
  swaps the node for one and touches nothing else. It then scores both graphs
  on the same input and refuses the result unless the maximum absolute
  difference is 0, which it is.

  26 classes, index = model output index: `0=logo, 1=photograph, 2=icon,
  3=engineering_drawing, 4=line_chart, 5=bar_chart, 6=other, 7=table,
  8=flow_chart, 9=screenshot_from_computer, 10=signature,
  11=screenshot_from_manual, 12=geographical_map, 13=pie_chart,
  14=page_thumbnail, 15=stamp, 16=music, 17=calendar, 18=qr_code, 19=bar_code,
  20=full_page_image, 21=scatter_plot, 22=chemistry_structure,
  23=topographical_map, 24=crossword_puzzle, 25=box_plot`.

  **This file replaced a 16-class predecessor of the same name.** The class
  count is read from the graph at startup, so a stale file stops the server
  with both counts rather than failing on the first figure of the first
  document; re-download it when upgrading.

  Preprocess (opset 20, unchanged from the predecessor): RGB order (unlike the
  OCR-family models), resize 224x224, scale 1/255, normalize mean
  `[0.485, 0.456, 0.406]` / std `[0.47853944, 0.4732864, 0.47434163]`, NCHW.
  Output is raw logits; gRParse softmaxes and attaches every class sorted by
  confidence as a `classification` annotation on the PictureItem.

  Requires layout; runs only on picture crops. When absent, pictures keep
  their bounding boxes and simply carry no class annotation.
