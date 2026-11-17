"""Compare quantized layout_800x608 (DFC emulator) against float on held-out pages.

Usage: python eval_layout.py OUT_DIR CALIB_DIR MODELS_DIR

Three runs over the uint8 pages of prep_calib_layout.py, as in eval_emulator.py:
ONNX Runtime on layout_publaynet.onnx with the server's host normalization,
the DFC float emulator (SDK_NATIVE) and the quantized emulator
(SDK_QUANTIZED). Each run is decoded by decode(), a numpy port of
AnchorFreeStrategy::detect in src/layout_engine.cpp (same threshold, DFL
expectation, clipping, class-wise NMS and rescale), and regions are matched to
the float ones at IoU >= 0.5 with an equal label.
"""
import json
import sys
from pathlib import Path

import numpy as np

HEIGHT, WIDTH = 800, 608
STRIDES = (8, 16, 32, 64)
REG_BINS = 8
CONFIDENCE = np.float32(0.5)
NMS_IOU = np.float32(0.5)
NMS_CANDIDATES, KEEP_TOP_K = 200, 100
LABELS = ("text", "title", "list", "table", "picture")
SURE = 0.7  # report recall apart for float regions well clear of the 0.5 threshold
MEAN = np.array([0.485, 0.456, 0.406], np.float32)  # applied to BGR as stored
STD = np.array([0.229, 0.224, 0.225], np.float32)


def host_norm(x):
    """uint8 NHWC BGR -> the float NCHW tensor the server feeds."""
    return ((x.astype(np.float32) / np.float32(255.0) - MEAN) / STD).transpose(0, 3, 1, 2)


def iou(a, b, eps=0.0):
    w = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    h = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = w * h
    area = max(0.0, a[2] - a[0]) * max(0.0, a[3] - a[1]) + max(0.0, b[2] - b[0]) * max(0.0, b[3] - b[1])
    return inter / (area - inter + eps) if area - inter + eps > 0 else 0.0


