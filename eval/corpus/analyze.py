#!/usr/bin/env python3
"""Corpus evaluation telemetry analyzer and run-over-run drift detector.

    # Single run summary:
    uv run python eval/corpus/analyze.py eval/out/corpus/live/results.jsonl \
        --out-json eval/out/corpus/live/analysis.json --out-md eval/out/corpus/live/analysis.md

    # Run-over-run comparison with regression gates:
    uv run python eval/corpus/analyze.py \
        --baseline eval/out/corpus/base/results.jsonl \
        --candidate eval/out/corpus/cand/results.jsonl \
        --out-json eval/out/corpus/cand/drift.json \
        --out-md eval/out/corpus/cand/drift.md \
        --fail-on-new-failures --max-error-rate-delta 0.01 --max-p95-regression-pct 20

Exit codes: 0 when all regression gates pass (or no gates failed), 1 when any
configured regression gate triggers, 2 on invalid arguments or missing files.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import threading
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from corpus.metrics import normalize_status  # noqa: E402

FAILURE_STATUSES = frozenset({"failure", "rpc_error"})
SUCCESS_STATUSES = frozenset({"success", "partial_success"})


def _atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.parent / f".{path.name}.tmp.{os.getpid()}.{threading.get_ident()}"
    tmp.write_text(content, encoding="utf-8")
    os.replace(tmp, path)


def percentile(sorted_vals: list[float], q: float) -> float:
    """Linearly interpolated percentile q in [0, 100] over a pre-sorted list."""
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return round(sorted_vals[0], 2)
    pos = (q / 100.0) * (len(sorted_vals) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return round(sorted_vals[lo], 2)
    weight = pos - lo
    return round(sorted_vals[lo] * (1.0 - weight) + sorted_vals[hi] * weight, 2)


def load_journal(path: Path) -> list[dict[str, Any]]:
    """Read a results.jsonl file, deduplicating by relpath (latest entry wins)."""
    if not path.is_file():
        raise FileNotFoundError(f"journal file not found: {path}")
    by_relpath: dict[str, dict[str, Any]] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line_no, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as err:
                raise ValueError(f"{path}:{line_no}: invalid JSON: {err}") from err
            relpath = str(rec.get("relpath") or rec.get("key") or f"line-{line_no}")
            rec = dict(rec)
            rec["relpath"] = relpath
            rec["doctype"] = str(rec.get("doctype") or rec.get("ext") or rec.get("family") or "unknown")
            rec["parser_type"] = str(rec.get("parser_type") or "none")
            rec["status"] = normalize_status(
                rec.get("status"),
                rec.get("rpc_error"),
                skipped=bool(rec.get("skipped")) or rec.get("status") == "skipped",
            )
            failed = rec.get("failed_checks")
            if failed is None:
                checks = rec.get("checks") or {}
                failed = sorted(k for k, v in checks.items() if v == "fail")
            rec["failed_checks"] = sorted(set(str(c) for c in failed))
            rec["elapsed_ms"] = float(rec.get("elapsed_ms") or 0.0)
            rec["sha256"] = str(rec.get("sha256") or "")
            rec["output_sha256"] = str(rec.get("output_sha256") or "")
            by_relpath[relpath] = rec
    return [by_relpath[k] for k in sorted(by_relpath)]


def _summarize_slice(records: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(records)
    skipped = sum(1 for r in records if r["status"] == "skipped")
    evaluated = [r for r in records if r["status"] != "skipped"]
    eval_count = len(evaluated)

    success_count = sum(1 for r in evaluated if r["status"] in SUCCESS_STATUSES)
    error_count = sum(1 for r in evaluated if r["status"] in FAILURE_STATUSES)
    check_fail_docs = sum(1 for r in evaluated if r["failed_checks"])

    success_rate = round(success_count / eval_count, 6) if eval_count > 0 else 0.0
    error_rate = round(error_count / eval_count, 6) if eval_count > 0 else 0.0

    latencies = sorted(r["elapsed_ms"] for r in evaluated)
    mean_ms = round(sum(latencies) / len(latencies), 2) if latencies else 0.0

    check_failures: dict[str, int] = {}
    status_counts: dict[str, int] = {}
    for r in records:
        st = r["status"]
        status_counts[st] = status_counts.get(st, 0) + 1
        for chk in r["failed_checks"]:
            check_failures[chk] = check_failures.get(chk, 0) + 1

    return {
        "total": total,
        "evaluated": eval_count,
        "skipped": skipped,
        "success_count": success_count,
        "error_count": error_count,
        "docs_with_check_failures": check_fail_docs,
        "success_rate": success_rate,
        "error_rate": error_rate,
        "latency_ms": {
            "p50": percentile(latencies, 50.0),
            "p90": percentile(latencies, 90.0),
            "p95": percentile(latencies, 95.0),
            "p99": percentile(latencies, 99.0),
            "mean": mean_ms,
            "max": round(latencies[-1], 2) if latencies else 0.0,
        },
        "status_counts": dict(sorted(status_counts.items())),
        "check_failures": dict(sorted(check_failures.items())),
    }


def summarize_run(records: list[dict[str, Any]], *, label: str = "run") -> dict[str, Any]:
    """Compute overall, per-doctype, and per-parser_type statistics for one journal."""
    by_doctype: dict[str, list[dict[str, Any]]] = {}
    by_parser: dict[str, list[dict[str, Any]]] = {}
    for rec in records:
        by_doctype.setdefault(rec["doctype"], []).append(rec)
        by_parser.setdefault(rec["parser_type"], []).append(rec)

    return {
        "label": label,
        "overall": _summarize_slice(records),
        "by_doctype": {dt: _summarize_slice(rows) for dt, rows in sorted(by_doctype.items())},
        "by_parser_type": {pt: _summarize_slice(rows) for pt, rows in sorted(by_parser.items())},
    }


def _pct_change(base_val: float, cand_val: float) -> float:
    if base_val <= 0.0:
        return 0.0 if cand_val <= 0.0 else 100.0
    return round(((cand_val - base_val) / base_val) * 100.0, 2)


def _compare_slices(base_slice: dict[str, Any], cand_slice: dict[str, Any]) -> dict[str, Any]:
    base_lat = base_slice["latency_ms"]
    cand_lat = cand_slice["latency_ms"]
    return {
        "baseline_evaluated": base_slice["evaluated"],
        "candidate_evaluated": cand_slice["evaluated"],
        "baseline_success_rate": base_slice["success_rate"],
        "candidate_success_rate": cand_slice["success_rate"],
        "success_rate_delta": round(cand_slice["success_rate"] - base_slice["success_rate"], 6),
        "baseline_error_rate": base_slice["error_rate"],
        "candidate_error_rate": cand_slice["error_rate"],
        "error_rate_delta": round(cand_slice["error_rate"] - base_slice["error_rate"], 6),
        "latency_ms": {
            "p50_baseline": base_lat["p50"],
            "p50_candidate": cand_lat["p50"],
            "p50_delta_ms": round(cand_lat["p50"] - base_lat["p50"], 2),
            "p50_regression_pct": _pct_change(base_lat["p50"], cand_lat["p50"]),
            "p90_baseline": base_lat["p90"],
            "p90_candidate": cand_lat["p90"],
            "p90_delta_ms": round(cand_lat["p90"] - base_lat["p90"], 2),
            "p90_regression_pct": _pct_change(base_lat["p90"], cand_lat["p90"]),
            "p95_baseline": base_lat["p95"],
            "p95_candidate": cand_lat["p95"],
            "p95_delta_ms": round(cand_lat["p95"] - base_lat["p95"], 2),
            "p95_regression_pct": _pct_change(base_lat["p95"], cand_lat["p95"]),
            "p99_baseline": base_lat["p99"],
            "p99_candidate": cand_lat["p99"],
            "p99_delta_ms": round(cand_lat["p99"] - base_lat["p99"], 2),
            "p99_regression_pct": _pct_change(base_lat["p99"], cand_lat["p99"]),
        },
    }


def compare_runs(
    baseline_records: list[dict[str, Any]],
    candidate_records: list[dict[str, Any]],
    *,
    baseline_label: str = "baseline",
    candidate_label: str = "candidate",
    max_error_rate_delta: float | None = None,
    max_p95_regression_pct: float | None = None,
    fail_on_new_failures: bool = False,
) -> dict[str, Any]:
    """Compare baseline and candidate journals, detecting regressions and content drift."""
    base_summary = summarize_run(baseline_records, label=baseline_label)
    cand_summary = summarize_run(candidate_records, label=candidate_label)

    base_by_rel = {r["relpath"]: r for r in baseline_records}
    cand_by_rel = {r["relpath"]: r for r in candidate_records}
    base_by_sha: dict[str, dict[str, Any]] = {}
    for r in baseline_records:
        if r["sha256"] and r["sha256"] not in base_by_sha:
            base_by_sha[r["sha256"]] = r

    new_failures: list[dict[str, Any]] = []
    fixed_documents: list[dict[str, Any]] = []
    new_check_failures: list[dict[str, Any]] = []
    resolved_check_failures: list[dict[str, Any]] = []
    content_drift: list[dict[str, Any]] = []

    common_relpaths = sorted(set(base_by_rel) & set(cand_by_rel))
    added_relpaths = sorted(set(cand_by_rel) - set(base_by_rel))
    removed_relpaths = sorted(set(base_by_rel) - set(cand_by_rel))

    drift_seen_relpaths: set[str] = set()

    for relpath in common_relpaths:
        b_rec = base_by_rel[relpath]
        c_rec = cand_by_rel[relpath]
        b_status = b_rec["status"]
        c_status = c_rec["status"]

        if b_status in SUCCESS_STATUSES and c_status in FAILURE_STATUSES:
            new_failures.append({
                "relpath": relpath,
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "baseline_status": b_status,
                "candidate_status": c_status,
                "rpc_error": c_rec.get("rpc_error"),
            })
        elif b_status in FAILURE_STATUSES and c_status in SUCCESS_STATUSES:
            fixed_documents.append({
                "relpath": relpath,
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "baseline_status": b_status,
                "candidate_status": c_status,
            })

        b_checks = set(b_rec["failed_checks"])
        c_checks = set(c_rec["failed_checks"])
        newly_failed = sorted(c_checks - b_checks)
        newly_resolved = sorted(b_checks - c_checks)
        if newly_failed:
            new_check_failures.append({
                "relpath": relpath,
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "new_failed_checks": newly_failed,
                "baseline_failed_checks": sorted(b_checks),
                "candidate_failed_checks": sorted(c_checks),
            })
        if newly_resolved:
            resolved_check_failures.append({
                "relpath": relpath,
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "resolved_checks": newly_resolved,
            })

        if (
            b_rec["sha256"]
            and b_rec["sha256"] == c_rec["sha256"]
            and b_rec["output_sha256"]
            and c_rec["output_sha256"]
            and b_rec["output_sha256"] != c_rec["output_sha256"]
        ):
            drift_seen_relpaths.add(relpath)
            content_drift.append({
                "relpath": relpath,
                "baseline_relpath": b_rec["relpath"],
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "sha256": c_rec["sha256"],
                "baseline_output_sha256": b_rec["output_sha256"],
                "candidate_output_sha256": c_rec["output_sha256"],
            })

    # Also detect content drift across renamed files sharing the same input sha256.
    for c_rec in candidate_records:
        relpath = c_rec["relpath"]
        if relpath in drift_seen_relpaths or relpath in base_by_rel:
            continue
        sha = c_rec["sha256"]
        if not sha or not c_rec["output_sha256"]:
            continue
        b_rec = base_by_sha.get(sha)
        if b_rec and b_rec["output_sha256"] and b_rec["output_sha256"] != c_rec["output_sha256"]:
            content_drift.append({
                "relpath": relpath,
                "baseline_relpath": b_rec["relpath"],
                "doctype": c_rec["doctype"],
                "parser_type": c_rec["parser_type"],
                "sha256": sha,
                "baseline_output_sha256": b_rec["output_sha256"],
                "candidate_output_sha256": c_rec["output_sha256"],
            })

    all_doctypes = sorted(set(base_summary["by_doctype"]) | set(cand_summary["by_doctype"]))
    empty_slice = _summarize_slice([])
    doctype_deltas = {
        dt: _compare_slices(
            base_summary["by_doctype"].get(dt, empty_slice),
            cand_summary["by_doctype"].get(dt, empty_slice),
        )
        for dt in all_doctypes
    }

    all_parsers = sorted(set(base_summary["by_parser_type"]) | set(cand_summary["by_parser_type"]))
    parser_deltas = {
        pt: _compare_slices(
            base_summary["by_parser_type"].get(pt, empty_slice),
            cand_summary["by_parser_type"].get(pt, empty_slice),
        )
        for pt in all_parsers
    }

    overall_delta = _compare_slices(base_summary["overall"], cand_summary["overall"])

    gate_failures: list[str] = []
    if fail_on_new_failures:
        if new_failures:
            gate_failures.append(f"new status failures: {len(new_failures)} document(s) regressed to failure/rpc_error")
        if new_check_failures:
            gate_failures.append(f"new check failures: {len(new_check_failures)} document(s) failed additional shape checks")
    if max_error_rate_delta is not None:
        actual_err_delta = overall_delta["error_rate_delta"]
        if actual_err_delta > max_error_rate_delta:
            gate_failures.append(
                f"error_rate_delta {actual_err_delta:+.4f} exceeds --max-error-rate-delta {max_error_rate_delta:.4f}"
            )
    if max_p95_regression_pct is not None:
        actual_p95_pct = overall_delta["latency_ms"]["p95_regression_pct"]
        if actual_p95_pct > max_p95_regression_pct:
            gate_failures.append(
                f"p95 latency regression {actual_p95_pct:+.2f}% exceeds --max-p95-regression-pct {max_p95_regression_pct:.2f}%"
            )

    exit_code = 1 if gate_failures else 0

    return {
        "mode": "comparison",
        "exit_code": exit_code,
        "gate_failures": gate_failures,
        "baseline": base_summary,
        "candidate": cand_summary,
        "deltas": {
            "overall": overall_delta,
            "by_doctype": doctype_deltas,
            "by_parser_type": parser_deltas,
        },
        "new_failures": new_failures,
        "fixed_documents": fixed_documents,
        "new_check_failures": new_check_failures,
        "resolved_check_failures": resolved_check_failures,
        "content_drift": content_drift,
        "added_documents": added_relpaths,
        "removed_documents": removed_relpaths,
    }


def _md_escape(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_analysis_markdown(report: dict[str, Any]) -> str:
    if report.get("mode") == "single":
        summary = report["summary"]
        ov = summary["overall"]
        lat = ov["latency_ms"]
        lines = [
            f"# Corpus evaluation analysis: {summary['label']}",
            "",
            f"- Documents: {ov['total']} total, {ov['evaluated']} evaluated, {ov['skipped']} skipped",
            f"- Success rate: {ov['success_rate'] * 100:.2f}% ({ov['success_count']}/{ov['evaluated']})",
            f"- Error rate: {ov['error_rate'] * 100:.2f}% ({ov['error_count']}/{ov['evaluated']})",
            f"- Latency (ms): p50={lat['p50']:.1f}, p90={lat['p90']:.1f}, p95={lat['p95']:.1f}, p99={lat['p99']:.1f}, max={lat['max']:.1f}",
            f"- Exit code: {report['exit_code']}",
            "",
        ]
        if report.get("gate_failures"):
            lines.append("## Gate failures")
            lines.append("")
            for gf in report["gate_failures"]:
                lines.append(f"- {gf}")
            lines.append("")

        lines += [
            "## By doctype",
            "",
            "| doctype | evaluated | success % | error % | p50 ms | p90 ms | p95 ms | p99 ms |",
            "|---|---|---|---|---|---|---|---|",
        ]
        for dt, st in summary["by_doctype"].items():
            l_dt = st["latency_ms"]
            lines.append(
                f"| `{_md_escape(dt)}` | {st['evaluated']} | {st['success_rate'] * 100:.1f}% | "
                f"{st['error_rate'] * 100:.1f}% | {l_dt['p50']:.1f} | {l_dt['p90']:.1f} | "
                f"{l_dt['p95']:.1f} | {l_dt['p99']:.1f} |"
            )

        lines += [
            "",
            "## By parser type",
            "",
            "| parser type | evaluated | success % | error % | p50 ms | p90 ms | p95 ms | p99 ms |",
            "|---|---|---|---|---|---|---|---|",
        ]
        for pt, st in summary["by_parser_type"].items():
            l_pt = st["latency_ms"]
            lines.append(
                f"| `{_md_escape(pt)}` | {st['evaluated']} | {st['success_rate'] * 100:.1f}% | "
                f"{st['error_rate'] * 100:.1f}% | {l_pt['p50']:.1f} | {l_pt['p90']:.1f} | "
                f"{l_pt['p95']:.1f} | {l_pt['p99']:.1f} |"
            )
        return "\n".join(lines) + "\n"

    # Comparison mode
    ov_delta = report["deltas"]["overall"]
    lat_delta = ov_delta["latency_ms"]
    lines = [
        f"# Corpus drift report: {report['baseline']['label']} vs {report['candidate']['label']}",
        "",
        f"- Evaluated: {ov_delta['baseline_evaluated']} (baseline) -> {ov_delta['candidate_evaluated']} (candidate)",
        f"- Success rate: {ov_delta['baseline_success_rate'] * 100:.2f}% -> {ov_delta['candidate_success_rate'] * 100:.2f}% "
        f"(delta {ov_delta['success_rate_delta'] * 100:+.2f}%)",
        f"- Error rate: {ov_delta['baseline_error_rate'] * 100:.2f}% -> {ov_delta['candidate_error_rate'] * 100:.2f}% "
        f"(delta {ov_delta['error_rate_delta'] * 100:+.2f}%)",
        f"- Latency p50: {lat_delta['p50_baseline']:.1f} ms -> {lat_delta['p50_candidate']:.1f} ms "
        f"({lat_delta['p50_delta_ms']:+.1f} ms, {lat_delta['p50_regression_pct']:+.1f}%)",
        f"- Latency p90: {lat_delta['p90_baseline']:.1f} ms -> {lat_delta['p90_candidate']:.1f} ms "
        f"({lat_delta['p90_delta_ms']:+.1f} ms, {lat_delta['p90_regression_pct']:+.1f}%)",
        f"- Latency p95: {lat_delta['p95_baseline']:.1f} ms -> {lat_delta['p95_candidate']:.1f} ms "
        f"({lat_delta['p95_delta_ms']:+.1f} ms, {lat_delta['p95_regression_pct']:+.1f}%)",
        f"- Latency p99: {lat_delta['p99_baseline']:.1f} ms -> {lat_delta['p99_candidate']:.1f} ms "
        f"({lat_delta['p99_delta_ms']:+.1f} ms, {lat_delta['p99_regression_pct']:+.1f}%)",
        f"- New failures: {len(report['new_failures'])}; Fixed documents: {len(report['fixed_documents'])}; "
        f"New check failures: {len(report['new_check_failures'])}; Content drift: {len(report['content_drift'])}",
        f"- Exit code: {report['exit_code']}",
        "",
    ]
    if report.get("gate_failures"):
        lines.append("## Gate failures")
        lines.append("")
        for gf in report["gate_failures"]:
            lines.append(f"- {gf}")
        lines.append("")

    lines += [
        "## Per-doctype comparison",
        "",
        "| doctype | base eval | cand eval | base success % | cand success % | error delta % | p50 (base->cand ms) | p90 (base->cand ms) | p99 (base->cand ms) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for dt, d in report["deltas"]["by_doctype"].items():
        ld = d["latency_ms"]
        lines.append(
            f"| `{_md_escape(dt)}` | {d['baseline_evaluated']} | {d['candidate_evaluated']} | "
            f"{d['baseline_success_rate'] * 100:.1f}% | {d['candidate_success_rate'] * 100:.1f}% | "
            f"{d['error_rate_delta'] * 100:+.1f}% | {ld['p50_baseline']:.1f} -> {ld['p50_candidate']:.1f} | "
            f"{ld['p90_baseline']:.1f} -> {ld['p90_candidate']:.1f} | "
            f"{ld['p99_baseline']:.1f} -> {ld['p99_candidate']:.1f} |"
        )

    lines += [
        "",
        "## Per-parser_type comparison",
        "",
        "| parser type | base eval | cand eval | base success % | cand success % | error delta % | p50 (base->cand ms) | p90 (base->cand ms) | p99 (base->cand ms) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for pt, d in report["deltas"]["by_parser_type"].items():
        ld = d["latency_ms"]
        lines.append(
            f"| `{_md_escape(pt)}` | {d['baseline_evaluated']} | {d['candidate_evaluated']} | "
            f"{d['baseline_success_rate'] * 100:.1f}% | {d['candidate_success_rate'] * 100:.1f}% | "
            f"{d['error_rate_delta'] * 100:+.1f}% | {ld['p50_baseline']:.1f} -> {ld['p50_candidate']:.1f} | "
            f"{ld['p90_baseline']:.1f} -> {ld['p90_candidate']:.1f} | "
            f"{ld['p99_baseline']:.1f} -> {ld['p99_candidate']:.1f} |"
        )

    lines += ["", "## New failures", ""]
    if not report["new_failures"]:
        lines.append("none")
    for item in report["new_failures"]:
        err = f" ({item['rpc_error']})" if item.get("rpc_error") else ""
        lines.append(
            f"- `{_md_escape(item['relpath'])}` (`{item['doctype']}`, `{item['parser_type']}`): "
            f"{item['baseline_status']} -> {item['candidate_status']}{err}"
        )

    lines += ["", "## Fixed documents", ""]
    if not report["fixed_documents"]:
        lines.append("none")
    for item in report["fixed_documents"]:
        lines.append(
            f"- `{_md_escape(item['relpath'])}` (`{item['doctype']}`, `{item['parser_type']}`): "
            f"{item['baseline_status']} -> {item['candidate_status']}"
        )

    lines += ["", "## New shape-check failures", ""]
    if not report["new_check_failures"]:
        lines.append("none")
    for item in report["new_check_failures"]:
        lines.append(
            f"- `{_md_escape(item['relpath'])}` (`{item['doctype']}`, `{item['parser_type']}`): "
            f"new failed checks `{', '.join(item['new_failed_checks'])}`"
        )

    lines += ["", "## Content drift (same input sha256, different output_sha256)", ""]
    if not report["content_drift"]:
        lines.append("none")
    for item in report["content_drift"]:
        lines.append(
            f"- `{_md_escape(item['relpath'])}` (sha256 `{item['sha256'][:12]}...`): "
            f"output `{item['baseline_output_sha256'][:12]}...` -> `{item['candidate_output_sha256'][:12]}...`"
        )

    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("paths", nargs="*", type=Path, help="one or two results.jsonl paths")
    parser.add_argument("--baseline", type=Path, help="baseline results.jsonl path")
    parser.add_argument("--candidate", type=Path, help="candidate results.jsonl path")
    parser.add_argument("--out-json", type=Path, help="write JSON analysis report to this path")
    parser.add_argument("--out-md", type=Path, help="write Markdown analysis report to this path")
    parser.add_argument(
        "--max-error-rate-delta",
        type=float,
        default=None,
        help="fail if candidate error_rate - baseline error_rate exceeds this threshold",
    )
    parser.add_argument(
        "--max-p95-regression-pct",
        type=float,
        default=None,
        help="fail if overall p95 latency regression percentage exceeds this threshold",
    )
    parser.add_argument(
        "--fail-on-new-failures",
        action="store_true",
        help="fail if any document regresses to failure/rpc_error or has new shape-check failures",
    )
    args = parser.parse_args(argv)

    base_path = args.baseline
    cand_path = args.candidate
    if base_path is None and cand_path is None:
        if len(args.paths) == 1:
            base_path = args.paths[0]
        elif len(args.paths) == 2:
            base_path, cand_path = args.paths[0], args.paths[1]
        else:
            print("provide --baseline and --candidate (or 1 or 2 positional results.jsonl paths)", file=sys.stderr)
            return 2
    elif base_path is None and cand_path is not None:
        base_path, cand_path = cand_path, None

    try:
        assert base_path is not None
        base_records = load_journal(base_path)
        if cand_path is not None:
            cand_records = load_journal(cand_path)
            report = compare_runs(
                base_records,
                cand_records,
                baseline_label=base_path.parent.name or "baseline",
                candidate_label=cand_path.parent.name or "candidate",
                max_error_rate_delta=args.max_error_rate_delta,
                max_p95_regression_pct=args.max_p95_regression_pct,
                fail_on_new_failures=args.fail_on_new_failures,
            )
        else:
            summary = summarize_run(base_records, label=base_path.parent.name or "run")
            gate_failures: list[str] = []
            if args.fail_on_new_failures:
                ov = summary["overall"]
                if ov["error_count"] > 0 or ov["docs_with_check_failures"] > 0:
                    gate_failures.append(
                        f"run has {ov['error_count']} error(s) and "
                        f"{ov['docs_with_check_failures']} document(s) with check failures"
                    )
            report = {
                "mode": "single",
                "exit_code": 1 if gate_failures else 0,
                "gate_failures": gate_failures,
                "summary": summary,
            }
    except (FileNotFoundError, ValueError, OSError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 2

    md_text = render_analysis_markdown(report)
    if args.out_json is not None:
        _atomic_write_text(
            args.out_json,
            json.dumps(report, indent=1, sort_keys=True, ensure_ascii=False) + "\n",
        )
    if args.out_md is not None:
        _atomic_write_text(args.out_md, md_text)
    if args.out_json is None and args.out_md is None:
        print(md_text, end="")

    return int(report["exit_code"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
