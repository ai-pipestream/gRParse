#!/usr/bin/env python3
"""Build a static page preview of gRParse beside Docling for a PDF corpus.

    uv run --with grpcio --with grpcio-tools python eval/preview/build.py \\
        --pdf-dir /path/to/pdfs --out /tmp/preview \\
        --grparse localhost:50051 --docling-url http://localhost:5001 --limit 50
    python3 -m http.server -d /tmp/preview 8090   # then open http://host:8090/

For every PDF it asks gRParse for the upstream-canonical JSON with page
images, and Docling for its JSON (from a running docling-serve, or from a
directory of JSON files a Docling run already wrote). Page images leave the
JSON as files under docs/<id>/pages/ and each page's image uri is rewritten
to that relative path, so the viewer fetches only the page on screen and
the JSON stays small: the export half of gRParse#306, done at the edge
until the service serves page bytes itself.

Both sides are read in the same docling-core dialect, so the viewer draws
each side's boxes over its own page raster in that page's own coordinate
space (gRParse's CV path measures in pixels, Docling in points; both scale
to the image by the page size).

manifest.json lists every document with each side's status, timing, item
counts by label, and a box agreement summary (greedy IoU >= 0.5 matching per
page), so the viewer can sort the corpus by where the two disagree most.

A rerun skips documents whose docs/<id>/meta.json exists (--force redoes
them). Exit codes: 0 done (individual parse failures are recorded in the
manifest, not fatal); 77 gRParse unreachable or no PDFs found.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import mimetypes
import os
import shutil
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

VIEWER_DIR = Path(__file__).resolve().parent / "viewer"
SKIP = 77
MATCH_IOU = 0.5
ARENAS = ("texts", "tables", "pictures", "key_value_items", "form_items")
IMAGE_EXT = {"image/png": "png", "image/jpeg": "jpg", "image/webp": "webp", "image/svg+xml": "svg"}


# ---------------------------------------------------------------------------
# Docling-dialect JSON helpers (shared shape for both sides)
# ---------------------------------------------------------------------------

def page_size(doc: dict[str, Any], page_no: int) -> tuple[float, float] | None:
    page = (doc.get("pages") or {}).get(str(page_no))
    size = (page or {}).get("size") or {}
    width, height = float(size.get("width") or 0), float(size.get("height") or 0)
    return (width, height) if width > 0 and height > 0 else None


def normalized_box(bbox: dict[str, Any], size: tuple[float, float]) -> tuple[float, float, float, float]:
    """The box as (x0, y0, x1, y1) fractions of the page, top-left origin."""
    width, height = size
    left, right = float(bbox.get("l") or 0), float(bbox.get("r") or 0)
    top, bottom = float(bbox.get("t") or 0), float(bbox.get("b") or 0)
    if str(bbox.get("coord_origin") or "").upper().endswith("BOTTOMLEFT"):
        top, bottom = height - top, height - bottom
    x0, x1 = sorted((left / width, right / width))
    y0, y1 = sorted((top / height, bottom / height))
    return x0, y0, x1, y1


def boxes_by_page(doc: dict[str, Any]) -> dict[int, list[tuple[str, tuple[float, float, float, float]]]]:
    pages: dict[int, list[tuple[str, tuple[float, float, float, float]]]] = {}
    for arena in ARENAS:
        for item in doc.get(arena) or []:
            label = str(item.get("label") or "unknown")
            for prov in item.get("prov") or []:
                page_no, bbox = int(prov.get("page_no") or 0), prov.get("bbox")
                size = page_size(doc, page_no)
                if not bbox or size is None:
                    continue
                pages.setdefault(page_no, []).append((label, normalized_box(bbox, size)))
    return pages


def label_counts(doc: dict[str, Any]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for arena in ARENAS:
        for item in doc.get(arena) or []:
            counts[str(item.get("label") or "unknown")] += 1
    return dict(sorted(counts.items()))


def iou(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> float:
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def agreement(ours: dict[str, Any], docling: dict[str, Any]) -> dict[str, Any]:
    """Greedy one-to-one box matching per page, highest IoU first."""
    totals = Counter()
    per_page: dict[str, dict[str, int]] = {}
    ours_pages, docling_pages = boxes_by_page(ours), boxes_by_page(docling)
    for page_no in sorted(set(ours_pages) | set(docling_pages)):
        left, right = ours_pages.get(page_no, []), docling_pages.get(page_no, [])
        pairs = sorted(((iou(a[1], b[1]), i, j) for i, a in enumerate(left) for j, b in enumerate(right)),
                       reverse=True)
        used_left: set[int] = set()
        used_right: set[int] = set()
        page = Counter()
        for score, i, j in pairs:
            if score < MATCH_IOU:
                break
            if i in used_left or j in used_right:
                continue
            used_left.add(i)
            used_right.add(j)
            page["same_label" if left[i][0] == right[j][0] else "other_label"] += 1
        page["ours_only"] = len(left) - len(used_left)
        page["docling_only"] = len(right) - len(used_right)
        per_page[str(page_no)] = dict(page)
        totals.update(page)
    total_boxes = sum(totals[k] for k in ("same_label", "other_label", "ours_only", "docling_only"))
    return {
        "same_label": totals["same_label"],
        "other_label": totals["other_label"],
        "ours_only": totals["ours_only"],
        "docling_only": totals["docling_only"],
        "score": round(totals["same_label"] / total_boxes, 4) if total_boxes else None,
        "pages": per_page,
    }


def split_images(doc: dict[str, Any], doc_dir: Path, side: str) -> int:
    """Move page image data URIs out to files and point the uri at them;
    drop picture and table data URIs (the viewer draws boxes, not crops)."""
    written = 0
    for key, page in (doc.get("pages") or {}).items():
        image = page.get("image") or {}
        uri = str(image.get("uri") or "")
        if not uri.startswith("data:"):
            continue
        header, _, payload = uri.partition(",")
        mime = header[5:].split(";", 1)[0] or str(image.get("mimetype") or "image/png")
        relative = f"pages/{side}-{key}.{IMAGE_EXT.get(mime, 'bin')}"
        target = doc_dir / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(base64.b64decode(payload))
        image["uri"] = relative
        written += 1
    for arena in ("pictures", "tables"):
        for item in doc.get(arena) or []:
            image = item.get("image")
            if isinstance(image, dict) and str(image.get("uri") or "").startswith("data:"):
                image["uri"] = None
    return written


# ---------------------------------------------------------------------------
# The two producers
# ---------------------------------------------------------------------------

def docling_serve(url: str, data: bytes, name: str, timeout: float, images_scale: float) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    """POST the file to docling-serve's /v1/convert/file; returns (doc, info)."""
    boundary = uuid.uuid4().hex
    fields = {
        "to_formats": "json",
        "image_export_mode": "embedded",
        "include_images": "true",
        "images_scale": str(images_scale),
    }
    body = bytearray()
    for key, value in fields.items():
        body += f"--{boundary}\r\nContent-Disposition: form-data; name=\"{key}\"\r\n\r\n{value}\r\n".encode()
    mime = mimetypes.guess_type(name)[0] or "application/pdf"
    body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"files\"; filename=\"{name}\"\r\n"
             f"Content-Type: {mime}\r\n\r\n").encode()
    body += data + f"\r\n--{boundary}--\r\n".encode()
    request = urllib.request.Request(url.rstrip("/") + "/v1/convert/file", data=bytes(body), method="POST",
                                     headers={"Content-Type": f"multipart/form-data; boundary={boundary}",
                                              "Accept": "application/json"})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read())
    except urllib.error.HTTPError as error:
        detail = error.read()[:500].decode("utf-8", "replace")
        return None, {"status": f"HTTP_{error.code}", "error": detail, "elapsed_ms": _ms(started)}
    except Exception as error:  # noqa: BLE001 - every failure is recorded per document
        return None, {"status": "ERROR", "error": f"{type(error).__name__}: {error}", "elapsed_ms": _ms(started)}
    document = (payload.get("document") or {}).get("json_content")
    info = {"status": str(payload.get("status") or "unknown"), "elapsed_ms": _ms(started),
            "errors": payload.get("errors") or []}
    if payload.get("processing_time") is not None:
        info["server_ms"] = round(float(payload["processing_time"]) * 1000.0, 1)
    if not document:
        info.setdefault("error", "docling-serve returned no json_content")
    return document, info


