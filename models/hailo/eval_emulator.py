"""Compare the quantized model (DFC emulator) against float on held-out inputs.

Usage: python eval_emulator.py VARIANT OUT_DIR CALIB_DIR FIXED_DIR MODELS_DIR

Three runs over the same uint8 eval tensors (prep_calib.py *_eval.npy):
  ort     the shape-pinned ONNX in ONNX Runtime with RapidOcrOnnx
          normalization done on the host (the server's float path)
  native  the parsed HAR in the DFC emulator, float (SDK_NATIVE)
  quant   the quantized HAR in the DFC emulator (SDK_QUANTIZED)
native vs ort checks the parse and the on-chip normalization; quant vs ort is
the quantization cost. Metrics: det binary-map IoU at 0.3 and box matching at
IoU 0.5; rec CTC text CER; cls argmax agreement.
"""
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
from hailo_sdk_client import ClientRunner, InferenceContext

sys.path.insert(0, str(Path(__file__).parent))
from prep_calib import DET_MEAN, DET_STD, cer, ctc_decode, db_boxes  # noqa: E402


def host_norm(variant, x):
    x = x.astype(np.float32)
    x = (x - DET_MEAN) / DET_STD if variant.startswith("det") else (x - 127.5) / 127.5
    return x.transpose(0, 3, 1, 2)


def emulate(har, x, context):
    runner = ClientRunner(har=str(har))
    with runner.infer_context(context) as ctx:
        return np.asarray(runner.infer(ctx, x))


def box_iou(a, b):
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def det_metrics(ref, got):
    rows = []
    for r, g in zip(ref, got):
        r, g = r.squeeze(), g.squeeze()
        rb, gb = r > 0.3, g > 0.3
        union = np.logical_or(rb, gb).sum()
        map_iou = float(np.logical_and(rb, gb).sum() / union) if union else 1.0
        rboxes, gboxes = db_boxes(r), db_boxes(g)
        used, ious = set(), []
        for a in rboxes:
            best, bj = 0.0, -1
            for j, b in enumerate(gboxes):
                if j not in used and (v := box_iou(a, b)) > best:
                    best, bj = v, j
            if best >= 0.5:
                used.add(bj)
                ious.append(best)
        rows.append({"map_iou": round(map_iou, 4), "ref_boxes": len(rboxes), "quant_boxes": len(gboxes),
                     "matched": len(ious), "mean_matched_iou": round(float(np.mean(ious)), 4) if ious else None,
                     "max_abs_prob_diff": round(float(np.abs(r - g).max()), 4)})
    return rows


def main() -> None:
    variant, out, calib, fixed, models = (sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]),
                                          Path(sys.argv[4]), Path(sys.argv[5]))
    x = np.load(calib / f"{variant}_eval.npy")
    sess = ort.InferenceSession(str(fixed / f"{variant}.onnx"), providers=["CPUExecutionProvider"])
    ref = np.concatenate([sess.run(None, {"x": host_norm(variant, x[i:i + 1])})[0] for i in range(len(x))])
    # SDK_NATIVE is the parsed graph before the model script, so it takes the
    # host-normalized float input; SDK_QUANTIZED includes the on-chip
    # normalization layer and takes the raw uint8.
    native = emulate(out / f"{variant}.q.har", host_norm(variant, x).transpose(0, 2, 3, 1), InferenceContext.SDK_NATIVE)
    quant = emulate(out / f"{variant}.q.har", x, InferenceContext.SDK_QUANTIZED)
    report = {"variant": variant, "samples": int(len(x)), "ort_shape": list(ref.shape),
              "emulator_shape": list(quant.shape)}

    if variant.startswith("det"):
        ref_maps = ref.reshape(len(x), ref.shape[-2], ref.shape[-1])
        nat_maps = native.reshape(ref_maps.shape)
        q_maps = quant.reshape(ref_maps.shape)
        report["native_vs_ort"] = det_metrics(ref_maps, nat_maps)
        report["quant_vs_ort"] = det_metrics(ref_maps, q_maps)
    elif variant.startswith("rec"):
        keys = (models / "ppocr_keys_v1.txt").read_text(encoding="utf-8").splitlines() + [" "]
        classes = ref.shape[-1]
        r_txt = [ctc_decode(p, keys) for p in ref.reshape(len(x), -1, classes)]
        n_txt = [ctc_decode(p, keys) for p in native.reshape(len(x), -1, classes)]
        q_txt = [ctc_decode(p, keys) for p in quant.reshape(len(x), -1, classes)]
        n_cer = [cer(a, b) for a, b in zip(n_txt, r_txt)]
        q_cer = [cer(a, b) for a, b in zip(q_txt, r_txt)]
        chars = sum(len(t) for t in r_txt)
        report["native_vs_ort_mean_cer"] = round(float(np.mean(n_cer)), 4)
        report["quant_vs_ort_mean_cer"] = round(float(np.mean(q_cer)), 4)
        report["quant_vs_ort_char_weighted_cer"] = round(
            sum(c * len(t) for c, t in zip(q_cer, r_txt)) / max(1, chars), 4)
        report["quant_exact_line_match"] = f"{sum(a == b for a, b in zip(q_txt, r_txt))}/{len(x)}"
        report["examples"] = [{"ort": a, "quant": b, "cer": round(c, 3)}
                              for a, b, c in sorted(zip(r_txt, q_txt, q_cer), key=lambda t: -t[2])[:8]]
    else:
        r_arg = ref.reshape(len(x), -1).argmax(-1)
        n_arg = native.reshape(len(x), -1).argmax(-1)
        q_arg = quant.reshape(len(x), -1).argmax(-1)
        half = len(x) // 2  # prep_calib stores upright crops first, then the same crops turned 180
        report["ort_says_180_on_rotated_half"] = f"{int((r_arg[half:] == 1).sum())}/{len(x) - half}"
        report["ort_says_0_on_upright_half"] = f"{int((r_arg[:half] == 0).sum())}/{half}"
        report["native_agrees_with_ort"] = f"{int((n_arg == r_arg).sum())}/{len(x)}"
        report["quant_agrees_with_ort"] = f"{int((q_arg == r_arg).sum())}/{len(x)}"
        report["quant_max_abs_prob_diff"] = round(float(np.abs(
            quant.reshape(len(x), -1) - ref.reshape(len(x), -1)).max()), 4)

    path = out / f"eval-{variant}.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False)[:3000])


if __name__ == "__main__":
    main()
