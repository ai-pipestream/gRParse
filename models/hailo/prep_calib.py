"""Build Hailo calibration and evaluation sets for the RapidOCR models.

PAGES_DIR holds page images (PNG): gRParse's golden corpus and test
fixtures, PDFs rendered at 200 dpi with pdftoppm. Pages are deduplicated by
sha256. EVAL_PAGES are held out of every calibration set and kept for the
float-vs-quantized comparison.

Preprocessing follows RapidOcrOnnx as gRParse builds it (BGR input):
  det  mean 0.485/0.456/0.406 * 255, std 0.229/0.224/0.225 * 255
  rec  mean 127.5, std 127.5, height 48, right-padded with mid grey
       (normalized 0, as CrnnNet::getTextLineBatch pads)
  cls  mean 127.5, std 127.5, 48x192, right-padded with white and cropped
       when wider (AngleNet adjustTargetImg)
The sets are stored as uint8 NHWC BGR; normalization runs on the chip
(model script), so calibration sees the same uint8 the runtime will feed.

Text lines come from gRParse's own CPU OCR (fetch_lines.py): one box and
one recognized string per OCR line. Each crop is re-recognized here with the
float rec model; a crop whose float text is far from gRParse's text
(CER > 0.5 at natural width, e.g. a box reported in a recovered-orientation
frame) is dropped and counted. Calibration uses every natural-scale window of every kept line (320 and 640
for rec, 192 for cls, each cls window also turned 180 degrees), sampled to
MAX_PER_SET; det uses a letterbox of each page plus tiles cut at a random
page scale in DET_TILE_SCALES. Eval sets keep one tensor per line (its
leftmost window) and one letterbox per page, unchanged across runs.

Usage: python prep_calib.py PAGES_DIR MODELS_DIR OUT_DIR LINES_DIR
"""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image
from scipy import ndimage

SEED = 1234
EVAL_PAGES = {"quickstart-sample-2.png", "scanned-image-1.png", "two-column-1.png", "report_page.png"}
DET_MEAN = np.array([0.485, 0.456, 0.406], np.float32) * 255
DET_STD = np.array([0.229, 0.224, 0.225], np.float32) * 255
MAX_PER_SET = 1024  # the DFC's recommended minimum for optimization level 2+
DET_TILE_SCALES = (0.5, 1.5)  # page scale jitter for det tiles


def load_pages(pages_dir):
    seen, pages = {}, []
    for path in sorted(pages_dir.glob("*.png")):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest in seen:
            continue
        seen[digest] = path.name
        rgb = np.asarray(Image.open(path).convert("RGB"))
        pages.append((path.name, digest, np.ascontiguousarray(rgb[:, :, ::-1])))
    return pages


def letterbox(img, size, fill=255):
    h, w = img.shape[:2]
    scale = min(size / h, size / w)
    nh, nw = max(1, round(h * scale)), max(1, round(w * scale))
    resized = np.asarray(Image.fromarray(img[:, :, ::-1]).resize((nw, nh), Image.BILINEAR))[:, :, ::-1]
    out = np.full((size, size, 3), fill, np.uint8)
    out[:nh, :nw] = resized
    return out


def random_tiles(img, size, count, rng):
    h, w = img.shape[:2]
    if h < size or w < size:
        padded = np.full((max(h, size), max(w, size), 3), 255, np.uint8)
        padded[:h, :w] = img
        img, (h, w) = padded, padded.shape[:2]
    tiles = []
    for _ in range(count):
        y, x = rng.integers(0, h - size + 1), rng.integers(0, w - size + 1)
        tiles.append(img[y:y + size, x:x + size])
    return tiles


def scaled(img, scale):
    h, w = img.shape[:2]
    nh, nw = max(1, round(h * scale)), max(1, round(w * scale))
    return np.asarray(Image.fromarray(img[:, :, ::-1]).resize((nw, nh), Image.BILINEAR))[:, :, ::-1]


def windows(line, width, fill):
    """Every non-overlapping natural-scale window of a height-48 line, padded."""
    out = []
    for x in range(0, line.shape[1], width):
        part = line[:, x:x + width]
        tile = np.full((48, width, 3), fill, np.uint8)
        tile[:, :part.shape[1]] = part
        out.append((x, tile))
    return out


def det_input(img_bgr):
    x = (img_bgr.astype(np.float32) - DET_MEAN) / DET_STD
    return x.transpose(2, 0, 1)[None]


