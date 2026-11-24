"""Calibration and evaluation pages for layout_800x608 (layout_publaynet.onnx).

Usage: python prep_calib_layout.py PAGES_DIR CORPUS_PAGES_DIR OUT_DIR

PAGES_DIR is the output of render_pages.sh; CORPUS_PAGES_DIR holds the 200 dpi
renders of the repo's own test corpus (the pages prep_calib.py uses). Every
page is resized the way AnchorFreeStrategy does it (src/layout_engine.cpp):
a plain cv::resize of the BGR raster to 608 wide x 800 high, no aspect
preservation. Mean and std are folded onto the chip by build_hef.py, so the
arrays are the raw uint8 NHWC BGR pixels the runtime feeds. OpenCV does the
resize so the pixels match cv::resize (PIL's bilinear filter averages when it
shrinks). The DFC image has no cv2: pip install --no-deps --target a mounted
directory opencv-python-headless and put that directory on PYTHONPATH.

Calibration: every unique corpus page, every page of papers acm001-060, and
pages of the ext papers drawn at random (seed 1234) up to CALIB_COUNT, then
shuffled, because the DFC reads activation ranges from a prefix of the set.
Evaluation: every page of the held-out papers (PAGES_DIR/eval), none of which
is in calibration. Writes OUT_DIR/layout_800x608_{calib,eval}.npy and
layout_800x608_manifest.json (names, sha256 and page sizes).
"""
import hashlib
import json
import sys
from pathlib import Path

import cv2
import numpy as np

HEIGHT, WIDTH = 800, 608
CALIB_COUNT = 1024
SEED = 1234


def unique(paths, seen):
    """Paths whose file bytes were not seen before, with their sha256."""
    kept = []
    for path in paths:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest not in seen:
            seen.add(digest)
            kept.append((path, digest))
    return kept


def load(entries):
    pixels = np.empty((len(entries), HEIGHT, WIDTH, 3), np.uint8)
    rows = []
    for i, (path, digest) in enumerate(entries):
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        pixels[i] = cv2.resize(image, (WIDTH, HEIGHT))
        rows.append({"name": path.name, "sha256": digest, "width": image.shape[1], "height": image.shape[0]})
    return pixels, rows


def main() -> None:
    pages, corpus, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    seen = set()
    held_out = unique(sorted((pages / "eval").glob("*.png")), seen)
    fixed = unique(sorted(corpus.glob("*.png")) + sorted((pages / "calib").glob("acm*.png")), seen)
    extra = unique(sorted((pages / "calib").glob("ext*.png")), seen)
    rng = np.random.default_rng(SEED)
    order = rng.permutation(len(extra))[:max(0, CALIB_COUNT - len(fixed))]
    calib = fixed + [extra[i] for i in sorted(order)]
    calib = [calib[i] for i in rng.permutation(len(calib))]
    manifest = {"input": "uint8 NHWC BGR, plain resize to 608x800", "calib_count": len(calib),
                "calib_corpus_pages": sum(p.parent == corpus for p, _ in calib),
                "calib_sample_pages": sum(p.name.startswith("acm") and p.parent != corpus for p, _ in calib),
                "calib_extra_pages": len(order), "eval_count": len(held_out)}
    for split, entries in (("calib", calib), ("eval", held_out)):
        pixels, manifest[split] = load(entries)
        np.save(out / f"layout_800x608_{split}.npy", pixels)
        print(f"layout_800x608 {split}: {pixels.shape} {pixels.dtype}", flush=True)
    (out / "layout_800x608_manifest.json").write_text(json.dumps(manifest, indent=1))
    print({k: v for k, v in manifest.items() if not isinstance(v, list)})


if __name__ == "__main__":
    main()
