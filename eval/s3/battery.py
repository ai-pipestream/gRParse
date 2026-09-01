"""One object through the battery, and the (parser type x file type) matrix
the report is built from."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any

from .checks import ObjectContext, run_checks
from .formats import collectors_of, parser_type
from .owners import owner_of


@dataclass
class ObjectResult:
    key: str
    extension: str
    family: str
    size: int
    parser_type: str
    collectors: list[str]
    status: str
    elapsed_ms: float
    counts: dict[str, int]
    checks: dict[str, str]
    failures: list[dict[str, Any]]
    skipped: str = ""

    @property
    def failed(self) -> bool:
        return any(verdict == "fail" for verdict in self.checks.values())


def counts_of(document: dict[str, Any]) -> dict[str, int]:
    return {
        "texts": len(document.get("texts", []) or []),
        "tables": len(document.get("tables", []) or []),
        "pictures": len(document.get("pictures", []) or []),
        "groups": len(document.get("groups", []) or []),
        "pages": len(document.get("pages", {}) or {}),
        "claims": len(document.get("claims", []) or []),
    }


def evaluate(ctx: ObjectContext) -> ObjectResult:
    verdicts, failures = run_checks(ctx)
    document = ctx.first.document or {}
    return ObjectResult(
        key=ctx.key, extension=ctx.ext, family=ctx.family, size=ctx.size,
        parser_type=parser_type(document), collectors=sorted(collectors_of(document)),
        status=ctx.first.rpc_error or ctx.first.status, elapsed_ms=round(ctx.first.elapsed_ms, 1),
        counts=counts_of(document), checks=verdicts, failures=[asdict(f) for f in failures])


def skipped(key: str, ext: str, family: str, size: int, reason: str) -> ObjectResult:
    return ObjectResult(key=key, extension=ext, family=family, size=size, parser_type="none", collectors=[],
                        status="skipped", elapsed_ms=0.0, counts={}, checks={}, failures=[], skipped=reason)


@dataclass
class Cell:
    files: int = 0
    passed: int = 0
    failed: int = 0


@dataclass
class Matrix:
    rows: dict[tuple[str, str], dict[str, Cell]] = field(default_factory=dict)
    objects: dict[tuple[str, str], int] = field(default_factory=dict)

    def add(self, result: ObjectResult) -> None:
        if result.skipped:
            return
        row = (result.parser_type, result.extension or "(none)")
        self.objects[row] = self.objects.get(row, 0) + 1
        cells = self.rows.setdefault(row, {})
        for name, verdict in result.checks.items():
            if verdict == "n/a":
                continue
            cell = cells.setdefault(name, Cell())
            cell.files += 1
            if verdict == "pass":
                cell.passed += 1
            else:
                cell.failed += 1

    def as_list(self) -> list[dict[str, Any]]:
        out = []
        for row in sorted(self.rows):
            out.append({
                "parser_type": row[0], "extension": row[1], "objects": self.objects[row],
                "checks": {name: asdict(cell) for name, cell in sorted(self.rows[row].items())},
            })
        return out

    def per_check(self) -> dict[str, Cell]:
        totals: dict[str, Cell] = {}
        for cells in self.rows.values():
            for name, cell in cells.items():
                total = totals.setdefault(name, Cell())
                total.files += cell.files
                total.passed += cell.passed
                total.failed += cell.failed
        return dict(sorted(totals.items()))


def findings(results: list[ObjectResult]) -> list[dict[str, Any]]:
    """Failures grouped by (check, cause): the report's triage list."""
    groups: dict[tuple[str, str], dict[str, Any]] = {}
    for result in results:
        for failure in result.failures:
            key = (failure["check"], failure["cause"])
            entry = groups.setdefault(key, {"check": key[0], "cause": key[1], "objects": 0, "keys": [],
                                            "parser_types": set(), "extensions": set(), "collectors": set(),
                                            "evidence": failure["evidence"]})
            entry["objects"] += 1
            if len(entry["keys"]) < 10:
                entry["keys"].append(result.key)
            entry["parser_types"].add(result.parser_type)
            entry["extensions"].add(result.extension)
            entry["collectors"].update(result.collectors)
    out = []
    for entry in sorted(groups.values(), key=lambda e: (-e["objects"], e["check"], e["cause"])):
        entry["parser_types"] = sorted(entry["parser_types"])
        entry["extensions"] = sorted(entry["extensions"])
        entry["collectors"] = sorted(entry["collectors"])
        known = owner_of(entry["check"], entry["cause"])
        entry["owner"] = known.owner if known else ""
        entry["note"] = known.note if known else ""
        out.append(entry)
    return out
