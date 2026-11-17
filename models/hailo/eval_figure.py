"""Compare quantized figure_224 (DFC emulator) against float on held-out crops.

Usage: python eval_figure.py OUT_DIR CALIB_DIR MODELS_DIR

Three runs over the uint8 BGR crops of prep_calib_figure.py, as in
eval_emulator.py: ONNX Runtime on figure_classifier.onnx with the server's
host preprocessing (src/figure_classifier.cpp: BGR->RGB, /255, mean/std), the
DFC float emulator (SDK_NATIVE) and the quantized emulator (SDK_QUANTIZED).
Reports, per kind of crop and overall, how often the top class agrees with
float and how far the softmax probability of float's top class moves.
"""
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
from hailo_sdk_client import ClientRunner, InferenceContext

MEAN = np.array([0.485, 0.456, 0.406], np.float32)  # RGB
STD = np.array([0.47853944, 0.4732864, 0.47434163], np.float32)
LABELS = ("logo", "photograph", "icon", "engineering_drawing", "line_chart", "bar_chart", "other", "table",
          "flow_chart", "screenshot_from_computer", "signature", "screenshot_from_manual", "geographical_map",
          "pie_chart", "page_thumbnail", "stamp", "music", "calendar", "qr_code", "bar_code", "full_page_image",
          "scatter_plot", "chemistry_structure", "topographical_map", "crossword_puzzle", "box_plot")


def host_norm(x):
    """uint8 NHWC BGR -> normalized float NHWC RGB."""
    return (x[..., ::-1].astype(np.float32) / np.float32(255.0) - MEAN) / STD


def softmax(logits):
    weight = np.exp(logits - logits.max(axis=1, keepdims=True))
    return weight / weight.sum(axis=1, keepdims=True)


def compare(ref, got, rows):
    """ref and got are softmax rows; rows pick the crops to score."""
    ref, got = ref[rows], got[rows]
    top = ref.argmax(1)
    moved = np.abs(got[np.arange(len(top)), top] - ref[np.arange(len(top)), top])
    agree = got.argmax(1) == top
    return {"crops": int(len(top)), "top1_agree": int(agree.sum()), "top1_agreement": round(float(agree.mean()), 4),
            "max_abs_top_prob_diff": round(float(moved.max()), 4),
            "mean_abs_top_prob_diff": round(float(moved.mean()), 4),
            "flips": [{"float": LABELS[a], "float_prob": round(float(ref[i, a]), 3), "quant": LABELS[b],
                       "quant_prob": round(float(got[i, b]), 3)}
                      for i, (a, b) in enumerate(zip(top, got.argmax(1))) if a != b]}


def main() -> None:
    out, calib, models = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    variant = "figure_224"
    x = np.load(calib / f"{variant}_eval.npy")
    kinds = np.array([row["kind"] for row in json.loads((calib / f"{variant}_manifest.json").read_text())["eval"]])
    sess = ort.InferenceSession(str(models / "figure_classifier.onnx"), providers=["CPUExecutionProvider"])
    feed = host_norm(x)
    ref = softmax(np.concatenate([sess.run(None, {sess.get_inputs()[0].name: feed[i:i + 1].transpose(0, 3, 1, 2)})[0]
                                  for i in range(len(x))]))
    runner = ClientRunner(har=str(out / f"{variant}.q.har"))
    # SDK_NATIVE is the parsed graph before the model script: it takes the
    # host-normalized RGB. SDK_QUANTIZED includes the on-chip swap and
    # normalization and takes the raw BGR uint8 (see eval_emulator.py).
    runs = {}
    for key, context, data in (("native_vs_ort", InferenceContext.SDK_NATIVE, feed),
                               ("quant_vs_ort", InferenceContext.SDK_QUANTIZED, x)):
        with runner.infer_context(context) as ctx:
            runs[key] = softmax(np.asarray(runner.infer(ctx, data)).reshape(len(x), -1))
    top = ref.argmax(1)
    report = {"variant": variant, "crops": int(len(x)),
              "float_top1_by_label": {LABELS[i]: int(n) for i, n in enumerate(np.bincount(top, minlength=26)) if n}}
    for key, got in runs.items():
        report[key] = {"all": compare(ref, got, np.arange(len(x)))}
        for kind in dict.fromkeys(kinds):
            report[key][kind] = compare(ref, got, np.nonzero(kinds == kind)[0])
    (out / f"eval-{variant}.json").write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
