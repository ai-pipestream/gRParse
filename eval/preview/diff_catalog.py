#!/usr/bin/env python3
"""Catalogue every difference between the two sides of a preview run.

    python3 eval/preview/diff_catalog.py <preview_dir> --out catalog.json \
        [--viewer-url http://host:8090]
    python3 eval/preview/diff_catalog.py --themes run1=catalog1.json run2=catalog2.json

Per page, both sides' items become boxes with the viewer's rules (provs merged
where they touch, a formula as one box, panel pictures left to their figure).
Text inside a picture is kept here: it is a difference too. Boxes are matched
one to one at IoU 0.5 or more. Every unmatched box, every label mismatch, every
text mismatch on a matched pair and every table shape mismatch lands in exactly
one category, with a count, the papers it touches and example viewer links.
``--themes`` rolls categories up into the QA themes, one column per catalog.

The viewer's agreement score is not changed by anything here.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import unicodedata
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from preview import build  # noqa: E402

ARENAS = ("texts", "tables", "pictures", "key_value_items", "form_items")
MATCH_IOU = 0.5
EXAMPLES = 4

Box = tuple[float, float, float, float]


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def parent_ref(item: dict[str, Any]) -> str:
    return str((item.get("parent") or {}).get("$ref") or "")


def page_boxes(doc: dict[str, Any]) -> dict[int, list[dict[str, Any]]]:
    """page -> boxes, each {label, box, item, arena}; panel pictures stay with their figure."""
    pages: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for arena in ARENAS:
        for item in doc.get(arena) or []:
            if arena == "pictures" and parent_ref(item).startswith("#/pictures/"):
                continue
            for page, merged in build.item_boxes(doc, item).items():
                for box in merged:
                    pages[page].append({"label": str(item.get("label") or "unknown"), "box": box,
                                        "item": item, "arena": arena})
    return pages


def area(box: Box) -> float:
    return max(0.0, box[2] - box[0]) * max(0.0, box[3] - box[1])


def overlap(a: Box, b: Box) -> float:
    return max(0.0, min(a[2], b[2]) - max(a[0], b[0])) * max(0.0, min(a[3], b[3]) - max(a[1], b[1]))


def centre_in(box: Box, region: Box) -> bool:
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    return region[0] <= cx <= region[2] and region[1] <= cy <= region[3]


def match_boxes(left: list[dict[str, Any]], right: list[dict[str, Any]]) -> dict[int, int]:
    """Greedy one-to-one matching, highest IoU first; returns left index -> right index."""
    pairs = sorted(((build.iou(a["box"], b["box"]), i, j) for i, a in enumerate(left)
                    for j, b in enumerate(right)), key=lambda p: -p[0])
    matched: dict[int, int] = {}
    taken: set[int] = set()
    for score, i, j in pairs:
        if score < MATCH_IOU:
            break
        if i in matched or j in taken:
            continue
        matched[i] = j
        taken.add(j)
    return matched


# ---------------------------------------------------------------------------
# Text comparison
# ---------------------------------------------------------------------------

def norm(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


STEPS = [
    ("U+FFFE soft-hyphen markers", lambda s: s.replace("￾", "").replace("­", "")),
    ("quotes", lambda s: re.sub(r"[‘’‛′`]", "'", re.sub(r"[“”″]", '"', s))),
    ("dashes/minus", lambda s: re.sub(r"[‐-―−]", "-", s)),
    ("bullets", lambda s: re.sub(r"[•·∙●▪]", "*", s)),
    ("ligatures/compatibility chars (NFKC)", lambda s: unicodedata.normalize("NFKC", s)),
    ("hyphenation joins", lambda s: re.sub(r"(\w)- (\w)", r"\1\2", s)),
    ("spacing", lambda s: re.sub(r"\s+", "", s)),
]


def soft_words(text: str) -> Counter[str]:
    for _, step in STEPS[:-1]:
        text = step(text)
    return Counter(norm(text).split())


def text_kind(ours: str, docling: str) -> str | None:
    """Name the cumulative normalisations that make two texts equal, else a word-level kind."""
    a, b = norm(ours), norm(docling)
    if a == b:
        return None
    if not b:
        return "text: Docling text empty, gRParse has text"
    if not a:
        return "text: gRParse text empty, Docling has text"
    used = []
    for name, step in STEPS:
        na, nb = step(a), step(b)
        if (na, nb) != (a, b):
            used.append(name)
        a, b = na, nb
        if a == b:
            return "text: differs only in " + " + ".join(used)
    wa, wb = soft_words(ours), soft_words(docling)
    if wa == wb:
        return "text: same words, different order"
    missing, extra = sum((wb - wa).values()), sum((wa - wb).values())
    if (missing + extra) / max(1, sum(wb.values())) <= 0.1:
        return "text: small word differences (<=10% of words)"
    if missing and not extra:
        return "text: gRParse item shorter (words missing)"
    if extra and not missing:
        return "text: gRParse item longer (extra words)"
    return "text: substantially different text"


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------

def label_category(a: dict[str, Any], b: dict[str, Any]) -> str:
    """A matched pair with different labels; a known structural difference gets its own bucket."""
    if (a["label"] == "caption" and parent_ref(a["item"]).startswith("#/pictures/")
            and b["label"] == "text" and parent_ref(b["item"]).startswith("#/pictures/")):
        return "known structure: gRParse picture caption vs Docling text child of the picture"
    return f"label: gRParse {a['label']} vs Docling {b['label']}"


def pair_categories(a: dict[str, Any], b: dict[str, Any]) -> list[tuple[str, str]]:
    """Categories for one matched pair: label, text and table shape."""
    out = []
    if a["label"] != b["label"]:
        out.append((label_category(a, b), f"gRParse {a['item'].get('self_ref')} {norm(a['item'].get('text') or '')[:80]!r} | "
                                          f"Docling {b['item'].get('self_ref')} {norm(b['item'].get('text') or '')[:80]!r}"))
    if a["arena"] == b["arena"] == "texts" and a["label"] == b["label"]:
        kind = text_kind(a["item"].get("text") or "", b["item"].get("text") or "")
        if kind:
            out.append((f"{kind} [{a['label']}]", f"gRParse {norm(a['item'].get('text') or '')[:100]!r} | "
                                                  f"Docling {norm(b['item'].get('text') or '')[:100]!r}"))
    if a["arena"] == b["arena"] == "tables":
        da, db = a["item"].get("data") or {}, b["item"].get("data") or {}
        sa, sb = (da.get("num_rows") or 0, da.get("num_cols") or 0), (db.get("num_rows") or 0, db.get("num_cols") or 0)
        if sa != sb:
            kind = ("gRParse more rows" if sa[0] > sb[0] else "gRParse fewer rows" if sa[0] < sb[0]
                    else "same rows, different columns")
            out.append((f"table shape: {kind}", f"gRParse {sa[0]}x{sa[1]} vs Docling {sb[0]}x{sb[1]}"))
    return out


def boundary_category(box: Box, others: list[dict[str, Any]], label: str, side: str) -> str | None:
    """Split/merge/overlap against the other side's boxes, else None."""
    covering = [o for o in others if overlap(box, o["box"]) > 0.5 * area(o["box"])]
    if len(covering) >= 2:
        return f"boundary: {side} merges what {'Docling' if side == 'gRParse' else 'gRParse'} splits [{label}]"
    if any(overlap(box, o["box"]) > 0.5 * area(box) for o in others):
        return f"boundary: {side} box is part of a larger {'Docling' if side == 'gRParse' else 'gRParse'} item [{label}]"
    if any(overlap(box, o["box"]) > 0 for o in others):
        return f"boundary: overlapping but IoU < 0.5 ({side} side) [{label}]"
    return None


