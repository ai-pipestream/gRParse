"""report.json and report.md for one run."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .battery import Matrix, ObjectResult, findings
from .checks import CHECKS


def build_report(*, label: str, target: str, endpoint: str, bucket: str, prefix: str, service: str,
                 results: list[ObjectResult], matrix: Matrix, wall_seconds: float, notes: list[str],
                 exit_code: int) -> dict[str, Any]:
    evaluated = [r for r in results if not r.skipped]
    failed_objects = sum(1 for r in evaluated if r.failed)
    per_check = matrix.per_check()
    return {
        "label": label, "target": target, "endpoint": endpoint, "bucket": bucket, "prefix": prefix,
        "service": service, "wall_seconds": round(wall_seconds, 2), "exit_code": exit_code,
        "totals": {
            "objects": len(results), "evaluated": len(evaluated), "skipped": len(results) - len(evaluated),
            "failed_objects": failed_objects,
            "checks_run": sum(c.files for c in per_check.values()),
            "checks_failed": sum(c.failed for c in per_check.values()),
        },
        "per_check": {name: {"files": c.files, "pass": c.passed, "fail": c.failed} for name, c in per_check.items()},
        "matrix": matrix.as_list(),
        "findings": findings(results),
        "notes": notes,
        "objects": [r.__dict__ for r in results],
        "checks": {entry.name: entry.doc for entry in CHECKS},
    }


def _md_escape(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_markdown(report: dict[str, Any]) -> str:
    totals = report["totals"]
    lines = [f"# gRParse S3 eval: {report['label']}", "",
             f"target `{report['target']}` service `{report['service']}` bucket `{report['bucket']}`"
             f"{' prefix `' + report['prefix'] + '`' if report['prefix'] else ''} at `{report['endpoint']}`", "",
             f"objects: {totals['objects']} listed, {totals['evaluated']} evaluated, {totals['skipped']} skipped, "
             f"{totals['failed_objects']} with failures; checks: {totals['checks_run']} run, "
             f"{totals['checks_failed']} failed; wall {report['wall_seconds']}s; exit {report['exit_code']}", ""]
    for note in report.get("notes", []):
        lines.append(f"- {note}")
    if report.get("notes"):
        lines.append("")
    lines += ["## Checks", "", "| check | files | pass | fail |", "|---|---|---|---|"]
    for name, cell in report["per_check"].items():
        lines.append(f"| {name} | {cell['files']} | {cell['pass']} | {cell['fail']} |")
    lines += ["", "## Matrix: parser type x file type", "",
              "| parser type | ext | objects | failing checks (fail/files) |", "|---|---|---|---|"]
    for row in report["matrix"]:
        failing = ", ".join(f"{name} {cell['failed']}/{cell['files']}" for name, cell in row["checks"].items()
                            if cell["failed"])
        lines.append(f"| {row['parser_type']} | {row['extension']} | {row['objects']} | {failing or '-'} |")
    lines += ["", "## Findings (failures grouped by check and cause)", ""]
    if not report["findings"]:
        lines.append("none")
    for finding in report["findings"]:
        lines.append(f"### {finding['check']}: {_md_escape(finding['cause'])}")
        lines.append("")
        lines.append(f"- objects: {finding['objects']}; parser types: {', '.join(finding['parser_types'])}; "
                     f"extensions: {', '.join(finding['extensions'])}; collectors: {', '.join(finding['collectors']) or '-'}")
        lines.append("- keys: " + ", ".join(f"`{k}`" for k in finding["keys"]))
        if finding.get("owner"):
            lines.append(f"- owner: {finding['owner']}; {finding['note']}")
        else:
            lines.append("- owner: not yet triaged (eval/s3/owners.py names the known ones)")
        evidence = json.dumps(finding["evidence"], ensure_ascii=False, sort_keys=True)
        lines.append(f"- evidence (first object): `{_md_escape(evidence[:600])}`")
        lines.append("")
    lines += ["## Objects", "", "| key | ext | parser type | status | ms | texts | tables | pictures | pages | failed checks |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for obj in report["objects"]:
        counts = obj.get("counts") or {}
        failed = ", ".join(name for name, verdict in obj["checks"].items() if verdict == "fail")
        status = obj["skipped"] and f"skipped: {obj['skipped']}" or obj["status"]
        lines.append(f"| `{_md_escape(obj['key'])}` | {obj['extension']} | {obj['parser_type']} | {_md_escape(status)[:60]} | "
                     f"{obj['elapsed_ms']:.0f} | {counts.get('texts', '')} | {counts.get('tables', '')} | "
                     f"{counts.get('pictures', '')} | {counts.get('pages', '')} | {failed or '-'} |")
    skipped = [o for o in report["objects"] if o["skipped"]]
    if skipped:
        lines += ["", "## Skipped", ""]
        lines += [f"- `{_md_escape(o['key'])}`: {o['skipped']}" for o in skipped]
    return "\n".join(lines) + "\n"


def write_report(out: Path, report: dict[str, Any]) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
    markdown = render_markdown(report)
    (out / "report.md").write_text(markdown)
    return out / "report.md"