def docling_from_dir(directory: Path, pdf: Path, root: Path) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    relative = pdf.relative_to(root).with_suffix(".json")
    for candidate in (directory / relative, directory / f"{pdf.stem}.json"):
        if candidate.is_file():
            return json.loads(candidate.read_text()), {"status": "from_dir", "source": str(candidate)}
    return None, {"status": "missing", "error": f"no {relative} under {directory}"}


def ours(client: Any, data: bytes, name: str, timeout: float) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    result = client.convert_bytes(data, name, formats=("CANONICAL_JSON",), timeout=timeout,
                                  include_page_images=True)
    info: dict[str, Any] = {"status": result.status, "elapsed_ms": round(result.elapsed_ms, 1)}
    if result.rpc_error:
        info["error"] = result.rpc_error
    if result.errors:
        info["errors"] = result.errors
    canonical = result.exports.get("canonical_json") or ""
    if not canonical:
        info.setdefault("error", "no canonical_json export")
        return None, info
    return json.loads(canonical), info


def _ms(started: float) -> float:
    return round((time.monotonic() - started) * 1000.0, 1)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def find_pdfs(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.is_file() and path.suffix.lower() == ".pdf")


def build_one(pdf: Path, args: argparse.Namespace, client: Any) -> dict[str, Any]:
    data = pdf.read_bytes()
    doc_id = hashlib.sha256(data).hexdigest()[:16]
    doc_dir = args.out / "docs" / doc_id
    meta_path = doc_dir / "meta.json"
    if meta_path.is_file() and not args.force:
        return json.loads(meta_path.read_text())
    if doc_dir.exists():
        shutil.rmtree(doc_dir)
    doc_dir.mkdir(parents=True)
    entry: dict[str, Any] = {"id": doc_id, "name": pdf.name, "path": str(pdf.relative_to(args.pdf_dir)),
                             "bytes": len(data)}
    with ThreadPoolExecutor(max_workers=2) as both:
        ours_future = both.submit(ours, client, data, pdf.name, args.timeout)
        if args.docling_url:
            docling_future = both.submit(docling_serve, args.docling_url, data, pdf.name, args.timeout,
                                         args.docling_images_scale)
        elif args.docling_json_dir:
            docling_future = both.submit(docling_from_dir, args.docling_json_dir, pdf, args.pdf_dir)
        else:
            docling_future = None
        ours_doc, ours_info = ours_future.result()
        docling_doc, docling_info = docling_future.result() if docling_future else (None, {"status": "not_run"})
    for side, doc, info in (("ours", ours_doc, ours_info), ("docling", docling_doc, docling_info)):
        if doc is not None:
            info["page_images"] = split_images(doc, doc_dir, side)
            info["pages"] = len(doc.get("pages") or {})
            info["labels"] = label_counts(doc)
            (doc_dir / f"{side}.json").write_text(json.dumps(doc, ensure_ascii=False))
        entry[side] = info
    if ours_doc is not None and docling_doc is not None:
        entry["agreement"] = agreement(ours_doc, docling_doc)
    meta_path.write_text(json.dumps(entry, ensure_ascii=False, indent=1))
    return entry