def ours_only_category(a: dict[str, Any], left: list[dict[str, Any]], right: list[dict[str, Any]]) -> str:
    box, label = a["box"], a["label"]
    our_pics = [o["box"] for o in left if o["label"] == "picture"]
    if label == "picture":
        doc_pics = [o["box"] for o in right if o["label"] == "picture"]
        return ("gRParse only: picture box differs from Docling's" if any(overlap(box, p) > 0.3 * area(box) for p in doc_pics)
                else "gRParse only: picture Docling did not detect")
    if a["arena"] == "texts" and label != "caption" and any(centre_in(box, p) for p in our_pics):
        return ("gRParse only: figure text item (picture child)" if parent_ref(a["item"]).startswith("#/pictures/")
                else "gRParse only: figure text as a body item (not picture children)")
    if a["arena"] == "texts" and label != "caption" and any(
            centre_in(box, o["box"]) for o in left if o["label"] == "table"):
        return "gRParse only: table words as body text"
    return boundary_category(box, right, label, "gRParse") or f"gRParse only: no Docling item there [{label}]"


def docling_only_category(b: dict[str, Any], left: list[dict[str, Any]], right: list[dict[str, Any]]) -> str:
    box, label = b["box"], b["label"]
    our_pics = [o["box"] for o in left if o["label"] == "picture"]
    if label == "picture":
        return ("Docling only: picture box differs from gRParse's" if any(overlap(box, p) > 0.3 * area(box) for p in our_pics)
                else "Docling only: picture gRParse did not detect")
    words = Counter(norm(b["item"].get("text") or "").split())
    our_words = Counter(norm(" ".join(o["item"].get("text") or "" for o in left)).split())
    doc_pics = [o["box"] for o in right if o["label"] == "picture"]
    if parent_ref(b["item"]).startswith("#/pictures/") or any(centre_in(box, p) for p in our_pics + doc_pics):
        return ("Docling only: figure text as separate items (gRParse has the words elsewhere)"
                if words and not (words - our_words) else "Docling only: figure text gRParse lacks")
    if any(centre_in(box, o["box"]) for o in left if o["label"] == "table"):
        return "Docling only: text inside a gRParse table region"
    found = boundary_category(box, left, label, "Docling")
    if found:
        return found
    return (f"Docling only: content gRParse lacks entirely [{label}]" if words and (words - our_words)
            else f"Docling only: no gRParse item there, words present elsewhere [{label}]")


