"""Cross-collector agreement derived from Document.claims and field_sources.

Every collector that touched a document leaves a claim (its own view of the
origin, source metadata, page styles, email or media facts); the merged
document names the winner per field in ``field_sources``. This section
records who claimed what, where claims agree, where they conflict and who
won, so a change in the ranking or in a collector's output shows up as a
difference against the baseline.
"""

from __future__ import annotations

import json
from typing import Any

CLAIM_SECTIONS = ("origin", "source_meta", "page_styles", "email", "media")
Scalar = str | int | float | bool


def flatten(value: Any, prefix: str = "") -> dict[str, Scalar]:
    """Dotted leaf paths to scalar values; lists become one JSON leaf."""
    leaves: dict[str, Scalar] = {}
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "field_sources":
                continue
            path = f"{prefix}.{key}" if prefix else key
            leaves.update(flatten(child, path))
    elif isinstance(value, list):
        leaves[prefix] = json.dumps(value, sort_keys=True, ensure_ascii=False)
    elif value is None:
        return leaves
    else:
        leaves[prefix] = value
    return leaves


def winners(document: dict[str, Any]) -> dict[str, str]:
    """field path -> collector that won it, read from every field_sources list."""
    won: dict[str, str] = {}
    for section in CLAIM_SECTIONS:
        block = document.get(section)
        if not isinstance(block, dict):
            continue
        for entry in block.get("field_sources", []) or []:
            collector = (entry.get("source") or {}).get("collector", "")
            if entry.get("field") and collector:
                won[f"{section}.{entry['field']}"] = collector
    return won


def agreement_section(document: dict[str, Any]) -> dict[str, Any] | None:
    claims = document.get("claims", []) or []
    if not claims:
        return None
    by_collector: dict[str, dict[str, Scalar]] = {}
    for claim in claims:
        collector = (claim.get("source") or {}).get("collector", "") or "unknown"
        leaves = by_collector.setdefault(collector, {})
        for section in CLAIM_SECTIONS:
            if section in claim:
                leaves.update(flatten(claim[section], section))
    fields: dict[str, dict[str, Scalar]] = {}
    for collector, leaves in by_collector.items():
        for path, value in leaves.items():
            fields.setdefault(path, {})[collector] = value
    shared = {path: values for path, values in fields.items() if len(values) > 1}
    won = winners(document)
    conflicts = []
    agreed = 0
    for path in sorted(shared):
        values = shared[path]
        if len({json.dumps(v, sort_keys=True) for v in values.values()}) == 1:
            agreed += 1
        else:
            conflicts.append({"field": path, "values": {c: str(v)[:80] for c, v in sorted(values.items())},
                              "winner": won.get(path, "")})
    return {
        "collectors": sorted(by_collector),
        "claimed_fields": {c: len(leaves) for c, leaves in sorted(by_collector.items())},
        "shared": len(shared), "agreed": agreed, "conflicts": conflicts,
        "winners": dict(sorted(won.items())),
    }
