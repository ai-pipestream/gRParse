"""Baseline provenance: ``baseline/_meta.json`` and how a re-record updates it.

A full ``--record`` describes the whole corpus; a partial one (``--only``)
must not erase that description. ``merge_meta`` is pure so the policy is
testable without a service: a partial record keeps the original target,
service and manifest, unions the recorded documents, and appends to
``history``; a full record replaces the description but still carries the
history forward, so every re-record reason accumulates. The per-document
``gates`` block (truth floors, latency budget, stability; see ``gates.py``)
survives both kinds of record and is updated row by row.
"""

from __future__ import annotations

from typing import Any

HISTORY_KEYS = ("timestamp", "documents", "reason", "service", "partial")


def history_entry(run: dict[str, Any], *, partial: bool, timestamp: str) -> dict[str, Any]:
    entry = {"timestamp": timestamp, "documents": list(run.get("documents", [])),
             "reason": run.get("reason", ""), "service": run.get("service", ""), "partial": partial}
    if run.get("gate_changes"):
        entry["gate_changes"] = [dict(change) for change in run["gate_changes"]]
    return entry


def merge_meta(existing: dict[str, Any] | None, run: dict[str, Any], *, partial: bool,
               timestamp: str) -> dict[str, Any]:
    """Fold one record run into the stored meta.

    ``run`` carries service, target, manifest, documents (recorded this run),
    skipped, reason, and optionally ``gates`` (rows to upsert) and
    ``gate_changes`` (history lines). A partial run onto no existing meta is
    treated as full.
    """
    history = list((existing or {}).get("history", []))
    history.append(history_entry(run, partial=partial, timestamp=timestamp))
    gates = {doc: dict(row) for doc, row in ((existing or {}).get("gates") or {}).items()}
    gates.update({doc: dict(row) for doc, row in (run.get("gates") or {}).items()})
    if not partial or not existing:
        fresh = {key: run.get(key) for key in ("service", "target", "manifest", "documents", "skipped", "reason")}
        fresh["documents"] = list(run.get("documents", []))
        fresh["skipped"] = dict(run.get("skipped", {}))
        fresh["reason"] = run.get("reason", "")
        fresh["history"] = history
        if gates:
            fresh["gates"] = dict(sorted(gates.items()))
        return fresh
    recorded = list(run.get("documents", []))
    documents = list(existing.get("documents", []))
    documents.extend(doc for doc in recorded if doc not in documents)
    skipped = {doc: why for doc, why in (existing.get("skipped") or {}).items() if doc not in recorded}
    skipped.update(run.get("skipped", {}))
    merged = {
        "service": existing.get("service", run.get("service")),
        "target": existing.get("target", run.get("target")),
        "manifest": existing.get("manifest", run.get("manifest")),
        "documents": documents, "skipped": skipped,
        "reason": existing.get("reason", ""), "history": history,
    }
    if gates:
        merged["gates"] = dict(sorted(gates.items()))
    return merged
