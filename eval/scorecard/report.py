"""Render the scorecard as report.md (one screen) and report.json (everything)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .changes import has_changes, render_lines
from .scoring import PASS, REGRESS, SKIP

METRIC_COLUMNS = ("text", "order", "table_cells", "table_structure", "headings", "pictures", "warnings", "agreement")
MARK = {PASS: "PASS", REGRESS: "REGRESS", SKIP: "SKIP"}


def _cell(metric: dict[str, Any] | None) -> str:
    if metric is None or metric["verdict"] == SKIP:
        return "SKIP"
    value = metric["value"]
    shown = f"{int(value)}" if metric["name"] == "warnings" else f"{value:.3f}"
    return f"{shown} {MARK[metric['verdict']]}"


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
        cells.extend(["SKIP"] * len(METRIC_COLUMNS))
        cells.append(f"SKIP ({entry['reason']})")
    elif entry["status"] == "no-baseline":
        cells.extend(["-"] * len(METRIC_COLUMNS))
        cells.append("NO-BASELINE")
    elif entry["status"] == "recorded":
        cells.extend(["-"] * len(METRIC_COLUMNS))
        cells.append("RECORDED")
    else:
        cells.extend(_cell(by_name.get(name)) for name in METRIC_COLUMNS)
        cells.append(entry["score"]["verdict"])
    return "| " + " | ".join(cells) + " |"


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
    header = ["doc", "format", "items", "headings", "tables", "pictures", "warnings", "ms", *METRIC_COLUMNS, "verdict"]
    lines = [f"# gRParse scorecard: {report['label']} ({report['mode']})", "",
             f"target `{report['target']}` service `{report['service']}` baseline `{report['baseline_dir']}`", "",
             f"docs: {report['totals']['scored']} scored, {report['totals']['passed']} pass, "
             f"{report['totals']['regressed']} regress, {report['totals']['skipped']} skipped, "
             f"{report['totals']['no_baseline']} without baseline, {report['totals']['recorded']} recorded; "
             f"wall {report['wall_seconds']:.1f}s", ""]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "---|" * len(header))
    lines.extend(_row(entry) for entry in report["documents"])
    skipped = [e for e in report["documents"] if e["status"] == "skipped"]
    if skipped:
        lines.extend(["", "Skipped:", *[f"- {e['doc_id']}: {e['reason']}" for e in skipped]])
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
    if report.get("notes"):
        lines.extend(["", "## Notes", "", *[f"- {note}" for note in report["notes"]]])
    return "\n".join(lines) + "\n"


def write_report(out: Path, report: dict[str, Any]) -> str:
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    markdown = render_markdown(report)
    (out / "report.md").write_text(markdown)
    return markdown