def db_boxes(prob, thresh=0.3, box_score=0.5, unclip=1.6, min_size=3):
    """Axis-aligned DB post-process: components of prob>thresh, scored and unclipped."""
    labels, n = ndimage.label(prob > thresh)
    boxes = []
    for idx, sl in enumerate(ndimage.find_objects(labels), start=1):
        if sl is None:
            continue
        ys, xs = sl
        bh, bw = ys.stop - ys.start, xs.stop - xs.start
        if min(bh, bw) < min_size:
            continue
        score = float(prob[sl][labels[sl] == idx].mean())
        if score < box_score:
            continue
        d = (bh * bw) * unclip / (2 * (bh + bw))
        boxes.append((xs.start - d, ys.start - d, xs.stop + d, ys.stop + d, score))
    return boxes


def to_height(crop, height=48):
    h, w = crop.shape[:2]
    nw = max(1, int(w * height / h))
    return np.asarray(Image.fromarray(crop[:, :, ::-1]).resize((nw, height), Image.BILINEAR))[:, :, ::-1]


def rec_tensor(crop, width, fill=128):
    """Height-48 line in a fixed-width bucket: right-padded with mid grey, or,
    when the line is wider than the bucket, its leftmost natural-scale window
    (a fixed-width HEF needs long lines split into windows; squeezing them
    destroys the glyphs)."""
    line = to_height(crop)[:, :width]
    out = np.full((48, width, 3), fill, np.uint8)
    out[:, :line.shape[1]] = line
    return out, line.shape[1]


def rec_natural(crop):
    """The float reference the server computes: height 48, natural width."""
    return norm127(to_height(crop))


def cls_tensor(crop):
    line = to_height(crop)[:, :192]
    out = np.full((48, 192, 3), 255, np.uint8)
    out[:, :line.shape[1]] = line
    return out


def norm127(u8):
    return ((u8.astype(np.float32) - 127.5) / 127.5).transpose(2, 0, 1)[None]


def ctc_decode(probs, keys):
    ids = probs.argmax(-1)
    out, prev = [], -1
    for i in ids:
        if i != prev and i != 0:
            out.append(keys[i - 1] if i - 1 < len(keys) else "")
        prev = i
    return "".join(out)


def cer(a, b):
    """Character error rate of a against reference b (Levenshtein / len(b))."""
    if not b:
        return 0.0 if not a else 1.0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1] / len(b)


def pick(items, rng, limit=MAX_PER_SET):
    if len(items) <= limit:
        return items
    idx = sorted(rng.choice(len(items), limit, replace=False))
    return [items[i] for i in idx]


