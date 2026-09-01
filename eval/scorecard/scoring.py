"""Apply the metrics to a (baseline, live) summary pair and issue verdicts.

Tolerances are the only policy in the scorecard; each one is a constant with
its rationale on the same line so a reviewer can argue with it directly.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any

from . import metrics

# Same bytes should read identically; 2% absorbs whitespace and OCR jitter on raster pages.
TEXT_SIMILARITY_MIN = 0.98
# One or two items shifting in a long book is noise; a section moving is not.
READING_ORDER_MIN = 0.95
# A table losing more than one cell in twenty has lost data.
TABLE_CELL_F1_MIN = 0.95
# Row and column counts and span layout are exact facts of the source; any change is a change.
TABLE_STRUCTURE_MIN = 1.0
# Heading text and levels are exact in every non-raster format; one flipped level in twenty is the limit.
HEADING_MIN = 0.95
# Pictures belong in their chapter, under their heading, on their page; one misplaced in ten is the limit.
PICTURE_PLACEMENT_MIN = 0.90
# A new warning on a fixed corpus is a new defect until a re-recorded baseline says otherwise.
WARNING_DELTA_MAX = 0
# The collector that wins a field is a deliberate ranking decision; a silent flip is a regression.
AGREEMENT_MIN = 1.0

PASS, REGRESS, SKIP = "PASS", "REGRESS", "SKIP"


@dataclass
class MetricResult:
    name: str
    value: float | None
    threshold: float
    higher_is_better: bool
    verdict: str
    detail: str = ""

    @staticmethod
    def of(name: str, value: float | None, threshold: float, *, higher_is_better: bool = True,
           detail: str = "") -> "MetricResult":
        if value is None:
            return MetricResult(name, None, threshold, higher_is_better, SKIP, detail)
        ok = value >= threshold if higher_is_better else value <= threshold
        return MetricResult(name, round(value, 4), threshold, higher_is_better, PASS if ok else REGRESS, detail)


@dataclass
class DocScore:
    doc_id: str
    verdict: str
    metrics: list[MetricResult] = field(default_factory=list)
    count_delta: dict[str, int] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {"doc_id": self.doc_id, "verdict": self.verdict,
                "metrics": [asdict(m) for m in self.metrics], "count_delta": self.count_delta}


def score_document(baseline: dict[str, Any], live: dict[str, Any]) -> DocScore:
    results: list[MetricResult] = []
    results.append(MetricResult.of("text", metrics.text_similarity(
        baseline.get("reading_text", ""), live.get("reading_text", "")), TEXT_SIMILARITY_MIN))
    results.append(MetricResult.of("order", metrics.reading_order_similarity(
        baseline.get("reading", []), live.get("reading", [])), READING_ORDER_MIN))

    prf = metrics.table_cell_f1(baseline.get("tables", []), live.get("tables", []))
    f1 = prf[2] if prf else None
    detail = f"p={prf[0]:.3f} r={prf[1]:.3f}" if prf else ""
    results.append(MetricResult.of("table_cells", f1, TABLE_CELL_F1_MIN, detail=detail))
    results.append(MetricResult.of("table_structure", metrics.table_structure_match(
        baseline.get("tables", []), live.get("tables", [])), TABLE_STRUCTURE_MIN))
    results.append(MetricResult.of("headings", metrics.heading_score(
        baseline.get("headings", []), live.get("headings", [])), HEADING_MIN))
    results.append(MetricResult.of("pictures", metrics.picture_placement_score(
        baseline.get("pictures", []), live.get("pictures", [])), PICTURE_PLACEMENT_MIN))

    new_count, new_warnings = metrics.warning_delta(baseline.get("warnings", []), live.get("warnings", []))
    results.append(MetricResult.of("warnings", float(new_count), WARNING_DELTA_MAX, higher_is_better=False,
                                   detail="; ".join(new_warnings)[:300]))
    results.append(MetricResult.of("agreement", metrics.agreement_score(
        baseline.get("agreement"), live.get("agreement")), AGREEMENT_MIN))

    verdict = REGRESS if any(r.verdict == REGRESS for r in results) else PASS
    return DocScore(doc_id=live.get("doc_id") or baseline.get("doc_id", ""), verdict=verdict, metrics=results,
                    count_delta=metrics.count_delta(baseline.get("counts", {}), live.get("counts", {})))