def page_differences(left: list[dict[str, Any]], right: list[dict[str, Any]]) -> list[tuple[str, str]]:
    """Every (category, detail) on one page."""
    matched = match_boxes(left, right)
    out: list[tuple[str, str]] = []
    for i, j in matched.items():
        out.extend(pair_categories(left[i], right[j]))
    for i, a in enumerate(left):
        if i not in matched:
            out.append((ours_only_category(a, left, right),
                        f"{a['item'].get('self_ref')} {norm(a['item'].get('text') or '')[:120]!r}"))
    taken = set(matched.values())
    for j, b in enumerate(right):
        if j not in taken:
            out.append((docling_only_category(b, left, right),
                        f"{b['item'].get('self_ref')} {norm(b['item'].get('text') or '')[:120]!r}"))
    return out


# ---------------------------------------------------------------------------
# Catalog and themes
# ---------------------------------------------------------------------------

def catalog(root: Path, viewer_url: str = "") -> list[dict[str, Any]]:
    cats: dict[str, dict[str, Any]] = defaultdict(lambda: {"count": 0, "papers": set(), "examples": []})
    for meta in json.loads((root / "manifest.json").read_text())["documents"]:
        if not meta.get("id"):
            continue
        folder = root / "docs" / meta["id"]
        if not (folder / "ours.json").is_file() or not (folder / "docling.json").is_file():
            continue
        ours, docling = (json.loads((folder / f"{side}.json").read_text()) for side in ("ours", "docling"))
        name = Path(meta["path"]).name[:70]
        ob, db = page_boxes(ours), page_boxes(docling)
        for page in sorted(set(ob) | set(db)):
            for category, detail in page_differences(ob.get(page, []), db.get(page, [])):
                entry = cats[category]
                entry["count"] += 1
                entry["papers"].add(name)
                if len(entry["examples"]) < EXAMPLES and all(e["doc"] != meta["id"] for e in entry["examples"]):
                    url = f"{viewer_url.rstrip('/')}/#doc={meta['id']}&page={page}" if viewer_url else ""
                    entry["examples"].append({"paper": name, "doc": meta["id"], "page": page, "url": url,
                                              "detail": detail[:220]})
    return sorted(({"category": k, "count": v["count"], "papers": len(v["papers"]), "examples": v["examples"]}
                   for k, v in cats.items()), key=lambda r: -r["count"])


