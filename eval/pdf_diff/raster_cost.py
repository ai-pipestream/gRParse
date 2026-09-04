"""Raster-over-the-wire cost at model DPI.

For each scorecard PDF, times a full-document Render stream at the model
DPI against one or more PdfBackend targets and, as the in-process
reference, the poppler render alone (poppler_floor in render-only mode,
no text extraction, no image IO). Records wall time per document, pages,
and payload megabytes on the wire.

Run (backends listening)::

    uv run --with grpcio --with grpcio-tools python eval/pdf_diff/raster_cost.py \
        --dpi 200 --targets localhost:51241 localhost:51242 localhost:51243

Writes ``out/raster-cost.md`` and ``out/raster-cost.json``.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path

from runner import HERE, OUT, corpus_pdfs, load_stubs, stage_stubs


def poppler_reference(pdf: Path, dpi: float) -> float:
    floor = HERE / "poppler_floor"
    start = time.perf_counter()
    subprocess.run(
        [str(floor), str(pdf), str(dpi), "-", "render-only"],
        capture_output=True,
        timeout=1800,
        check=True,
    )
    return time.perf_counter() - start


def backend_render(stub, svc, types, pdf: Path, dpi: float) -> dict:
    data = pdf.read_bytes()
    request = svc.RenderRequest(
        document=types.PdfDocument(data=data),
        dpi=dpi,
        pixel_format=types.PIXEL_FORMAT_BGR8,
    )
    start = time.perf_counter()
    pages = 0
    payload = 0
    for msg in stub.Render(request, timeout=1800):
        pages += 1
        payload += len(msg.raster.pixels)
    elapsed = time.perf_counter() - start
    return {
        "seconds": round(elapsed, 4),
        "pages": pages,
        "payload_mb": round(payload / (1024 * 1024), 2),
        "request_mb": round(len(data) / (1024 * 1024), 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dpi", type=float, default=200.0)
    parser.add_argument("--targets", nargs="+", required=True)
    args = parser.parse_args()

    import grpc

    gen_dir = stage_stubs()
    svc, svc_grpc, types = load_stubs(gen_dir)

    backends = {}
    for target in args.targets:
        channel = grpc.insecure_channel(
            target,
            options=[("grpc.max_receive_message_length", 520 * 1024 * 1024)],
        )
        stub = svc_grpc.PdfBackendServiceStub(channel)
        probe = stub.Probe(
            svc.ProbeRequest(document=types.PdfDocument(data=b"%PDF-1.4\n")),
            timeout=30,
        )
        backends[probe.capabilities.backend_name] = (target, stub)

    rows = []
    for doc_id, path in corpus_pdfs():
        row = {"id": doc_id, "poppler_inprocess_s": round(poppler_reference(path, args.dpi), 4)}
        for name, (_, stub) in backends.items():
            try:
                row[name] = backend_render(stub, svc, types, path, args.dpi)
            except Exception as error:  # noqa: BLE001 - record, keep measuring
                row[name] = {"error": str(error)[:120]}
        rows.append(row)
        print(f"  {doc_id}: done", flush=True)

    OUT.mkdir(exist_ok=True)
    (OUT / "raster-cost.json").write_text(
        json.dumps({"dpi": args.dpi, "rows": rows}, indent=2)
    )

    names = sorted(backends)
    lines = [
        "# Raster-over-the-wire cost",
        "",
        f"Model DPI {args.dpi}; times are one full-document Render stream, "
        "wall clock, BGR8 requested; the reference column is the in-process "
        "poppler render alone (no text pass, no image IO). Payload is the "
        "raster bytes crossing the wire; every request also re-sends the "
        "document bytes (stateless contract).",
        "",
        "| doc | poppler in-process s | "
        + " | ".join(f"{n} s / pages / payload MB" for n in names)
        + " |",
        "|---" * (2 + len(names)) + "|",
    ]
    for row in rows:
        cells = [row["id"], f"{row['poppler_inprocess_s']:.2f}"]
        for name in names:
            data = row.get(name, {})
            if "error" in data:
                cells.append("error: " + data["error"])
            else:
                cells.append(
                    f"{data['seconds']:.2f} / {data['pages']} / {data['payload_mb']}"
                )
        lines.append("| " + " | ".join(cells) + " |")
    (OUT / "raster-cost.md").write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT / 'raster-cost.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
