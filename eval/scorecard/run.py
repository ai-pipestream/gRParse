#!/usr/bin/env python3
"""Score a running gRParse against the committed scorecard baseline.

    uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py            # score
    uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record   # re-record baseline

Environment: GRPARSE_TARGET (default localhost:50051), EVAL_LABEL (default
"live"), EVAL_OUT (default eval/out), EVAL_EXTERNAL_CORPUS (directory that
overrides where external corpus files are looked up).

Exit codes: 0 every scored document is within tolerance; 1 at least one
regression; 77 (the CTest skip code) when gRParse is unreachable, grpcio is
not importable, or the corpus resolves to nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.corpus import REPO, CorpusDocument, DEFAULT_MANIFEST, load_manifest, select  # noqa: E402
from scorecard.meta import merge_meta  # noqa: E402
from scorecard.report import write_report  # noqa: E402
from scorecard.scoring import PASS, REGRESS, score_document  # noqa: E402
from scorecard.summary import summarize  # noqa: E402

SKIP = 77
BASELINE_DIR = Path(__file__).resolve().parent / "baseline"
CTEST_NOTE = ("CTest wiring (add_test with LABELS eval and SKIP_RETURN_CODE 77, like vlm-oracle-eval) is left "
              "to the CMakeLists.txt owner; this runner is invoked directly until then.")


def skip(reason: str) -> int:
    print(f"skip: {reason}", file=sys.stderr)
    return SKIP


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", action="store_true", help="write baselines instead of scoring against them")
    parser.add_argument("--target", default=os.environ.get("GRPARSE_TARGET", "localhost:50051"))
    parser.add_argument("--label", default=os.environ.get("EVAL_LABEL", "live"))
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline-dir", type=Path, default=BASELINE_DIR)
    parser.add_argument("--out", type=Path, default=Path(os.environ.get("EVAL_OUT", REPO / "eval" / "out")))
    parser.add_argument("--only", nargs="*", default=None, help="document ids or formats to run")
    parser.add_argument("--reason", default="", help="why the baseline is being re-recorded (recorded in report notes)")
    return parser.parse_args(argv)


def convert_and_summarize(client: Any, doc: CorpusDocument) -> tuple[dict[str, Any], dict[str, Any], str]:
    result = client.convert(doc.path)
    summary = summarize(result.document, result.markdown, doc_id=doc.doc_id, fmt=doc.format,
                        content_type=doc.content_type, status=result.status, errors=result.errors,
                        rpc_error=result.rpc_error)
    timing = {"elapsed_ms": round(result.elapsed_ms, 1), "timings": result.timings}
    return summary, timing, result.markdown


def load_baseline(baseline_dir: Path, doc_id: str) -> dict[str, Any] | None:
    path = baseline_dir / f"{doc_id}.json"
    return json.loads(path.read_text()) if path.is_file() else None


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=1, sort_keys=True, ensure_ascii=False) + "\n")


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        import grpc  # noqa: F401
        import grpc_tools  # noqa: F401
    except ImportError:
        return skip("grpcio and grpcio-tools are not importable (use uv run --with grpcio --with grpcio-tools)")
    from scorecard.client import GrparseClient, Unreachable

    documents = select(load_manifest(args.manifest), args.only)
    if not documents:
        return skip(f"corpus is empty after selection ({args.manifest})")
    if not any(d.present for d in documents):
        return skip("no corpus document is present on disk")

    out = args.out / "scorecard" / args.label
    started = time.monotonic()
    entries: list[dict[str, Any]] = []
    totals = {"scored": 0, "passed": 0, "regressed": 0, "skipped": 0, "no_baseline": 0, "recorded": 0}
    notes = [CTEST_NOTE]
    if args.record and args.reason:
        notes.append(f"baseline re-recorded: {args.reason}")

    try:
        with GrparseClient(args.target) as client:
            info = client.service_info()
            for doc in documents:
                entry: dict[str, Any] = {"doc_id": doc.doc_id, "format": doc.format, "path": str(doc.path),
                                         "external": doc.external}
                if not doc.present:
                    entry.update({"status": "skipped", "reason": doc.skip_reason})
                    totals["skipped"] += 1
                    entries.append(entry)
                    print(f"-- {doc.doc_id}: skipped ({doc.skip_reason})", file=sys.stderr)
                    continue
                summary, timing, markdown = convert_and_summarize(client, doc)
                write_json(out / "summaries" / f"{doc.doc_id}.json", summary)
                (out / "markdown").mkdir(parents=True, exist_ok=True)
                (out / "markdown" / f"{doc.doc_id}.md").write_text(markdown)
                entry.update({"summary": summary, **timing})
                if args.record:
                    write_json(args.baseline_dir / f"{doc.doc_id}.json", summary)
                    entry["status"] = "recorded"
                    totals["recorded"] += 1
                    print(f"== {doc.doc_id}: recorded ({timing['elapsed_ms']} ms, {len(summary['warnings'])} warnings)",
                          file=sys.stderr)
                else:
                    baseline = load_baseline(args.baseline_dir, doc.doc_id)
                    if baseline is None:
                        entry["status"] = "no-baseline"
                        totals["no_baseline"] += 1
                        print(f"?? {doc.doc_id}: no baseline", file=sys.stderr)
                    else:
                        score = score_document(baseline, summary)
                        entry.update({"status": "scored", "score": score.as_dict()})
                        totals["scored"] += 1
                        totals["passed" if score.verdict == PASS else "regressed"] += 1
                        print(f"== {doc.doc_id}: {score.verdict} ({timing['elapsed_ms']} ms)", file=sys.stderr)
                entries.append(entry)
    except Unreachable as error:
        return skip(f"gRParse unreachable: {error}")

    wall = time.monotonic() - started
    if args.record:
        meta_path = args.baseline_dir / "_meta.json"
        existing = json.loads(meta_path.read_text()) if meta_path.is_file() else None
        run_meta = {
            "service": f"{info.name} {info.version}", "target": info.target,
            "manifest": str(args.manifest.relative_to(REPO)) if args.manifest.is_relative_to(REPO) else str(args.manifest),
            "documents": [e["doc_id"] for e in entries if e["status"] == "recorded"],
            "skipped": {e["doc_id"]: e["reason"] for e in entries if e["status"] == "skipped"},
            "reason": args.reason,
        }
        timestamp = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        write_json(meta_path, merge_meta(existing, run_meta, partial=bool(args.only), timestamp=timestamp))
    report = {
        "label": args.label, "mode": "record" if args.record else "score", "target": info.target,
        "service": f"{info.name} {info.version}", "baseline_dir": str(args.baseline_dir),
        "totals": totals, "wall_seconds": round(wall, 2), "notes": notes,
        "documents": [{k: v for k, v in e.items() if k != "summary"} | {"summary": _compact(e.get("summary"))}
                      for e in entries],
    }
    print(write_report(out, report))
    print(f"report: {out / 'report.md'}", file=sys.stderr)
    if not args.record and totals["regressed"]:
        return 1
    return 0


def _compact(summary: dict[str, Any] | None) -> dict[str, Any] | None:
    """The report keeps counts, warnings and agreement; the full summary is in summaries/."""
    if summary is None:
        return None
    return {key: summary[key] for key in ("counts", "warnings", "agreement", "collectors", "status") if key in summary}


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
