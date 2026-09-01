"""Unit tests for the _meta.json merge policy."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.meta import merge_meta  # noqa: E402

FULL = {"service": "gRParse a", "target": "host:1", "manifest": "eval/scorecard/corpus.json",
        "documents": ["a", "b", "c"], "skipped": {"d": "external file missing: /x"}, "reason": ""}
PARTIAL = {"service": "gRParse b", "target": "other:2", "manifest": "eval/scorecard/corpus.json",
           "documents": ["b"], "skipped": {}, "reason": "b moved"}


def test_full_record_onto_nothing() -> None:
    meta = merge_meta(None, FULL, partial=False, timestamp="t0")
    assert meta["documents"] == ["a", "b", "c"] and meta["skipped"] == {"d": "external file missing: /x"}
    assert meta["service"] == "gRParse a" and meta["target"] == "host:1" and meta["reason"] == ""
    assert meta["history"] == [{"timestamp": "t0", "documents": ["a", "b", "c"], "reason": "", "service": "gRParse a", "partial": False}]


def test_partial_record_keeps_corpus_and_provenance() -> None:
    base = merge_meta(None, FULL, partial=False, timestamp="t0")
    meta = merge_meta(base, PARTIAL, partial=True, timestamp="t1")
    assert meta["documents"] == ["a", "b", "c"], "union, original order, no duplicate"
    assert meta["service"] == "gRParse a" and meta["target"] == "host:1", "a partial record keeps the original provenance"
    assert meta["reason"] == "", "the top-level reason belongs to the full record"
    assert meta["skipped"] == {"d": "external file missing: /x"}
    assert [h["timestamp"] for h in meta["history"]] == ["t0", "t1"]
    assert meta["history"][1] == {"timestamp": "t1", "documents": ["b"], "reason": "b moved", "service": "gRParse b", "partial": True}


def test_partial_record_adds_new_document_and_clears_its_skip() -> None:
    base = merge_meta(None, FULL, partial=False, timestamp="t0")
    run = dict(PARTIAL, documents=["d", "e"], skipped={"f": "external file missing: /f"})
    meta = merge_meta(base, run, partial=True, timestamp="t1")
    assert meta["documents"] == ["a", "b", "c", "d", "e"]
    assert meta["skipped"] == {"f": "external file missing: /f"}, "a recorded doc leaves skipped; new skips are added"


def test_partial_onto_nothing_behaves_like_full() -> None:
    meta = merge_meta(None, PARTIAL, partial=True, timestamp="t1")
    assert meta["documents"] == ["b"] and meta["service"] == "gRParse b" and meta["reason"] == "b moved"
    assert meta["history"][0]["partial"] is True


def test_full_record_resets_description_but_keeps_history() -> None:
    base = merge_meta(None, FULL, partial=False, timestamp="t0")
    base = merge_meta(base, PARTIAL, partial=True, timestamp="t1")
    fresh = merge_meta(base, dict(FULL, documents=["a", "c"], service="gRParse c", reason="new stack"), partial=False, timestamp="t2")
    assert fresh["documents"] == ["a", "c"] and fresh["service"] == "gRParse c" and fresh["reason"] == "new stack"
    assert [h["timestamp"] for h in fresh["history"]] == ["t0", "t1", "t2"]
    assert fresh["history"][2]["partial"] is False


def test_merge_does_not_mutate_inputs() -> None:
    base = merge_meta(None, FULL, partial=False, timestamp="t0")
    before = (list(base["documents"]), list(base["history"]), dict(base["skipped"]))
    merge_meta(base, dict(PARTIAL, documents=["z"]), partial=True, timestamp="t1")
    assert (base["documents"], base["history"], base["skipped"]) == before


def test_gates_survive_partial_and_full_records_and_upsert() -> None:
    base = merge_meta(None, dict(FULL, gates={"a": {"truth": {"truth_order": 0.9}, "latency_ms": 10.0}}), partial=False, timestamp="t0")
    assert base["gates"] == {"a": {"truth": {"truth_order": 0.9}, "latency_ms": 10.0}}
    run = dict(PARTIAL, gates={"b": {"stable": False}},
               gate_changes=[{"doc_id": "b", "metric": "stable", "before": True, "after": False, "reason": "observed"}])
    meta = merge_meta(base, run, partial=True, timestamp="t1")
    assert meta["gates"] == {"a": {"truth": {"truth_order": 0.9}, "latency_ms": 10.0}, "b": {"stable": False}}
    assert meta["history"][1]["gate_changes"] == run["gate_changes"]
    assert "gate_changes" not in meta["history"][0]
    fresh = merge_meta(meta, dict(FULL, gates={"a": {"latency_ms": 20.0}}), partial=False, timestamp="t2")
    assert fresh["gates"] == {"a": {"latency_ms": 20.0}, "b": {"stable": False}}, "a full record keeps other rows and replaces recorded ones"