def by_level(outputs):
    """Order one page's 8 head outputs as (scores, distributions) per stride."""
    cells = [((HEIGHT + s - 1) // s) * ((WIDTH + s - 1) // s) for s in STRIDES]
    flat = [np.asarray(o, np.float32).reshape(-1) for o in outputs]
    pick = lambda n: next(f for f in flat if f.size == n)  # noqa: E731
    return [(pick(c * len(LABELS)).reshape(c, len(LABELS)), pick(c * 4 * REG_BINS).reshape(c, 4, REG_BINS))
            for c in cells]


def decode(outputs, page_width, page_height):
    """Regions (label, confidence, left, top, right, bottom) in page pixels."""
    candidates = []  # (score, label, box in model input space)
    for stride, (scores, bins) in zip(STRIDES, by_level(outputs)):
        wide = (WIDTH + stride - 1) // stride
        cells = np.nonzero(scores.max(axis=1) > CONFIDENCE)[0]
        weight = np.exp(bins[cells] - bins[cells].max(axis=-1, keepdims=True))
        dist = (weight * np.arange(REG_BINS, dtype=np.float32)).sum(-1) / weight.sum(-1) * np.float32(stride)
        cx = ((cells % wide).astype(np.float32) + np.float32(0.5)) * np.float32(stride)
        cy = ((cells // wide).astype(np.float32) + np.float32(0.5)) * np.float32(stride)
        boxes = np.stack([np.clip(cx - dist[:, 0], 0, WIDTH), np.clip(cy - dist[:, 1], 0, HEIGHT),
                          np.clip(cx + dist[:, 2], 0, WIDTH), np.clip(cy + dist[:, 3], 0, HEIGHT)], axis=1)
        for row, cell in enumerate(cells):
            for label in np.nonzero(scores[cell] > CONFIDENCE)[0]:
                candidates.append((scores[cell, label], int(label), boxes[row]))
    picked = []
    for label in range(len(LABELS)):
        ranked = sorted((c for c in candidates if c[1] == label), key=lambda c: -c[0])[:NMS_CANDIDATES]
        kept = []
        for cand in ranked:
            if len(kept) == KEEP_TOP_K:
                break
            if all(np.float32(iou(cand[2], k[2], 1e-5)) <= NMS_IOU for k in kept):
                kept.append(cand)
        picked += kept
    sx, sy = np.float32(WIDTH) / np.float32(page_width), np.float32(HEIGHT) / np.float32(page_height)
    lround = lambda v: int(np.floor(v + np.float32(0.5)))  # noqa: E731
    regions = [(LABELS[label], float(score), lround(b[0] / sx), lround(b[1] / sy), lround(b[2] / sx),
                lround(b[3] / sy)) for score, label, b in picked]
    return sorted(regions, key=lambda r: (-r[1], r[3], r[2]))


def match(ref, got):
    """Greedy one-to-one matching of got to ref (equal label, IoU >= 0.5): IoU or None per ref region."""
    used, ious = set(), []
    for r in ref:
        best, pick = 0.0, -1
        for j, g in enumerate(got):
            if j not in used and g[0] == r[0] and (v := iou(r[2:], g[2:])) > best:
                best, pick = v, j
        if best >= 0.5:
            used.add(pick)
        ious.append(best if best >= 0.5 else None)
    return ious


def compare(ref_pages, got_pages, names):
    rows, all_ious, sure = [], [], []  # sure: was a float region scoring >= SURE matched?
    for name, ref, got in zip(names, ref_pages, got_pages):
        found = match(ref, got)
        sure += [v is not None for r, v in zip(ref, found) if r[1] >= SURE]
        ious = [v for v in found if v is not None]
        all_ious += ious
        rows.append({"page": name, "float_regions": len(ref), "regions": len(got), "matched": len(ious),
                     "mean_matched_iou": round(float(np.mean(ious)), 4) if ious else None})
    n_ref, n_got, hit = (sum(r[k] for r in rows) for k in ("float_regions", "regions", "matched"))
    summary = {"pages": len(rows), "float_regions": n_ref, "regions": n_got, "matched": hit,
               "precision": round(hit / n_got, 4) if n_got else None,
               "recall": round(hit / n_ref, 4) if n_ref else None,
               f"float_regions_scoring_{SURE}_up": len(sure),
               f"recall_scoring_{SURE}_up": round(sum(sure) / len(sure), 4) if sure else None,
               "mean_matched_iou": round(float(np.mean(all_ious)), 4) if all_ious else None,
               "pages_with_identical_count": sum(r["float_regions"] == r["regions"] for r in rows),
               "pages_fully_matched": sum(r["float_regions"] == r["regions"] == r["matched"] for r in rows)}
    return summary, rows


def main() -> None:
    import onnxruntime as ort
    from hailo_sdk_client import ClientRunner, InferenceContext

    out, calib, models = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    variant = "layout_800x608"
    x = np.load(calib / f"{variant}_eval.npy")
    pages = json.loads((calib / f"{variant}_manifest.json").read_text())["eval"]
    sess = ort.InferenceSession(str(models / "layout_publaynet.onnx"), providers=["CPUExecutionProvider"])
    float_pages = [decode(sess.run(None, {sess.get_inputs()[0].name: host_norm(x[i:i + 1])}), p["width"], p["height"])
                   for i, p in enumerate(pages)]
    report = {"variant": variant, "pages": len(pages),
              "float_regions_by_label": {l: sum(r[0] == l for p in float_pages for r in p) for l in LABELS}}
    runner = ClientRunner(har=str(out / f"{variant}.q.har"))
    # SDK_NATIVE takes the host-normalized input, SDK_QUANTIZED the raw uint8 (see eval_emulator.py).
    for key, context, feed in (("native_vs_ort", InferenceContext.SDK_NATIVE, lambda a: host_norm(a).transpose(0, 2, 3, 1)),
                               ("quant_vs_ort", InferenceContext.SDK_QUANTIZED, lambda a: a)):
        decoded = []
        with runner.infer_context(context) as ctx:
            for start in range(0, len(x), 32):
                outs = [np.asarray(o) for o in runner.infer(ctx, feed(x[start:start + 32]))]
                for i in range(len(outs[0])):
                    p = pages[start + i]
                    decoded.append(decode([o[i] for o in outs], p["width"], p["height"]))
        report[key], rows = compare(float_pages, decoded, [p["name"] for p in pages])
        report[key + "_pages"] = rows
    (out / f"eval-{variant}.json").write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if not k.endswith("_pages")}, indent=1))


if __name__ == "__main__":
    main()
