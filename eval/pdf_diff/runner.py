"""PDF backend differential: tables per-family divergence between a
PdfBackend service (grpc-pdfium first) and the in-process poppler path.

The poppler leg is ``poppler_floor`` (built from this directory), which
replays the exact poppler-cpp calls gRParse makes. The service leg dials a
running backend over the ``ai.pipestream.parse.pdf.v1`` contract, with the
python stubs generated from the sha256-pinned pipestream-protos release
tarball. Corpus: the scorecard PDFs plus ``~/parser-failed-docs`` as
load-status cases.

Run (backend listening on localhost:50051 by default)::

    uv run --with grpcio --with grpcio-tools --with numpy \
        python eval/pdf_diff/runner.py [--target host:port] [--dpi 72]

Writes ``eval/pdf_diff/out/report.md`` and ``metrics.json``. Divergence is
the diagnostic, not a failure: the report quantifies parity, the reader
judges it (same discipline as the frontend regression legs).
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
CACHE = HERE / ".cache"
OUT = HERE / "out"

PROTOS_VERSION = "0.15.0"
PROTOS_SHA256 = "558974e94c148b351f7dce74ac1189bd41e0433d5fa104cffa21c82c62c6db9b"
PROTOS_URL = (
    "https://git.rokkon.com/api/packages/ai-pipestream/generic/"
    f"pipestream-protos/{PROTOS_VERSION}/pipestream-protos-{PROTOS_VERSION}.tgz"
)
PROTO_REL = "ai/pipestream/parse/pdf/v1"

FAILED_DOCS_DIR = Path.home() / "parser-failed-docs"


def stage_stubs() -> Path:
    """Fetches the pinned proto release and generates python stubs."""
    CACHE.mkdir(exist_ok=True)
    tgz = CACHE / f"pipestream-protos-{PROTOS_VERSION}.tgz"
    if not tgz.exists():
        urllib.request.urlretrieve(PROTOS_URL, tgz)
    digest = hashlib.sha256(tgz.read_bytes()).hexdigest()
    if digest != PROTOS_SHA256:
        tgz.unlink()
        raise SystemExit(f"proto tarball sha256 mismatch: {digest}")
    proto_dir = CACHE / "protos"
    gen_dir = CACHE / "gen"
    if not (gen_dir / "ai" / "pipestream").exists():
        proto_dir.mkdir(exist_ok=True)
        with tarfile.open(tgz) as tar:
            members = [m for m in tar.getmembers() if m.name.startswith("pdf-backend/proto/")]
            tar.extractall(proto_dir, members=members, filter="data")
        gen_dir.mkdir(exist_ok=True)
        from grpc_tools import protoc

        root = proto_dir / "pdf-backend" / "proto"
        args = [
            "protoc",
            f"-I{root}",
            f"--python_out={gen_dir}",
            f"--grpc_python_out={gen_dir}",
            str(root / PROTO_REL / "pdf_backend_types.proto"),
            str(root / PROTO_REL / "pdf_backend_service.proto"),
        ]
        if protoc.main(args) != 0:
            raise SystemExit("stub generation failed")
    return gen_dir


def load_stubs(gen_dir: Path):
    sys.path.insert(0, str(gen_dir))
    from ai.pipestream.parse.pdf.v1 import (  # noqa: E402
        pdf_backend_service_pb2 as svc,
        pdf_backend_service_pb2_grpc as svc_grpc,
        pdf_backend_types_pb2 as types,
    )

    return svc, svc_grpc, types


def poppler_leg(pdf: Path, dpi: float, raster_dir: Path) -> dict:
    floor = HERE / "poppler_floor"
    if not floor.exists():
        subprocess.run([str(HERE / "build_floor.sh")], check=True)
    raster_dir.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        [str(floor), str(pdf), str(dpi), str(raster_dir)],
        capture_output=True,
        timeout=600,
    )
    if proc.returncode != 0:
        return {"load_ok": False, "pages": [], "crashed": True}
    return json.loads(proc.stdout.decode())


def service_leg(stub, svc, types, pdf: Path, dpi: float) -> dict:
    data = pdf.read_bytes()
    out: dict = {"pages": [], "fonts": [], "rasters": []}
    parse_req = svc.ParseRequest(document=types.PdfDocument(data=data))
    load_status = None
    for msg in stub.Parse(parse_req, timeout=600):
        kind = msg.WhichOneof("payload")
        if kind == "header":
            load_status = types.LoadStatus.Name(msg.header.capabilities.load_status)
            for p in msg.header.pages:
                out["pages"].append(
                    {
                        "index": p.page_index,
                        "width_pts": p.width_pts,
                        "height_pts": p.height_pts,
                        "rotation": p.rotation_degrees,
                        "text": [],
                    }
                )
        elif kind == "page":
            page = out["pages"][msg.page.page_index]
            for cell in msg.page.text_cells:
                page["text"].append(
                    {
                        "x": cell.bbox.x0,
                        # contract boxes are bottom-left origin; normalize to
                        # the top-left origin poppler uses.
                        "y": page["height_pts"] - cell.bbox.y1,
                        "w": cell.bbox.x1 - cell.bbox.x0,
                        "h": cell.bbox.y1 - cell.bbox.y0,
                        "text": cell.text,
                        "font_size": cell.font_size,
                    }
                )
        elif kind == "fonts":
            for f in msg.fonts.fonts:
                out["fonts"].append(f.base_name)
    out["load_status"] = load_status or "NO_HEADER"
    if out["load_status"] == "LOAD_STATUS_OK":
        render_req = svc.RenderRequest(
            document=types.PdfDocument(data=data),
            dpi=dpi,
            pixel_format=types.PIXEL_FORMAT_BGR8,
        )
        for msg in stub.Render(render_req, timeout=600):
            r = msg.raster
            out["rasters"].append(
                {
                    "index": r.page_index,
                    "width": r.width_px,
                    "height": r.height_px,
                    "stride": r.stride_bytes,
                    "format": types.PixelFormat.Name(r.pixel_format),
                    "pixels": r.pixels,
                }
            )
    return out


def normalize_text(items: list[dict]) -> str:
    return re.sub(r"\s+", " ", " ".join(i["text"] for i in items)).strip()


def strip_subset(name: str) -> str:
    return re.sub(r"^[A-Z]{6}\+", "", name)


def read_ppm(path: Path):
    import numpy as np

    raw = path.read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", raw)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    pixels = np.frombuffer(raw[m.end() :], dtype=np.uint8)
    return pixels[: w * h * 3].reshape(h, w, 3)


def raster_metrics(ppm_path: Path, raster: dict) -> dict:
    import numpy as np

    ref = read_ppm(ppm_path)
    if ref is None:
        return {"status": "no-poppler-raster"}
    stride, w, h = raster["stride"], raster["width"], raster["height"]
    buf = np.frombuffer(raster["pixels"], dtype=np.uint8).reshape(h, stride)
    fmt = raster.get("format", "PIXEL_FORMAT_BGR8")
    if fmt == "PIXEL_FORMAT_BGR8":
        rgb = buf[:, : w * 3].reshape(h, w, 3)[:, :, ::-1]
    elif fmt == "PIXEL_FORMAT_RGB8":
        rgb = buf[:, : w * 3].reshape(h, w, 3)
    elif fmt == "PIXEL_FORMAT_RGBA8":
        rgb = buf[:, : w * 4].reshape(h, w, 4)[:, :, :3]
    elif fmt == "PIXEL_FORMAT_BGRA8":
        rgb = buf[:, : w * 4].reshape(h, w, 4)[:, :, 2::-1]
    elif fmt == "PIXEL_FORMAT_GRAY8":
        rgb = np.repeat(buf[:, :w].reshape(h, w, 1), 3, axis=2)
    else:
        return {"status": f"unhandled pixel format {fmt}"}
    ch = min(ref.shape[0], h)
    cw = min(ref.shape[1], w)
    a = ref[:ch, :cw].astype(np.int16)
    b = rgb[:ch, :cw].astype(np.int16)
    diff = np.abs(a - b)
    # Renderers can disagree by a pixel on where a rotation or scale lands
    # content; the aligned metric takes the best diff over one-pixel shifts
    # so a rounding offset reads differently from real content divergence.
    aligned = float(diff.mean())
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            sa = a[max(0, dy) : ch + min(0, dy), max(0, dx) : cw + min(0, dx)]
            sb = b[max(0, -dy) : ch + min(0, -dy), max(0, -dx) : cw + min(0, -dx)]
            if sa.size:
                aligned = min(aligned, float(np.abs(sa - sb).mean()))
    return {
        "status": "compared",
        "poppler_px": [int(ref.shape[1]), int(ref.shape[0])],
        "service_px": [int(w), int(h)],
        "mean_abs_diff": float(diff.mean()),
        "aligned_mean_abs_diff": aligned,
        "pct_pixels_off_gt16": float((diff.max(axis=2) > 16).mean() * 100.0),
    }


def compare_doc(doc_id: str, pop: dict, svc_out: dict, raster_dir: Path) -> dict:
    result: dict = {"id": doc_id}

    pop_loaded = bool(pop.get("load_ok"))
    service_loaded = svc_out.get("load_status") == "LOAD_STATUS_OK"
    result["load"] = {
        "poppler": "ok" if pop_loaded else "failed",
        "service": svc_out.get("load_status"),
        "verdict": "AGREE" if pop_loaded == service_loaded else "DIVERGE",
    }
    if not (pop_loaded and service_loaded):
        return result

    pp, fp = pop["pages"], svc_out["pages"]
    dims_ok = len(pp) == len(fp)
    rot_notes = []
    for a, b in zip(pp, fp):
        if abs(a["width_pts"] - b["width_pts"]) > max(1.0, 0.01 * a["width_pts"]):
            dims_ok = False
        if abs(a["height_pts"] - b["height_pts"]) > max(1.0, 0.01 * a["height_pts"]):
            dims_ok = False
        quarter = b["rotation"] % 180 != 0
        if a["quarter_turn"] != quarter:
            rot_notes.append(f"page {a['index']}: poppler quarter_turn={a['quarter_turn']} service rotation={b['rotation']}")
    result["inventory"] = {
        "poppler_pages": len(pp),
        "service_pages": len(fp),
        "verdict": "AGREE" if dims_ok and not rot_notes else "DIVERGE",
        "rotation_notes": rot_notes,
    }

    ratios = []
    bag_ratios = []
    cells = [0, 0]
    for a, b in zip(pp, fp):
        ta, tb = normalize_text(a["text"]), normalize_text(b["text"])
        cells[0] += len(a["text"])
        cells[1] += len(b["text"])
        if ta or tb:
            ratios.append(difflib.SequenceMatcher(None, ta, tb).ratio())
            # Order-insensitive character-bag similarity: separates "the
            # same text in a different reading order" (bag stays high) from
            # "text is missing" (bag drops too).
            from collections import Counter

            ca, cb = Counter(ta), Counter(tb)
            total = sum(ca.values()) + sum(cb.values())
            bag_ratios.append(2 * sum((ca & cb).values()) / total if total else 1.0)
    text_ratio = min(ratios) if ratios else 1.0
    bag_ratio = min(bag_ratios) if bag_ratios else 1.0
    if text_ratio >= 0.98:
        verdict = "AGREE"
    elif bag_ratio >= 0.98:
        verdict = "REORDERED"
    elif text_ratio >= 0.90:
        verdict = "CLOSE"
    else:
        verdict = "DIVERGE"
    result["text"] = {
        "poppler_boxes": cells[0],
        "service_cells": cells[1],
        "min_page_similarity": round(text_ratio, 4),
        "mean_page_similarity": round(sum(ratios) / len(ratios), 4) if ratios else 1.0,
        "min_page_bag_similarity": round(bag_ratio, 4),
        "verdict": verdict,
    }

    pop_fonts = {strip_subset(t.get("font_name", "")) for p in pp for t in p["text"] if t.get("font_name")}
    service_fonts = {strip_subset(n) for n in svc_out["fonts"]}
    union = pop_fonts | service_fonts
    jaccard = len(pop_fonts & service_fonts) / len(union) if union else 1.0
    result["fonts"] = {
        "poppler": sorted(pop_fonts),
        "service": sorted(service_fonts),
        "jaccard": round(jaccard, 3),
        "verdict": "AGREE" if jaccard >= 0.99 else ("CLOSE" if jaccard >= 0.5 else "DIVERGE"),
    }

    rmetrics = []
    for raster in svc_out["rasters"]:
        ppm = raster_dir / f"page-{raster['index']}.ppm"
        rmetrics.append(raster_metrics(ppm, raster) if ppm.exists() else {"status": "no-poppler-raster"})
    compared = [m for m in rmetrics if m.get("status") == "compared"]
    worst = max((m["pct_pixels_off_gt16"] for m in compared), default=None)
    result["raster"] = {
        "pages_compared": len(compared),
        "worst_pct_pixels_off_gt16": worst,
        "mean_abs_diff_max": max((m["mean_abs_diff"] for m in compared), default=None),
        "aligned_mean_abs_diff_max": max(
            (m["aligned_mean_abs_diff"] for m in compared), default=None
        ),
        "per_page": rmetrics,
        "verdict": "QUANTIFIED" if compared else "SKIP",
    }
    return result


def _fmt(v) -> str:
    return f"{v:.2f}" if isinstance(v, float) else str(v) if v is not None else "-"


def corpus_pdfs() -> list[tuple[str, Path]]:
    manifest = json.loads((REPO / "eval/scorecard/corpus.json").read_text())
    root = REPO / manifest["corpus_root"]
    docs = []
    external = os.environ.get("EVAL_EXTERNAL_CORPUS")
    for entry in manifest["documents"]:
        if entry.get("format") != "pdf":
            continue
        if "path" in entry:
            docs.append((entry["id"], root / entry["path"]))
        elif "external_path" in entry and external:
            docs.append((entry["id"], Path(external) / Path(entry["external_path"]).name))
    return [(i, p) for i, p in docs if p.exists()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default=os.environ.get("PDF_DIFF_TARGET", "localhost:50051"))
    parser.add_argument("--dpi", type=float, default=72.0)
    args = parser.parse_args()

    import grpc

    gen_dir = stage_stubs()
    svc, svc_grpc, types = load_stubs(gen_dir)
    channel = grpc.insecure_channel(
        args.target,
        options=[
            ("grpc.max_receive_message_length", 520 * 1024 * 1024),
            ("grpc.max_send_message_length", 520 * 1024 * 1024),
        ],
    )
    stub = svc_grpc.PdfBackendServiceStub(channel)

    probe = stub.Probe(
        svc.ProbeRequest(document=types.PdfDocument(data=b"%PDF-1.4\n")), timeout=30
    )
    backend = probe.capabilities.backend_name
    engine = probe.capabilities.engine_version

    results = []
    with tempfile.TemporaryDirectory(prefix="pdf-diff-") as tmp:
        for doc_id, path in corpus_pdfs():
            raster_dir = Path(tmp) / doc_id
            pop = poppler_leg(path, args.dpi, raster_dir)
            svc_out = service_leg(stub, svc, types, path, args.dpi)
            results.append(compare_doc(doc_id, pop, svc_out, raster_dir))
            print(f"  {doc_id}: load={results[-1]['load']['verdict']}", flush=True)

    # Non-PDF load-status cases.
    load_cases = []
    if FAILED_DOCS_DIR.is_dir():
        for path in sorted(FAILED_DOCS_DIR.iterdir()):
            if not path.is_file():
                continue
            resp = stub.Probe(
                svc.ProbeRequest(document=types.PdfDocument(data=path.read_bytes())),
                timeout=60,
            )
            with tempfile.TemporaryDirectory() as tmp:
                pop = poppler_leg(path, args.dpi, Path(tmp))
            load_cases.append(
                {
                    "file": path.name,
                    "poppler": "ok" if pop.get("load_ok") else "failed",
                    "service": types.LoadStatus.Name(resp.capabilities.load_status),
                    "verdict": "AGREE"
                    if bool(pop.get("load_ok"))
                    == (resp.capabilities.load_status == types.LOAD_STATUS_OK)
                    else "DIVERGE",
                }
            )

    OUT.mkdir(exist_ok=True)
    import importlib.metadata

    meta = {
        "backend": backend,
        "engine": engine,
        "target": args.target,
        "dpi": args.dpi,
        "poppler_leg": subprocess.run(
            ["pkg-config", "--modversion", "poppler-cpp"], capture_output=True, text=True
        ).stdout.strip(),
        "grpcio": importlib.metadata.version("grpcio"),
    }
    (OUT / "metrics.json").write_text(
        json.dumps({"meta": meta, "documents": results, "load_cases": load_cases}, indent=2)
    )

    lines = [
        "# PDF backend differential",
        "",
        f"Backend: `{backend}` ({engine}) at `{args.target}`; poppler leg: "
        f"poppler-cpp {meta['poppler_leg']} (host build; the in-process gRParse "
        f"image vendors its own poppler, so treat raster deltas as indicative). "
        f"DPI {args.dpi}.",
        "",
        "| doc | load | inventory | text sim (min/mean/bag) | cells (pop/service) | fonts | raster worst %px>16 | raster mean abs (raw/aligned) |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        inv = r.get("inventory", {})
        txt = r.get("text", {})
        fon = r.get("fonts", {})
        ras = r.get("raster", {})
        lines.append(
            f"| {r['id']} | {r['load']['verdict']} | {inv.get('verdict', '-')} "
            f"| {txt.get('min_page_similarity', '-')}/{txt.get('mean_page_similarity', '-')}/{txt.get('min_page_bag_similarity', '-')} ({txt.get('verdict', '-')}) "
            f"| {txt.get('poppler_boxes', '-')}/{txt.get('service_cells', '-')} "
            f"| {fon.get('jaccard', '-')} ({fon.get('verdict', '-')}) "
            f"| {_fmt(ras.get('worst_pct_pixels_off_gt16'))} "
            f"| {_fmt(ras.get('mean_abs_diff_max'))}/{_fmt(ras.get('aligned_mean_abs_diff_max'))} |"
        )
    if load_cases:
        lines += [
            "",
            "## Load-status cases (~/parser-failed-docs)",
            "",
            "| file | poppler | service | verdict |",
            "|---|---|---|---|",
        ]
        for c in load_cases:
            lines.append(f"| {c['file']} | {c['poppler']} | {c['service']} | {c['verdict']} |")
    (OUT / "report.md").write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT / 'report.md'} and metrics.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
