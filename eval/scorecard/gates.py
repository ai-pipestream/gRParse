"""Per-document gates kept in ``baseline/_meta.json`` under ``gates``:
truth floors (a ratchet), the latency budget and the stability row.

    "gates": {"<doc-id>": {"truth": {"truth_headings": 0.92, ...},
                           "latency_ms": 812.4, "stable": true}}

Truth floors only go up on a record unless a reason is given; the reason and
the old and new values land in the meta history. Latency is a budget, not a
quality score, so a record simply replaces it with the median of that run.
Stability is recorded only when the run repeated conversions, because one
conversion cannot show instability. All functions are pure.
"""

from __future__ import annotations

from dataclasses import dataclass
from statistics import median
from typing import Any

from .scoring import PASS, REGRESS, SKIP, MetricResult

# A truth score may wobble by OCR jitter or a reordered tie; two points below the floor is a real drop.
TRUTH_TOLERANCE = 0.02
# Latency may drift with load; a quarter slower, or half a second on a small document, is a regression.
LATENCY_RATIO_MAX = 1.25
LATENCY_SLACK_MS = 500.0


@dataclass(frozen=True)
class GateChange:
    doc_id: str
    metric: str
    before: float | bool | None
    after: float | bool | None
    reason: str

    def as_dict(self) -> dict[str, Any]:
        return {"doc_id": self.doc_id, "metric": self.metric, "before": self.before, "after": self.after,
                "reason": self.reason}


def truth_results(scores: dict[str, float | None], floors: dict[str, float] | None,
                  details: dict[str, str] | None = None) -> list[MetricResult]:
    """One MetricResult per truth metric: REGRESS when more than the tolerance
    below its floor, PASS otherwise (a metric without a floor passes and says so)."""
    results: list[MetricResult] = []
    floors = floors or {}
    details = details or {}
    for name, value in scores.items():
        if value is None:
            results.append(MetricResult(name, None, 0.0, True, SKIP, "truth is silent"))
            continue
        floor = floors.get(name)
        detail = details.get(name, "")
        if floor is None:
            results.append(MetricResult(name, round(value, 4), 0.0, True, PASS, f"no floor recorded; {detail}".strip("; ")))
            continue
        threshold = round(floor - TRUTH_TOLERANCE, 4)
        verdict = PASS if value >= threshold else REGRESS
        results.append(MetricResult(name, round(value, 4), threshold, True, verdict, f"floor {floor:.3f}; {detail}".strip("; ")))
    return results


def latency_result(baseline_ms: float | None, live_ms: float | None, *, enabled: bool = True) -> MetricResult:
    if not enabled:
        return MetricResult("latency", None if live_ms is None else round(live_ms, 1), 0.0, False, SKIP, "EVAL_LATENCY=off")
    if baseline_ms is None or live_ms is None:
        return MetricResult("latency", None if live_ms is None else round(live_ms, 1), 0.0, False, SKIP, "no latency recorded")
    budget = max(baseline_ms * LATENCY_RATIO_MAX, baseline_ms + LATENCY_SLACK_MS)
    verdict = PASS if live_ms <= budget else REGRESS
    return MetricResult("latency", round(live_ms, 1), round(budget, 1), False, verdict, f"baseline {baseline_ms:.0f} ms")


def stability_result(baseline_stable: bool | None, live_stable: bool | None, first_diff: str | None) -> MetricResult:
    """Stable is 1.0, unstable 0.0; a known-unstable document that stays unstable passes,
    one that becomes stable passes with a note, a stable one that flips regresses."""
    if live_stable is None:
        return MetricResult("stability", None, 1.0, True, SKIP, "single conversion (pass --repeat 2)")
    value = 1.0 if live_stable else 0.0
    if baseline_stable is None:
        return MetricResult("stability", value, 1.0, True, PASS, "no stability recorded" + (f"; differs at {first_diff}" if first_diff else ""))
    if live_stable:
        note = "was unstable; re-record to lock" if not baseline_stable else ""
        return MetricResult("stability", value, 1.0, True, PASS, note)
    if not baseline_stable:
        return MetricResult("stability", value, 1.0, True, PASS, f"known unstable; differs at {first_diff}")
    return MetricResult("stability", value, 1.0, True, REGRESS, f"was stable; differs at {first_diff}")


def median_ms(values: list[float]) -> float | None:
    return float(median(values)) if values else None


def ratchet(doc_id: str, existing: dict[str, Any] | None, scores: dict[str, float | None], *,
            latency_ms: float | None, stable: bool | None, reason: str) -> tuple[dict[str, Any], list[GateChange]]:
    """The new gate row for one document plus every change worth a history line.

    Floors rise freely; without a reason a lower live score keeps the old
    floor (reported as a change with reason "kept"), with a reason the floor
    is lowered and the reason is kept. Latency is replaced when measured.
    Stability is replaced only when measured (repeat >= 2).
    """
    row: dict[str, Any] = {key: value for key, value in (existing or {}).items()}
    floors: dict[str, float] = dict(row.get("truth", {}) or {})
    changes: list[GateChange] = []
    for name, value in scores.items():
        if value is None:
            continue
        value = round(value, 4)
        before = floors.get(name)
        if before is None or value > before:
            floors[name] = value
            if before is not None:
                changes.append(GateChange(doc_id, name, before, value, "raised"))
        elif value < before - 1e-9:
            if reason:
                floors[name] = value
                changes.append(GateChange(doc_id, name, before, value, reason))
            else:
                changes.append(GateChange(doc_id, name, before, before, f"kept (live {value:.3f}; pass --reason to lower)"))
    if floors:
        row["truth"] = dict(sorted(floors.items()))
    if latency_ms is not None:
        row["latency_ms"] = round(latency_ms, 1)
    if stable is not None:
        before_stable = row.get("stable")
        if before_stable is not None and before_stable != stable:
            changes.append(GateChange(doc_id, "stable", before_stable, stable, reason or "observed"))
        row["stable"] = stable
    return row, changes
