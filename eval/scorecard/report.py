"""Render the scorecard as report.md (one screen) and report.json (everything)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .changes import has_changes, render_lines
from .scoring import PASS, REGRESS, SKIP

METRIC_COLUMNS = ("text", "order", "table_cells", "table_structure", "headings", "pictures", "warnings", "agreement",
                  "latency", "stability")
TRUTH_COLUMNS = ("truth_headings", "truth_heading_levels", "truth_order", "truth_anchors_found", "truth_table_cells",
                 "truth_figures")
MARK = {PASS: "PASS", REGRESS: "REGRESS", SKIP: "SKIP"}


def _cell(metric: dict[str, Any] | None) -> str:
    if metric is None or metric["verdict"] == SKIP:
        return "SKIP"
    value = metric["value"]
    if metric["name"] == "warnings":
        shown = f"{int(value)}"
    elif metric["name"] == "latency":
        shown = f"{value:.0f}ms"
    elif metric["name"] == "stability":
        shown = "stable" if value >= 1.0 else "unstable"
    else:
        shown = f"{value:.3f}"
    return f"{shown} {MARK[metric['verdict']]}"


def _truth_verdict(by_name: dict[str, dict[str, Any]]) -> str:
    verdicts = [by_name[name]["verdict"] for name in TRUTH_COLUMNS if name in by_name]
    if not verdicts or all(v == SKIP for v in verdicts):
        return "-"
    return REGRESS if REGRESS in verdicts else PASS


def _row(entry: dict[str, Any]) -> str:
    summary = entry.get("summary") or {}
    counts = summary.get("counts") or {}
    by_name = {m["name"]: m for m in (entry.get("score") or {}).get("metrics", [])}
    cells = [
        entry["doc_id"], entry["format"],
        str(counts.get("body_items", "")), str(counts.get("headings", "")), str(counts.get("tables", "")),
        str(counts.get("pictures", "")), str(len(summary.get("warnings", []))),
        f"{entry['elapsed_ms']:.0f}" if entry.get("elapsed_ms") is not None else "",
    ]
    if entry["status"] == "skipped":
        cells.extend(["SKIP"] * (len(METRIC_COLUMNS) + 1))
        cells.append(f"SKIP ({entry['reason']})")
    elif entry["status"] in ("no-baseline", "recorded", "floors-recorded"):
        cells.extend(["-"] * (len(METRIC_COLUMNS) + 1))
        cells.append(entry["status"].upper())
    else:
        cells.extend(_cell(by_name.get(name)) for name in METRIC_COLUMNS)
        cells.append(_truth_verdict(by_name))
        cells.append(entry["score"]["verdict"])
    return "| " + " | ".join(cells) + " |"


def _truth_cell(name: str, block: dict[str, Any], by_name: dict[str, dict[str, Any]]) -> str:
    value = (block.get("scores") or {}).get(name)
    if value is None:
        return "-"
    floor = (block.get("floors") or {}).get(name)
    shown = f"{value:.3f}"
    shown += f" (floor {floor:.3f})" if floor is not None else " (no floor)"
    metric = by_name.get(name)
    if metric is not None and metric["verdict"] != SKIP:
        shown += f" {MARK[metric['verdict']]}"
    return shown


def _truth_rows(entries: list[dict[str, Any]]) -> list[str]:
    rows = []
    for entry in entries:
        block = entry.get("truth") or {}
        if not block.get("scores"):
            continue
        by_name = {m["name"]: m for m in (entry.get("score") or {}).get("metrics", [])}
        cells = [entry["doc_id"], *[_truth_cell(name, block, by_name) for name in TRUTH_COLUMNS]]
        rows.append("| " + " | ".join(cells) + " |")
    return rows


def _truth_details(entries: list[dict[str, Any]]) -> list[str]:
    lines = []
    for entry in entries:
        block = entry.get("truth") or {}
        details = block.get("details") or {}
        scores = block.get("scores") or {}
        shown = [f"{name.removeprefix('truth_')}: {details[name]}" for name in TRUTH_COLUMNS
                 if name in details and scores.get(name) is not None and scores[name] < 1.0]
        if shown:
            lines.append(f"- {entry['doc_id']}: " + " | ".join(shown))
    return lines


def _agreement_rows(entries: list[dict[str, Any]]) -> list[str]:
    rows = []
    for entry in entries:
        agreement = (entry.get("summary") or {}).get("agreement")
        if not agreement:
            continue
        conflicts = "; ".join(f"{c['field']} -> {c['winner'] or '?'}" for c in agreement["conflicts"][:3])
        if len(agreement["conflicts"]) > 3:
            conflicts += f"; +{len(agreement['conflicts']) - 3} more"
        rows.append(f"| {entry['doc_id']} | {', '.join(agreement['collectors'])} | {agreement['shared']} | "
                    f"{agreement['agreed']} | {len(agreement['conflicts'])} | {len(agreement['winners'])} | {conflicts} |")
    return rows


def render_markdown(report: dict[str, Any]) -> str:
    header = ["doc", "format", "items", "headings", "tables", "pictures", "warnings", "ms", *METRIC_COLUMNS,
              "vs truth", "verdict"]
    totals = report["totals"]
    lines = [f"# gRParse scorecard: {report['label']} ({report['mode']})", "",
             f"target `{report['target']}` service `{report['service']}` baseline `{report['baseline_dir']}`"
             + (f" repeat {report['repeat']}" if report.get("repeat", 1) > 1 else ""), "",
             f"docs: {totals['scored']} scored, {totals['passed']} pass, {totals['regressed']} regress, "
             f"{totals['skipped']} skipped, {totals['no_baseline']} without baseline, {totals['recorded']} recorded; "
             f"wall {report['wall_seconds']:.1f}s", ""]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "---|" * len(header))
    lines.extend(_row(entry) for entry in report["documents"])
    skipped = [e for e in report["documents"] if e["status"] == "skipped"]
    if skipped:
        lines.extend(["", "Skipped:", *[f"- {e['doc_id']}: {e['reason']}" for e in skipped]])
    truth_rows = _truth_rows(report["documents"])
    if truth_rows:
        lines.extend(["", "## Truth (absolute, 1.0 = matches the source document)", "",
                      "| doc | " + " | ".join(c.removeprefix("truth_") for c in TRUTH_COLUMNS) + " |",
                      "|---|" + "---|" * len(TRUTH_COLUMNS), *truth_rows])
        details = _truth_details(report["documents"])
        if details:
            lines.extend(["", *details])
    agreement_rows = _agreement_rows(report["documents"])
    if agreement_rows:
        lines.extend(["", "## Cross-collector agreement", "",
                      "| doc | collectors | shared fields | agreed | conflicts | winners | conflict -> winner |",
                      "|---|---|---|---|---|---|---|", *agreement_rows])
    changed = [e for e in report["documents"] if e.get("status") == "scored" and has_changes(e.get("changes"))]
    if changed:
        lines.extend(["", "## Changes", ""])
        for entry in changed:
            lines.append(f"### {entry['doc_id']} ({entry['score']['verdict']})")
            lines.extend(render_lines(entry["changes"]))
            lines.append("")
    unstable = [e for e in report["documents"] if e.get("stable") is False]
    if unstable:
        lines.extend(["", "## Unstable across repeats", "",
                      *[f"- {e['doc_id']}: first difference at `{e.get('first_diff')}`" for e in unstable]])
    if report.get("notes"):
        lines.extend(["", "## Notes", "", *[f"- {note}" for note in report["notes"]]])
    return "\n".join(lines) + "\n"


def write_report(out: Path, report: dict[str, Any]) -> str:
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    markdown = render_markdown(report)
    (out / "report.md").write_text(markdown)
    return markdown
