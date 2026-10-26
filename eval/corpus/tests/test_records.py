"""The typed run.json and results.jsonl records read back strictly."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import Any

from corpus.journal import JournalError, JournalRecord
from corpus.provenance import load_manifest
from corpus.run import load_resume_journal
from corpus.run_manifest import (
    CorpusIdentity,
    GrparseCommit,
    Harness,
    Headline,
    Host,
    ManifestError,
    RunManifest,
    ServiceImage,
    Stack,
)


def _record(**overrides: Any) -> JournalRecord:
    fields: dict[str, Any] = {
        "relpath": "a.pdf", "sha256": "ab" * 32, "size_bytes": 10, "ext": "pdf", "family": "pdf",
        "doctype": "pdf", "parser_type": "grparse-cv", "status": "success",
        "raw_status": "CONVERSION_STATUS_SUCCESS", "rpc_error": None, "elapsed_ms": 12.5, "page_count": 1,
        "warnings_count": 0, "checks": {"parse_succeeds": "pass"}, "failed_checks": (), "output_sha256": "cd" * 32,
        "artifact_md": None, "artifact_json": None, "counts": {"pages": 1}, "collectors": (), "failures": (),
        "skipped": "",
    }
    return JournalRecord(**(fields | overrides))


def _manifest(**overrides: Any) -> RunManifest:
    fields: dict[str, Any] = {
        "label": "unit", "started_at": "2026-10-26T12:00:00.000Z", "target": "localhost:50051",
        "grparse": GrparseCommit(commit="a" * 40, source="--grparse-commit"),
        "stack": Stack(recorded=True, compose_project="p", services={
            "grparse": ServiceImage(image="pipestreamai/grparse:latest", image_id="sha256:" + "1" * 64,
                                    repo_digests=(), revision=None, version="0.3.0", created=None,
                                    state="running", env={"GRPARSE_ORT_EP": "cpu"}),
        }),
        "harness": Harness(commit="b" * 40, eval_dirty=False),
        "host": Host(hostname="h", platform="linux", python="3.14.0"),
        "config": {"repeat": 1, "include": ["*.pdf"]},
        "run_id": "20261026T120000Z-aaaaaaaaaaaa-nocorpus",
    }
    return RunManifest(**(fields | overrides))


def _raises(error: type[Exception], call: Any) -> str:
    try:
        call()
    except error as caught:
        return str(caught)
    raise AssertionError(f"expected {error.__name__}")


def test_journal_record_round_trips_and_rejects_gaps() -> None:
    record = _record()
    assert JournalRecord.from_json(json.loads(json.dumps(record.to_json())), "line 1") == record
    missing = record.to_json()
    del missing["failures"]
    assert "missing 'failures'" in _raises(JournalError, lambda: JournalRecord.from_json(missing, "line 1"))
    wrong = record.to_json() | {"size_bytes": True}
    assert "bool" in _raises(JournalError, lambda: JournalRecord.from_json(wrong, "line 1"))
    counts = record.to_json() | {"counts": {"pages": "1"}}
    assert "counts" in _raises(JournalError, lambda: JournalRecord.from_json(counts, "line 1"))


def test_resume_drops_only_a_cut_short_final_line() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        journal = Path(tmp) / "results.jsonl"
        line = json.dumps(_record().to_json())
        journal.write_text(line + "\n" + line[:20] + "\n", encoding="utf-8")
        keys, records = load_resume_journal(journal)
        assert keys == {("a.pdf", 10)} and len(records) == 1
        journal.write_text(line[:20] + "\n" + line + "\n", encoding="utf-8")
        assert "invalid JSON" in _raises(JournalError, lambda: load_resume_journal(journal))


def test_manifest_round_trips_unfinished_and_finished() -> None:
    unfinished = _manifest()
    data = json.loads(json.dumps(unfinished.to_json()))
    assert "corpus" not in data and data["finished_at"] is None
    assert RunManifest.from_json(data) == unfinished
    finished = _manifest(
        corpus=CorpusIdentity(sha256="c" * 64, files=2, bytes=20, unhashed_files=0, manifest="corpus-manifest.tsv"),
        headline=Headline(evaluated=2, skipped=0, status_success_count=2, success_count=2, error_count=0,
                          clean_count=1, clean_battery_count=2),
        finished_at="2026-10-26T12:05:00.000Z", wall_seconds=300.0, exit_code=0,
        resumed_at=("2026-10-26T12:01:00.000Z",), service="gRParse 0.3.0",
    )
    assert RunManifest.from_json(json.loads(json.dumps(finished.to_json()))) == finished


def test_malformed_manifest_names_the_file_and_field() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "run.json"
        assert load_manifest(path) is None
        data = _manifest().to_json()
        del data["harness"]["commit"]
        path.write_text(json.dumps(data), encoding="utf-8")
        message = _raises(ManifestError, lambda: load_manifest(path))
        assert str(path) in message and "'commit'" in message
        path.write_text(json.dumps(_manifest().to_json() | {"schema": "other/v9"}), encoding="utf-8")
        assert "schema" in _raises(ManifestError, lambda: load_manifest(path))
