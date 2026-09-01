#!/usr/bin/env python3
"""Score a running gRParse against the committed scorecard baseline and truth.

    uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py            # score
    uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record   # re-record baseline
    uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record-floors --only <id>
                                                    # record truth floors, latency and stability only

Environment: GRPARSE_TARGET (default localhost:50051), EVAL_LABEL (default
"live"), EVAL_OUT (default eval/out), EVAL_EXTERNAL_CORPUS (directory that
overrides where external corpus files are looked up), EVAL_LATENCY=off
(disable the latency budget; a record then leaves the budget unchanged),
EVAL_REQUIRE=1 (a skip is a failure),
EVAL_METRICS_URL (Prometheus endpoint for the informational memory line).

Exit codes: 0 every scored document is within tolerance; 1 at least one
regression, or a skip under --require; 77 (the CTest skip code) when gRParse
is unreachable, grpcio is not importable, or the corpus resolves to nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.changes import document_changes  # noqa: E402
from scorecard.corpus import DEFAULT_MANIFEST, REPO, CorpusDocument, load_manifest, select  # noqa: E402
from scorecard.gates import (  # noqa: E402
    GateChange,
    latency_result,
    median_ms,
    ratchet,
    stability_result,
    truth_results,
)
from scorecard.memory import memory_note, metrics_url, sample_rss  # noqa: E402
from scorecard.meta import merge_meta  # noqa: E402
from scorecard.report import write_report  # noqa: E402
from scorecard.scoring import PASS, REGRESS, DocScore, MetricResult, score_document  # noqa: E402
from scorecard.stability import stability  # noqa: E402
from scorecard.summary import summarize  # noqa: E402
from scorecard.truth_metrics import score_truth  # noqa: E402

SKIP = 77
BASELINE_DIR = Path(__file__).resolve().parent / "baseline"
TRUTH_DIR = Path(__file__).resolve().parent / "truth"


def env_flag(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("0", "off", "false", "no", "")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", action="store_true", help="write baselines (and gates) instead of scoring")
    parser.add_argument("--record-floors", action="store_true",
                        help="write only the truth floors, latency budget and stability rows (summaries untouched)")
    parser.add_argument("--target", default=os.environ.get("GRPARSE_TARGET", "localhost:50051"))
    parser.add_argument("--label", default=os.environ.get("EVAL_LABEL", "live"))
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline-dir", type=Path, default=BASELINE_DIR)
    parser.add_argument("--truth-dir", type=Path, default=TRUTH_DIR)
    parser.add_argument("--out", type=Path, default=Path(os.environ.get("EVAL_OUT", REPO / "eval" / "out")))
    parser.add_argument("--only", nargs="*", default=None, help="document ids or formats to run")
    parser.add_argument("--reason", default="", help="why the baseline or a floor moved (kept in meta history)")
    parser.add_argument("--repeat", type=int, default=1, help="convert each document N times (stability, median latency)")
    parser.add_argument("--require", action="store_true", default=env_flag("EVAL_REQUIRE", False),
                        help="pre-release mode: any skip (missing file, unreachable service) is a failure")
    parser.add_argument("--no-latency", action="store_true", default=not env_flag("EVAL_LATENCY", True),
                        help="do not gate on latency (also EVAL_LATENCY=off)")
    args = parser.parse_args(argv)
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")
    return args


@dataclass
class Conversion:
    """Everything one document produced across --repeat conversions."""

    summary: dict[str, Any]
    markdown: str
    elapsed_ms: list[float] = field(default_factory=list)
    timings: dict[str, float] = field(default_factory=dict)
    stable: bool | None = None
    first_diff: str | None = None

    @property
    def latency_ms(self) -> float | None:
        return median_ms(self.elapsed_ms)


def convert(client: Any, doc: CorpusDocument, repeat: int) -> Conversion:
    summaries: list[dict[str, Any]] = []
    elapsed: list[float] = []
    markdown = ""
    timings: dict[str, float] = {}
    for _ in range(repeat):
        result = client.convert(doc.path)
        summaries.append(summarize(result.document, result.markdown, doc_id=doc.doc_id, fmt=doc.format,
                                   content_type=doc.content_type, status=result.status, errors=result.errors,
                                   rpc_error=result.rpc_error))
        elapsed.append(round(result.elapsed_ms, 1))
        if not markdown:
            markdown, timings = result.markdown, result.timings
    stable, first_diff = stability(summaries)
    return Conversion(summary=summaries[0], markdown=markdown, elapsed_ms=elapsed, timings=timings,
                      stable=stable, first_diff=first_diff)


def load_json(path: Path) -> dict[str, Any] | None:
    return json.loads(path.read_text()) if path.is_file() else None


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=1, sort_keys=True, ensure_ascii=False) + "\n")


class Runner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.out = args.out / "scorecard" / args.label
        self.recording = args.record or args.record_floors
        self.gates: dict[str, Any] = dict((load_json(args.baseline_dir / "_meta.json") or {}).get("gates") or {})
        self.new_gates: dict[str, Any] = {}
        self.gate_changes: list[GateChange] = []
        self.entries: list[dict[str, Any]] = []
        self.totals = {"scored": 0, "passed": 0, "regressed": 0, "skipped": 0, "no_baseline": 0, "recorded": 0}
        self.notes: list[str] = []
        self.required_missing: list[str] = []

    def truth_for(self, doc_id: str) -> dict[str, Any] | None:
        return load_json(self.args.truth_dir / f"{doc_id}.json")

    def score(self, doc: CorpusDocument, conversion: Conversion) -> tuple[DocScore | None, dict[str, Any]]:
        """Baseline metrics plus truth, latency and stability results; the truth block for the report."""
        baseline = load_json(self.args.baseline_dir / f"{doc.doc_id}.json")
        gate_row = self.gates.get(doc.doc_id) or {}
        truth = self.truth_for(doc.doc_id)
        truth_block: dict[str, Any] = {}
        extra: list[MetricResult] = []
        if truth is not None:
            scores = score_truth(truth, conversion.summary)
            truth_block = {"scores": scores.values(), "floors": dict(gate_row.get("truth") or {}),
                           "details": scores.details()}
            extra.extend(truth_results(scores.values(), gate_row.get("truth"), scores.details()))
        extra.append(latency_result(gate_row.get("latency_ms"), conversion.latency_ms, enabled=not self.args.no_latency))
        extra.append(stability_result(gate_row.get("stable"), conversion.stable, conversion.first_diff))
        if baseline is None:
            return None, truth_block | {"extra": [m.__dict__ for m in extra]}
        score = score_document(baseline, conversion.summary)
        score.metrics.extend(extra)
        score.verdict = REGRESS if any(m.verdict == REGRESS for m in score.metrics) else PASS
        return score, truth_block

    def record_gates(self, doc: CorpusDocument, conversion: Conversion) -> None:
        truth = self.truth_for(doc.doc_id)
        scores = score_truth(truth, conversion.summary).values() if truth is not None else {}
        # A run with the latency gate off (a CPU instance) measures a latency
        # that is no budget for anyone; it leaves the recorded budget alone.
        latency_ms = None if self.args.no_latency else conversion.latency_ms
        row, changes = ratchet(doc.doc_id, self.gates.get(doc.doc_id), scores, latency_ms=latency_ms,
                               stable=conversion.stable, reason=self.args.reason)
        self.new_gates[doc.doc_id] = row
        self.gate_changes.extend(changes)
        for change in changes:
            self.notes.append(f"gate {change.doc_id}.{change.metric}: {change.before} -> {change.after} ({change.reason})")

    def run_document(self, client: Any, doc: CorpusDocument) -> None:
        entry: dict[str, Any] = {"doc_id": doc.doc_id, "format": doc.format, "path": str(doc.path), "external": doc.external}
        if not doc.present:
            entry.update({"status": "skipped", "reason": doc.skip_reason})
            self.totals["skipped"] += 1
            self.required_missing.append(f"{doc.doc_id}: {doc.skip_reason}")
            self.entries.append(entry)
            print(f"-- {doc.doc_id}: skipped ({doc.skip_reason})", file=sys.stderr)
            return
        conversion = convert(client, doc, self.args.repeat)
        write_json(self.out / "summaries" / f"{doc.doc_id}.json", conversion.summary)
        (self.out / "markdown").mkdir(parents=True, exist_ok=True)
        (self.out / "markdown" / f"{doc.doc_id}.md").write_text(conversion.markdown)
        entry.update({"summary": conversion.summary, "elapsed_ms": conversion.latency_ms,
                      "elapsed_ms_runs": conversion.elapsed_ms, "timings": conversion.timings,
                      "stable": conversion.stable, "first_diff": conversion.first_diff})
        shown_ms = f"{conversion.latency_ms:.0f} ms" if conversion.latency_ms is not None else "n/a"
        if self.recording:
            self.record_gates(doc, conversion)
        if self.args.record:
            write_json(self.args.baseline_dir / f"{doc.doc_id}.json", conversion.summary)
            entry["status"] = "recorded"
            self.totals["recorded"] += 1
            print(f"== {doc.doc_id}: recorded ({shown_ms}, {len(conversion.summary['warnings'])} warnings)", file=sys.stderr)
        else:
            score, truth_block = self.score(doc, conversion)
            entry["truth"] = truth_block
            if score is None:
                entry["status"] = "floors-recorded" if self.args.record_floors else "no-baseline"
                self.totals["no_baseline"] += 1
                print(f"?? {doc.doc_id}: no baseline ({shown_ms})", file=sys.stderr)
            else:
                baseline = load_json(self.args.baseline_dir / f"{doc.doc_id}.json") or {}
                entry.update({"status": "scored", "score": score.as_dict(),
                              "changes": document_changes(baseline, conversion.summary, score.count_delta)})
                self.totals["scored"] += 1
                self.totals["passed" if score.verdict == PASS else "regressed"] += 1
                failing = [m.name for m in score.metrics if m.verdict == REGRESS]
                tail = f" [{', '.join(failing)}]" if failing else ""
                print(f"== {doc.doc_id}: {score.verdict} ({shown_ms}){tail}", file=sys.stderr)
        self.entries.append(entry)

    def write_meta(self, info: Any) -> None:
        meta_path = self.args.baseline_dir / "_meta.json"
        existing = load_json(meta_path)
        recorded = [e["doc_id"] for e in self.entries if e["status"] == "recorded"]
        run_meta = {
            "service": f"{info.name} {info.version}", "target": info.target,
            "manifest": str(self.args.manifest.relative_to(REPO)) if self.args.manifest.is_relative_to(REPO) else str(self.args.manifest),
            "documents": recorded if self.args.record else list((existing or {}).get("documents", [])),
            "skipped": {e["doc_id"]: e["reason"] for e in self.entries if e["status"] == "skipped"},
            "reason": self.args.reason,
            "gates": self.new_gates,
            "gate_changes": [c.as_dict() for c in self.gate_changes],
        }
        if not self.args.record:
            run_meta["documents"] = list(self.new_gates)
            run_meta["reason"] = f"floors: {self.args.reason}" if self.args.reason else "floors"
        timestamp = datetime.now(UTC).replace(microsecond=0).isoformat()
        partial = bool(self.args.only) or not self.args.record
        write_json(meta_path, merge_meta(existing, run_meta, partial=partial, timestamp=timestamp))


def skip(reason: str, *, require: bool) -> int:
    if require:
        print(f"FAIL (--require): {reason}", file=sys.stderr)
        return 1
    print(f"skip: {reason}", file=sys.stderr)
    return SKIP


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        import grpc  # noqa: F401
        import grpc_tools  # noqa: F401
    except ImportError:
        return skip("grpcio and grpcio-tools are not importable (use uv run --with grpcio --with grpcio-tools)",
                    require=args.require)
    from scorecard.client import GrparseClient, Unreachable

    documents = select(load_manifest(args.manifest), args.only)
    if not documents:
        return skip(f"corpus is empty after selection ({args.manifest})", require=args.require)
    if not any(d.present for d in documents):
        return skip("no corpus document is present on disk", require=args.require)

    runner = Runner(args)
    if args.record and args.reason:
        runner.notes.append(f"baseline re-recorded: {args.reason}")
    if args.no_latency:
        runner.notes.append("latency budget disabled (EVAL_LATENCY=off)")
    started = time.monotonic()
    endpoint = metrics_url(args.target) if env_flag("EVAL_MEMORY", True) else None
    rss_before = sample_rss(endpoint)
    try:
        with GrparseClient(args.target) as client:
            info = client.service_info()
            for doc in documents:
                runner.run_document(client, doc)
    except Unreachable as error:
        return skip(f"gRParse unreachable: {error}", require=args.require)
    rss_after = sample_rss(endpoint)
    runner.notes.append(memory_note(rss_before, rss_after))
    wall = time.monotonic() - started

    if runner.recording:
        runner.write_meta(info)
    report = {
        "label": args.label, "mode": "record" if args.record else ("record-floors" if args.record_floors else "score"),
        "target": info.target, "service": f"{info.name} {info.version}", "baseline_dir": str(args.baseline_dir),
        "repeat": args.repeat, "require": args.require,
        "totals": runner.totals, "wall_seconds": round(wall, 2), "notes": runner.notes,
        "documents": [{k: v for k, v in e.items() if k != "summary"} | {"summary": _compact(e.get("summary"))}
                      for e in runner.entries],
    }
    print(write_report(runner.out, report))
    print(f"report: {runner.out / 'report.md'}", file=sys.stderr)
    if args.require and runner.required_missing:
        print("FAIL (--require): skipped documents: " + "; ".join(runner.required_missing), file=sys.stderr)
        return 1
    if not args.record and runner.totals["regressed"]:
        return 1
    return 0


def _compact(summary: dict[str, Any] | None) -> dict[str, Any] | None:
    """The report keeps counts, warnings and agreement; the full summary is in summaries/."""
    if summary is None:
        return None
    return {key: summary[key] for key in ("counts", "warnings", "agreement", "collectors", "status") if key in summary}


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