NESTED = ("gRParse only: picture nested inside another picture", "gRParse only: composite picture around other pictures")
THEMES = [
    ("1 Figure and chart text", lambda c: "figure text" in c),
    ("2 U+FFFE markers", lambda c: c.startswith("text: differs only in U+FFFE")),
    ("3 Formula text empty on Docling", lambda c: c.startswith("text: Docling text empty") and "formula" in c),
    ("4 Formula boxes", lambda c: c.startswith("boundary") and "formula" in c),
    ("5 Table words as body text", lambda c: "table words as body text" in c),
    ("6 Pictures (nested/composite)", lambda c: c in NESTED),
    ("6b Pictures (box differs / missed)", lambda c: "picture" in c and "figure text" not in c and c not in NESTED
     and not c.startswith(("label", "known structure"))),
    ("7 Labels", lambda c: c.startswith("label:")),
    ("7b Known structure (panel captions)", lambda c: c.startswith("known structure:")),
    ("8 Box boundaries", lambda c: c.startswith("boundary") and "formula" not in c),
    ("9 Typography only", lambda c: c.startswith("text: differs only in") and "U+FFFE" not in c),
    ("10 Word-level text", lambda c: c.startswith("text:") and not c.startswith("text: differs only") and "empty" not in c),
    ("11 Table shape", lambda c: c.startswith("table shape")),
    ("Content on one side only", lambda c: "no Docling item there" in c or "lacks entirely" in c
     or "no gRParse item there" in c),
]


def theme_counts(rows: list[dict[str, Any]]) -> dict[str, int]:
    counts = {name: sum(r["count"] for r in rows if match(r["category"])) for name, match in THEMES}
    counts["TOTAL"] = sum(r["count"] for r in rows)
    return counts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("preview", nargs="?", type=Path, help="preview output folder (has manifest.json)")
    parser.add_argument("--out", type=Path, help="write the catalog JSON here")
    parser.add_argument("--viewer-url", default="", help="base URL for example links")
    parser.add_argument("--themes", nargs="+", metavar="NAME=CATALOG", help="print theme counts side by side")
    args = parser.parse_args(argv)
    if args.themes:
        runs = [(spec.split("=", 1)[0], theme_counts(json.loads(Path(spec.split("=", 1)[1]).read_text())))
                for spec in args.themes]
        print("theme".ljust(38) + "".join(name.rjust(10) for name, _ in runs))
        for theme in runs[0][1]:
            print(theme.ljust(38) + "".join(str(counts[theme]).rjust(10) for _, counts in runs))
        return 0
    if args.preview is None or args.out is None:
        parser.error("a preview folder and --out are required unless --themes is given")
    rows = catalog(args.preview, args.viewer_url)
    args.out.write_text(json.dumps(rows, ensure_ascii=False, indent=1))
    for row in rows:
        print(f"{row['count']:7d} {row['papers']:4d}  {row['category']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
