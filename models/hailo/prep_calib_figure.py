"""Calibration and evaluation crops for figure_224 (figure_classifier.onnx).

Usage: python prep_calib_figure.py PAGES_DIR CORPUS_PAGES_DIR REF_JSON_DIR MODELS_DIR OUT_DIR

Crops are cut from 200 dpi page rasters the way the server does it
(crop_region, then a plain cv::resize to 224x224 in FigureClassifierEngine).
The arrays keep the BGR order of the raster: build_hef.py puts the BGR->RGB
swap and the mean/std on the chip, so the runtime feeds the resized crop as is.
Needs cv2 like prep_calib_layout.py.

Where the boxes come from:
  picture         picture items of the reference JSON (gRParse CPU export,
                  REF_JSON_DIR/<pdf stem>.json) for the acm papers and the
                  repo corpus
  layout-picture  picture regions the float layout_publaynet.onnx finds: on the
                  ext papers, which have no reference JSON (calibration), and
                  on the held-out papers, where they are the crops this
                  layout model would hand the classifier (the same figures
                  as the picture boxes, cut by a different detector)
  table           table items of the reference JSON. The classifier is at its
                  most confident on a table a layout model took for a picture,
                  so CALIB_TABLES of them are in calibration: with none, the
                  quantized ranges saturate and such a crop comes out as a tie
                  between classes. They also extend the held-out set.
eval_figure.py reports each kind on its own.
Calibration takes CALIB_COUNT crops from acm001-060, the corpus and the ext
papers (every picture, CALIB_TABLES tables, layout-pictures for the rest),
shuffled, because the DFC reads activation ranges from a prefix of the set.
Evaluation takes every crop of the held-out papers. Exact duplicates are
dropped, held-out crops first, so no evaluation crop is in calibration.
Writes OUT_DIR/figure_224_{calib,eval}.npy and figure_224_manifest.json.
"""
import hashlib
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

sys.path.insert(0, str(Path(__file__).parent))
from eval_layout import HEIGHT, WIDTH, decode, host_norm  # noqa: E402

SIDE = 224
CALIB_COUNT = 1024
CALIB_TABLES = 128
SEED = 1234


def page_files(folder: Path, doc: str) -> dict:
    """Page number -> raster for the <doc>-<page>.png files pdftoppm wrote."""
    tails = ((p.name[len(doc) + 1:-4], p) for p in folder.iterdir() if p.name.startswith(doc + "-"))
    return {int(tail): p for tail, p in tails if tail.isdigit()}


def reference_boxes(ref: Path, pages: dict):
    """(kind, page path, box in raster pixels) for the pictures and tables of one reference JSON."""
    doc = json.loads(ref.read_text())
    for kind, key in (("picture", "pictures"), ("table", "tables")):
        for item in doc.get(key, []):
            prov = item["prov"][0]
            path = pages.get(prov["page_no"])
            if path is None:
                continue
            box, size = prov["bbox"], doc["pages"][str(prov["page_no"])]["size"]
            yield kind, path, (box["l"], box["t"], box["r"], box["b"]), (size["width"], size["height"])


def layout_boxes(session, path: Path):
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    tensor = host_norm(cv2.resize(image, (WIDTH, HEIGHT))[None])
    outputs = session.run(None, {session.get_inputs()[0].name: tensor})
    for region in decode(outputs, image.shape[1], image.shape[0]):
        if region[0] == "picture":
            yield "layout-picture", path, region[2:], (image.shape[1], image.shape[0])


def cut(found, seen, rasters):
    """Resized crops of the found boxes, dropping empty boxes and exact duplicates."""
    crops, rows = [], []
    for kind, path, box, (page_w, page_h) in found:
        if path not in rasters:
            rasters.clear()  # boxes arrive grouped by page; keep one raster in memory
            rasters[path] = cv2.imread(str(path), cv2.IMREAD_COLOR)
        image = rasters[path]
        sx, sy = image.shape[1] / page_w, image.shape[0] / page_h
        left, right = (int(np.clip(round(v * sx), 0, image.shape[1])) for v in (box[0], box[2]))
        top, bottom = (int(np.clip(round(v * sy), 0, image.shape[0])) for v in (box[1], box[3]))
        if right <= left or bottom <= top:
            continue
        crop = cv2.resize(image[top:bottom, left:right], (SIDE, SIDE))
        digest = hashlib.sha256(crop.tobytes()).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        crops.append(crop)
        rows.append({"kind": kind, "page": path.name, "box": [left, top, right, bottom], "sha256": digest})
    return crops, rows


def main() -> None:
    pages, corpus, refs, models, out = (Path(a) for a in sys.argv[1:6])
    out.mkdir(parents=True, exist_ok=True)
    docs = [line.rstrip("\n").split("\t") for line in (pages / "docs.tsv").read_text().splitlines()]
    session = ort.InferenceSession(str(models / "layout_publaynet.onnx"), providers=["CPUExecutionProvider"])
    found = {"eval": [], "calib": [], "extra": []}
    for ref in sorted(refs.glob("*.json")):  # the repo corpus: <stem>-<page>.png beside <stem>.json
        found["calib"] += sorted(reference_boxes(ref, page_files(corpus, ref.stem)), key=lambda f: f[1])
    for doc, split, _, pdf in docs:
        rasters_of = page_files(pages / split, doc)
        if doc.startswith("acm"):
            ref = refs / (Path(pdf).stem + ".json")
            found[split] += sorted(reference_boxes(ref, rasters_of), key=lambda f: f[1])
        if split == "eval" or not doc.startswith("acm"):
            target = "eval" if split == "eval" else "extra"
            for _, path in sorted(rasters_of.items()):
                found[target] += list(layout_boxes(session, path))
    seen, rasters = set(), {}
    kind_order = ("picture", "layout-picture", "table")
    held, held_rows = cut(sorted(found["eval"], key=lambda f: kind_order.index(f[0])), seen, rasters)
    pool, pool_rows = cut(found["calib"] + found["extra"], seen, rasters)
    kinds = np.array([r["kind"] for r in pool_rows])
    rng = np.random.default_rng(SEED)
    take = list(np.nonzero(kinds == "picture")[0])
    take += list(rng.permutation(np.nonzero(kinds == "table")[0])[:CALIB_TABLES])
    for kind in ("layout-picture", "table"):  # fill in this order, each drawn at random
        rest = np.setdiff1d(np.nonzero(kinds == kind)[0], take)
        take += list(rng.permutation(rest)[:max(0, CALIB_COUNT - len(take))])
    take = [int(i) for i in rng.permutation(take)]
    calib, calib_rows = [pool[i] for i in take], [pool_rows[i] for i in take]
    count = lambda rows: {k: sum(r["kind"] == k for r in rows) for k in kind_order}  # noqa: E731
    manifest = {"input": "uint8 NHWC BGR, plain resize of the crop to 224x224",
                "calib_count": len(calib), "calib_kinds": count(calib_rows), "pool_kinds": count(pool_rows),
                "eval_count": len(held), "eval_kinds": count(held_rows), "calib": calib_rows, "eval": held_rows}
    np.save(out / "figure_224_calib.npy", np.stack(calib))
    np.save(out / "figure_224_eval.npy", np.stack(held))
    (out / "figure_224_manifest.json").write_text(json.dumps(manifest, indent=1))
    print({k: v for k, v in manifest.items() if not isinstance(v, list)})


if __name__ == "__main__":
    main()