def main():
    pages_dir, models_dir, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    keys = (models_dir / "ppocr_keys_v1.txt").read_text(encoding="utf-8").splitlines() + [" "]
    so = ort.SessionOptions()
    so.intra_op_num_threads = 16
    rec = ort.InferenceSession(str(models_dir / "ch_PP-OCRv3_rec_infer.onnx"), so, providers=["CPUExecutionProvider"])
    cls = ort.InferenceSession(str(models_dir / "ch_ppocr_mobile_v2.0_cls_infer.onnx"), so, providers=["CPUExecutionProvider"])

    pages = load_pages(pages_dir)
    calib_pages = [p for p in pages if p[0] not in EVAL_PAGES]
    eval_pages = [p for p in pages if p[0] in EVAL_PAGES]
    manifest = {"seed": SEED, "calib_pages": [[n, d] for n, d, _ in calib_pages],
                "eval_pages": [[n, d] for n, d, _ in eval_pages]}

    for size in (960, 640):
        tiles = []
        per_page = -(-int(MAX_PER_SET * 1.1) // len(calib_pages)) - 1
        for name, _, img in calib_pages:
            tiles.append(("letterbox", name, letterbox(img, size)))
            for _ in range(per_page):
                s_ = float(rng.uniform(*DET_TILE_SCALES))
                tiles += [(f"tile@{s_:.3f}", name, t) for t in random_tiles(scaled(img, s_), size, 1, rng)]
        tiles = pick(tiles, rng)
        np.save(out / f"det_{size}_calib.npy", np.stack([t for _, _, t in tiles]))
        np.save(out / f"det_{size}_eval.npy", np.stack([letterbox(img, size) for _, _, img in eval_pages]))
        manifest[f"det_{size}"] = {"count": len(tiles), "kinds": [[k, n] for k, n, _ in tiles]}
        print(f"det_{size}: {len(tiles)} calibration tiles, {len(eval_pages)} eval pages")

    lines_dir = Path(sys.argv[4])

    def lines_of(page_set):
        rows, dropped = [], {}
        for name, _, img in page_set:
            data = json.loads((lines_dir / f"{Path(name).stem}.json").read_text(encoding="utf-8"))
            size = data["sizes"].get("1") or next(iter(data["sizes"].values()), None)
            if not size:
                continue
            sx, sy = img.shape[1] / size["width"], img.shape[0] / size["height"]
            for line in data["lines"]:
                l, t, r, b = line["bbox"]
                if line["origin"] != "COORD_ORIGIN_TOPLEFT":
                    t, b = size["height"] - t, size["height"] - b
                box = [max(0, int(l * sx)), max(0, int(min(t, b) * sy)),
                       min(img.shape[1], int(r * sx + 0.999)), min(img.shape[0], int(max(t, b) * sy + 0.999))]
                if box[2] - box[0] < 8 or box[3] - box[1] < 8 or not line["text"].strip():
                    continue
                crop = img[box[1]:box[3], box[0]:box[2]]
                text = ctc_decode(rec.run(None, {"x": rec_natural(crop)})[0][0], keys)
                if cer(text, line["text"]) > 0.5:
                    dropped[name] = dropped.get(name, 0) + 1
                    continue
                rows.append((name, box, line["text"], crop))
        return rows, dropped

    rows, dropped = lines_of(calib_pages)
    sets = {"rec_48x320": [], "rec_48x640": [], "cls_48x192": []}
    for name, box, gtext, crop in rows:
        line = to_height(crop)
        for width, key, fill in ((320, "rec_48x320", 128), (640, "rec_48x640", 128), (192, "cls_48x192", 255)):
            for x, tile in windows(line, width, fill):
                sets[key].append(({"page": name, "box": box, "x": x}, tile))
                if key == "cls_48x192":
                    sets[key].append(({"page": name, "box": box, "x": x, "rot180": True},
                                      np.ascontiguousarray(tile[::-1, ::-1])))
    calib_meta = {"lines": len(rows), "dropped_cer_gt_0.5": dropped}
    for key, items in sets.items():
        chosen = pick(items, rng)
        np.save(out / f"{key}_calib.npy", np.stack([t for _, t in chosen]))
        calib_meta[key] = {"windows_available": len(items), "count": len(chosen), "sources": [m for m, _ in chosen]}
        print(f"{key}: {len(chosen)} calibration windows of {len(items)} from {len(rows)} lines")
    manifest["lines_calib"] = calib_meta
    print(f"calib lines dropped: {dropped}")

    for split, page_set in (("eval", eval_pages),):
        rows, dropped = lines_of(page_set)
        r320, r640, c192, c192_rot, meta = [], [], [], [], []
        for name, box, gtext, crop in rows:
            t320, used = rec_tensor(crop, 320)
            t640, used640 = rec_tensor(crop, 640)
            tc = cls_tensor(crop)
            tcr = cls_tensor(np.ascontiguousarray(crop[::-1, ::-1]))
            meta.append({"page": name, "box": box, "grparse_text": gtext, "rec_width_320": used,
                         "rec_width_640": used640,
                         "float_text_320": ctc_decode(rec.run(None, {"x": norm127(t320)})[0][0], keys),
                         "float_text_640": ctc_decode(rec.run(None, {"x": norm127(t640)})[0][0], keys),
                         "float_cls": [float(a) for a in cls.run(None, {"x": norm127(tc)})[0][0]],
                         "float_cls_rot180": [float(a) for a in cls.run(None, {"x": norm127(tcr)})[0][0]]})
            r320.append(t320); r640.append(t640); c192.append(tc); c192_rot.append(tcr)
        np.save(out / f"rec_48x320_{split}.npy", np.stack(r320))
        np.save(out / f"rec_48x640_{split}.npy", np.stack(r640))
        cls_set = np.stack(c192 + c192_rot)
        np.save(out / f"cls_48x192_{split}.npy", cls_set)
        (out / f"lines_{split}.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
        manifest[f"lines_{split}"] = {"count": len(meta), "dropped_cer_gt_0.5": dropped, "cls_count": len(cls_set)}
        print(f"{split}: {len(meta)} line crops, cls {len(cls_set)}, dropped {dropped}")

    for f in sorted(out.glob("*.npy")):
        manifest.setdefault("files", {})[f.name] = hashlib.sha256(f.read_bytes()).hexdigest()
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
