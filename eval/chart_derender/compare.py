#!/usr/bin/env python3
"""Measure chart derendering through grpc-enrich, per VLM endpoint, against truth.

    uv run --with grpcio --with grpcio-tools python eval/chart_derender/compare.py \\
        --enrich <host:port> [--endpoints URL ...] [--repeats 3] [--out eval/chart_derender/out]

Sends each chart image to EnrichService.EnrichDocument with
do_chart_extraction and a per-request vlm_endpoint, the same wire shape
gRParse's chart derender leg uses (options, one ItemImage, the completing
DocumentChunk), and scores the ChartTable that comes back:

- cells: share of truth cells (header row plus data rows) whose text matches
  at the same grid position, after the best small row/column shift, so a
  table that dropped or added a header row is judged on its body;
- numeric: share of truth numbers matched within 2 percent (or 0.5
  absolute) on the row whose category label matches, positional otherwise;
- title: exact, contains, or none against the source title (an untitled
  chart matches an empty title);
- latency: median and range over the repeats;
- stable: whether the cell grid, and separately the title, came back
  identical across the repeats.

Inputs are the matplotlib renders of the scorecard chart fixtures
(``renders/*.png`` with ``renders/truth.json``, both written by
``eval/scorecard/fixtures/xlsx_charts.py``) and the corpus raster
``tests/golden/corpus/bar_chart.png``, an axis-less five-bar chart with no
numbers to read; for it the score is the row count and the rank agreement
of the extracted values with the bar heights measured off the pixels.

The enrich service is reached by address; in the compose stack it is
``parse-stack-enrich-1`` on the ``parse-stack_default`` network (port
50056), whose container IP ``docker inspect`` prints. Nothing here changes
any default: the numbers are evidence for the endpoint decision.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import statistics
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
if str(REPO / "eval") not in sys.path:
    sys.path.insert(0, str(REPO / "eval"))

DEFAULT_ENDPOINTS = (
    "http://krick-1.taild24b1c.ts.net:8085",  # Qwen2.5-VL, the stack default
    "http://krick-1.taild24b1c.ts.net:8086",  # North Micro Vision
)
CORPUS_BAR_CHART = REPO / "tests" / "golden" / "corpus" / "bar_chart.png"
# bar_chart.png: five bars, baseline at y=260, tops at y=140, 80, 170, 48,
# 110 in a 400x300 raster, so the heights in pixels are these. No axis, no
# numbers, no title: only the ranking can be checked.
CORPUS_BAR_HEIGHTS = (120.0, 180.0, 90.0, 212.0, 150.0)
NUMERIC_REL = 0.02
NUMERIC_ABS = 0.5
SHIFTS = tuple((dr, dc) for dr in (-1, 0, 1) for dc in (-1, 0, 1))


def normalize(text: str | None) -> str:
    folded = " ".join((text or "").strip().lower().split())
    return folded.rstrip(".").strip()


def as_number(text: str | None) -> float | None:
    if text is None:
        return None
    cleaned = re.sub(r"[,%$\s]", "", str(text))
    try:
        return float(cleaned)
    except ValueError:
        return None


def same_text(truth: str, live: str | None) -> bool:
    a, b = normalize(truth), normalize(live)
    if a == b:
        return True
    ta, tb = as_number(a), as_number(b)
    return ta is not None and tb is not None and abs(ta - tb) <= max(NUMERIC_ABS, NUMERIC_REL * abs(ta))


@dataclass
class Extracted:
    title: str = ""
    model: str = ""
    cells: dict[tuple[int, int], str] = field(default_factory=dict)
    csv: str = ""
    skipped: str = ""
    latency_ms: float = 0.0

    @property
    def rows(self) -> int:
        return max((r + 1 for r, _ in self.cells), default=0)

    @property
    def cols(self) -> int:
        return max((c + 1 for _, c in self.cells), default=0)

    def grid_key(self) -> str:
        return json.dumps(sorted((r, c, normalize(t)) for (r, c), t in self.cells.items()))


def truth_grid(spec: dict[str, Any]) -> dict[tuple[int, int], str]:
    grid: dict[tuple[int, int], str] = {}
    for c, text in enumerate(spec["header"]):
        grid[(0, c)] = str(text)
    for r, row in enumerate(spec["rows"], start=1):
        for c, value in enumerate(row):
            grid[(r, c)] = str(value)
    return grid


def score_cells(spec: dict[str, Any], live: Extracted) -> tuple[float, tuple[int, int]]:
    truth = truth_grid(spec)
    best, best_shift = 0.0, (0, 0)
    for shift in SHIFTS:
        hits = sum(1 for (r, c), text in truth.items() if same_text(text, live.cells.get((r + shift[0], c + shift[1]))))
        share = hits / len(truth)
        if share > best:
            best, best_shift = share, shift
    return best, best_shift


def score_numeric(spec: dict[str, Any], live: Extracted, shift: tuple[int, int]) -> float:
    total = matched = 0
    live_rows: dict[str, int] = {}
    for (r, c), text in live.cells.items():
        if c == max(0, shift[1]) and normalize(text) not in live_rows:
            live_rows[normalize(text)] = r
    for r, row in enumerate(spec["rows"], start=1):
        label = normalize(str(row[0]))
        live_row = live_rows.get(label, r + shift[0])
        for c, value in enumerate(row[1:], start=1):
            total += 1
            got = as_number(live.cells.get((live_row, c + shift[1])))
            if got is not None and abs(got - float(value)) <= max(NUMERIC_ABS, NUMERIC_REL * abs(float(value))):
                matched += 1
    return matched / total if total else 0.0


def score_title(truth_title: str, live_title: str) -> str:
    a, b = normalize(truth_title), normalize(live_title)
    if a == b:
        return "exact"
    if a and b and (a in b or b in a):
        return "contains"
    return "none"


def spearman(xs: list[float], ys: list[float]) -> float | None:
    if len(xs) != len(ys) or len(xs) < 2:
        return None

    def ranks(values: list[float]) -> list[float]:
        order = sorted(range(len(values)), key=lambda i: values[i])
        out = [0.0] * len(values)
        for rank, index in enumerate(order, start=1):
            out[index] = float(rank)
        return out

    rx, ry = ranks(xs), ranks(ys)
    n = len(xs)
    d2 = sum((a - b) ** 2 for a, b in zip(rx, ry))
    return 1 - 6 * d2 / (n * (n * n - 1))


def first_numeric_column(live: Extracted) -> list[float]:
    """The values of the first column that holds numbers in every data row."""
    rows = live.rows
    for c in range(live.cols):
        values = [as_number(live.cells.get((r, c))) for r in range(rows)]
        numbers = [v for v in values if v is not None]
        if len(numbers) >= 2 and len(numbers) >= rows - 1:
            return numbers
    return []


def score_corpus_bar_chart(live: Extracted) -> dict[str, Any]:
    values = first_numeric_column(live)
    rho = spearman(values, list(CORPUS_BAR_HEIGHTS)) if len(values) == len(CORPUS_BAR_HEIGHTS) else None
    return {"rows_match": len(values) == len(CORPUS_BAR_HEIGHTS), "values": values, "rank_rho": rho}


def stage_and_load(staged: Path):
    """Generate Python stubs for document.proto and the vendored enrich contract."""
    from compare_vlm import stage_protos  # noqa: WPS433

    stage_protos(staged)
    enrich_dir = staged / "ai/pipestream/enrich/v1"
    enrich_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO / "collectors" / "enrich_service.proto", enrich_dir / "enrich_service.proto")
    import grpc_tools
    from grpc_tools import protoc

    include = Path(grpc_tools.__file__).parent / "_proto"
    out = staged / "gen"
    out.mkdir(exist_ok=True)
    args = ["protoc", f"-I{staged}", f"-I{include}", f"--python_out={out}", f"--grpc_python_out={out}",
            str(staged / "ai/pipestream/document/v1/document.proto"),
            str(enrich_dir / "enrich_service.proto")]
    if protoc.main(args) != 0:
        raise RuntimeError("protoc failed")
    sys.path.insert(0, str(out))
    from ai.pipestream.document.v1 import document_pb2
    from ai.pipestream.enrich.v1 import enrich_service_pb2, enrich_service_pb2_grpc

    return document_pb2, enrich_service_pb2, enrich_service_pb2_grpc


def chart_document(document_pb2, kind: str):
    document = document_pb2.Document()
    document.schema_name = "docling_document_v2"
    document.body.self_ref = "#/body"
    document.furniture.self_ref = "#/furniture"
    picture = document.pictures.add()
    picture.self_ref = "#/pictures/0"
    picture.parent.ref = "#/body"
    picture.label = document_pb2.DOC_ITEM_LABEL_PICTURE
    classification = picture.annotations.add().classification
    classification.kind = "classification"
    classification.provenance = "figure-classifier"
    top = classification.predicted_classes.add()
    top.class_name = f"{kind}_chart"
    top.confidence = 0.9
    document.body.children.add().ref = "#/pictures/0"
    return document


def enrich_once(stub, document_pb2, enrich_pb2, image: Path, kind: str, endpoint: str, timeout: float) -> Extracted:
    def frames():
        options = enrich_pb2.EnrichDocumentRequest()
        options.options.do_chart_extraction = True
        options.options.vlm_endpoint = endpoint
        options.options.timeout_seconds = int(timeout)
        yield options
        crop = enrich_pb2.EnrichDocumentRequest()
        crop.image.self_ref = "#/pictures/0"
        crop.image.mimetype = "image/png"
        crop.image.data = image.read_bytes()
        yield crop
        chunk = enrich_pb2.EnrichDocumentRequest()
        chunk.chunk.data = chart_document(document_pb2, kind).SerializeToString()
        chunk.chunk.complete = True
        yield chunk

    result = Extracted()
    started = time.monotonic()
    try:
        for event in stub.EnrichDocument(frames(), timeout=timeout + 30):
            if event.HasField("annotation") and event.annotation.HasField("chart_table"):
                table = event.annotation.chart_table
                result.title = table.title
                result.model = event.annotation.model
                result.csv = table.csv
                for cell in table.table.table_cells:
                    result.cells.setdefault((cell.start_row_offset_idx, cell.start_col_offset_idx), cell.text)
                if not result.cells:
                    for r, row in enumerate(table.table.grid):
                        for c, cell in enumerate(row.cells):
                            result.cells.setdefault((r, c), cell.text)
            elif event.HasField("skipped"):
                result.skipped = f"{enrich_pb2.SkipReason.Name(event.skipped.reason)}: {event.skipped.detail}"
    except Exception as error:  # noqa: BLE001
        result.skipped = f"rpc: {error}"
    result.latency_ms = (time.monotonic() - started) * 1000.0
    return result


def load_inputs(renders: Path) -> list[dict[str, Any]]:
    truth = json.loads((renders / "truth.json").read_text())
    inputs = [{"image": renders / spec["image"], "name": spec["image"], "kind": spec["kind"], "spec": spec}
              for spec in truth.values()]
    inputs.sort(key=lambda entry: entry["name"])
    if CORPUS_BAR_CHART.is_file():
        inputs.append({"image": CORPUS_BAR_CHART, "name": "bar_chart.png (corpus)", "kind": "bar", "spec": None})
    return inputs


def run(args: argparse.Namespace) -> dict[str, Any]:
    import grpc

    with tempfile.TemporaryDirectory(prefix="chart-derender-") as staged:
        document_pb2, enrich_pb2, enrich_grpc = stage_and_load(Path(staged))
        channel = grpc.insecure_channel(args.enrich, options=[("grpc.max_receive_message_length", 64 * 1024 * 1024)])
        grpc.channel_ready_future(channel).result(timeout=10)
        stub = enrich_grpc.EnrichServiceStub(channel)
        rows: list[dict[str, Any]] = []
        for entry in load_inputs(args.renders):
            for endpoint in args.endpoints:
                runs = [enrich_once(stub, document_pb2, enrich_pb2, entry["image"], entry["kind"], endpoint, args.timeout)
                        for _ in range(args.repeats)]
                rows.append(summarize_runs(entry, endpoint, runs))
                print(format_row(rows[-1]), file=sys.stderr)
        return {"enrich": args.enrich, "repeats": args.repeats, "rows": rows}


def summarize_runs(entry: dict[str, Any], endpoint: str, runs: list[Extracted]) -> dict[str, Any]:
    answered = [r for r in runs if r.cells]
    row: dict[str, Any] = {
        "image": entry["name"], "endpoint": endpoint, "answered": len(answered), "repeats": len(runs),
        "model": next((r.model for r in answered), ""),
        "latency_ms": {"median": round(statistics.median(r.latency_ms for r in runs), 0),
                       "min": round(min(r.latency_ms for r in runs), 0), "max": round(max(r.latency_ms for r in runs), 0)},
        "grid_stable": len({r.grid_key() for r in runs}) == 1,
        "title_stable": len({normalize(r.title) for r in runs}) == 1,
        "skips": [r.skipped for r in runs if r.skipped],
        "titles": [r.title for r in runs],
        "shapes": [f"{r.rows}x{r.cols}" for r in runs],
        "first_csv": next((r.csv for r in answered), ""),
    }
    spec = entry["spec"]
    if spec is not None:
        scores = []
        for r in runs:
            cells, shift = score_cells(spec, r)
            scores.append({"cells": cells, "numeric": score_numeric(spec, r, shift), "title": score_title(spec["title"], r.title)})
        row["cells"] = round(statistics.mean(s["cells"] for s in scores), 3)
        row["numeric"] = round(statistics.mean(s["numeric"] for s in scores), 3)
        row["title"] = "/".join(s["title"] for s in scores)
        row["per_run"] = scores
    else:
        ranks = [score_corpus_bar_chart(r) for r in runs]
        row["rows_match"] = sum(1 for k in ranks if k["rows_match"])
        rhos = [k["rank_rho"] for k in ranks if k["rank_rho"] is not None]
        row["rank_rho"] = round(statistics.mean(rhos), 3) if rhos else None
        row["per_run"] = ranks
    return row


def format_row(row: dict[str, Any]) -> str:
    latency = row["latency_ms"]
    stable = f"grid {'yes' if row['grid_stable'] else 'no'}, title {'yes' if row['title_stable'] else 'no'}"
    if "cells" in row:
        score = f"{row['cells']:.2f} | {row['numeric']:.2f} | {row['title']}"
    else:
        rho = "n/a" if row.get("rank_rho") is None else f"{row['rank_rho']:.2f}"
        score = f"rows {row['rows_match']}/{row['repeats']} | rank rho {rho} | n/a"
    return (f"| {row['image']} | {row['endpoint']} | {row['model'] or '-'} | {row['answered']}/{row['repeats']} | {score} | "
            f"{latency['median']:.0f} ms ({latency['min']:.0f}..{latency['max']:.0f}) | {stable} |")


def render_table(report: dict[str, Any]) -> str:
    lines = ["| image | endpoint | model | answered | cells | numeric | title | latency | stable |",
             "|---|---|---|---|---|---|---|---|---|"]
    lines.extend(format_row(row) for row in report["rows"])
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--enrich", required=True, help="grpc-enrich host:port (plaintext)")
    parser.add_argument("--endpoints", nargs="+", default=list(DEFAULT_ENDPOINTS), help="VLM endpoints to compare")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=180.0, help="per-call VLM timeout in seconds")
    parser.add_argument("--renders", type=Path, default=HERE / "renders")
    parser.add_argument("--out", type=Path, default=HERE / "out")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = run(args)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "report.json").write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    table = render_table(report)
    (args.out / "report.md").write_text(table + "\n")
    print(table)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
