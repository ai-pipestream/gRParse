# OpenVINO IR for the openvino flavor

The openvino flavor runs every model on native OpenVINO (`src/inference_model.cpp`), from IR, on the GPU only. It never loads ONNX at runtime. The server reads `<GRPARSE_MODELS_DIR>/openvino/<stem>.xml` (and its `.bin`), where `<stem>` is the ONNX file name the other flavors use. A missing IR, or a model the GPU will not compile, fails boot and names the model.

Each IR is converted from the model's original framework, with FP16 weights. Shapes stay dynamic in the IR; the server reshapes them to its static buckets when it loads them. These files are not published yet (#334 model zoo).

| IR stem | Source | Converter |
|---|---|---|
| `ch_PP-OCRv3_det_infer` | PaddleOCR `ch_PP-OCRv3_det_infer.tar` (Paddle inference model) | `convert_paddle_ocr.py` |
| `ch_PP-OCRv3_rec_infer` | PaddleOCR `ch_PP-OCRv3_rec_infer.tar` | `convert_paddle_ocr.py` |
| `ch_ppocr_mobile_v2.0_cls_infer` | PaddleOCR `ch_ppocr_mobile_v2.0_cls_infer.tar` | `convert_paddle_ocr.py` |
| `layout_heron` | docling's ONNX export `docling-project/docling-layout-heron-onnx` (`layout_heron.onnx` in MANIFEST), dynamic shapes, FP32 weights | `convert_heron_docling.py` |
| `figure_classifier` | `docling-project/DocumentFigureClassifier-v2.5`, `model.safetensors` sha256 `6bf1e44d…c3cd` (PyTorch, EfficientNet) | `convert_figure_classifier.py` |
| `slanet_plus_encoder`, `slanet_plus_step`, `slanet_plus.vocab` | `slanet_plus.onnx` (paddle2onnx of PaddleX SLANet_plus; the Paddle 3 PIR model is unreadable by OpenVINO 2025.4): encoder cut before the decode loop, decoder step rebuilt in PyTorch over the graph's weights | `convert_slanet.py` |

Tool versions: openvino 2025.4.1 (the runtime the image ships, from Intel's release archive), torch 2.8.0+cpu, transformers 4.56.1.

`convert_heron.py` (heron from its HF PyTorch checkpoint, `model.safetensors` sha256 `00333a43…9e0c`) is kept for reference: its IR is exact on OpenVINO's CPU device but miscomputes at FP32 on the B70.

## Accuracy

Checked against the ONNX models on ONNX Runtime CPU (2026-11-16):
- **PP-OCRv3 det, rec and cls:** max abs output difference 1e-4 to 4e-3; rec and cls argmax agree 100%.
- **heron:** the IR on OpenVINO CPU f32 matches the ONNX export on every region. On the B70 at FP16, 98–100% of regions match at IoU > 0.9.
- **figure classifier:** top-1 agrees 100%.

## Precision on the B70

On the Arc Pro B70 (xe, OpenVINO 2025.4), FP32 inference is broken for two of these models:
- **heron:** FP32 returns 5–10 regions instead of 56–118, even with FP32 weights.
- **figure classifier:** FP32 faults the device (CAT error, engine reset).

- **OCR detector:** FP32 faults the device (CAT error) at its first inference.

So the OCR nets and the figure classifier compile at FP16 (`GRPARSE_OPENVINO_PRECISION=f16`, the default). Two models ask for FP32: heron, as docling's dynamic export (exact at FP32 on the B70), and the slanet encoder and step (FP16 drifts the 501-step decode).