def write_manifest(out: Path, entries: list[dict[str, Any]], run: dict[str, Any]) -> None:
    slim = []
    for entry in entries:
        item = {k: v for k, v in entry.items() if k != "agreement"}
        if "agreement" in entry:
            item["agreement"] = {k: v for k, v in entry["agreement"].items() if k != "pages"}
        slim.append(item)
    temporary = out / "manifest.json.tmp"
    temporary.write_text(json.dumps({"run": run, "documents": slim}, ensure_ascii=False, indent=1))
    os.replace(temporary, out / "manifest.json")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pdf-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--grparse", default=os.environ.get("GRPARSE_TARGET", "localhost:50051"))
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--docling-url", help="docling-serve base URL, e.g. http://localhost:5001")
    source.add_argument("--docling-json-dir", type=Path, help="directory of Docling JSON, mirroring --pdf-dir")
    parser.add_argument("--docling-images-scale", type=float, default=1.0)
    parser.add_argument("--limit", type=int, default=0, help="first N PDFs in path order (0 = all)")
    parser.add_argument("--every", type=int, default=1, help="take every Nth PDF, for a spread sample")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    pdfs = find_pdfs(args.pdf_dir)[:: max(1, args.every)]
    if args.limit:
        pdfs = pdfs[: args.limit]
    if not pdfs:
        print(f"no PDFs under {args.pdf_dir}", file=sys.stderr)
        return SKIP

    from scorecard.client import GrparseClient, Unreachable

    args.out.mkdir(parents=True, exist_ok=True)
    for asset in VIEWER_DIR.iterdir():
        shutil.copy2(asset, args.out / asset.name)
    run = {"started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "grparse": args.grparse,
           "docling": args.docling_url or (str(args.docling_json_dir) if args.docling_json_dir else None),
           "pdf_dir": str(args.pdf_dir), "documents": len(pdfs)}
    entries: list[dict[str, Any]] = []
    lock = threading.Lock()
    with GrparseClient(args.grparse) as client:
        try:
            info = client.service_info()
            run["grparse_service"] = f"{info.name} {info.version}"
        except Unreachable as error:
            print(f"gRParse unreachable: {error}", file=sys.stderr)
            return SKIP
        with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
            futures = {pool.submit(build_one, pdf, args, client): pdf for pdf in pdfs}
            for done, future in enumerate(as_completed(futures), start=1):
                pdf = futures[future]
                try:
                    entry = future.result()
                except Unreachable:
                    raise
                except Exception as error:  # noqa: BLE001 - record and keep going
                    entry = {"id": None, "name": pdf.name, "path": str(pdf.relative_to(args.pdf_dir)),
                             "error": f"{type(error).__name__}: {error}"}
                with lock:
                    entries.append(entry)
                    score = (entry.get("agreement") or {}).get("score")
                    print(f"[{done}/{len(pdfs)}] {pdf.name}: ours={(entry.get('ours') or {}).get('status')} "
                          f"docling={(entry.get('docling') or {}).get('status')} agreement={score}", flush=True)
                    if done % 10 == 0:
                        write_manifest(args.out, entries, run)
    run["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    entries.sort(key=lambda e: e.get("path") or "")
    write_manifest(args.out, entries, run)
    print(f"wrote {args.out / 'manifest.json'}; serve with: python3 -m http.server -d {args.out} 8090")
    return 0


if __name__ == "__main__":
    sys.exit(main())
