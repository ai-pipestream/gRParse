"""Tests for eval/corpus/analyze.py telemetry summary and run-over-run drift detection."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import Any

from corpus.analyze import main


def _write_journal(path: Path, entries: list[dict[str, Any]]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [json.dumps(e, separators=(",", ":")) for e in entries]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def _sample_baseline() -> list[dict[str, Any]]:
    return [
        {
            "relpath": "a.pdf",
            "sha256": "sha_a",
            "size_bytes": 100,
            "ext": "pdf",
            "family": "pdf",
            "doctype": "pdf",
            "parser_type": "pdf",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 20.0,
            "page_count": 2,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "pass"},
            "failed_checks": [],
            "output_sha256": "out_a_v1",
            "skipped": "",
        },
        {
            "relpath": "b.docx",
            "sha256": "sha_b",
            "size_bytes": 200,
            "ext": "docx",
            "family": "word",
            "doctype": "docx",
            "parser_type": "libreoffice",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 50.0,
            "page_count": 1,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "pass"},
            "failed_checks": [],
            "output_sha256": "out_b_v1",
            "skipped": "",
        },
        {
            "relpath": "c.html",
            "sha256": "sha_c",
            "size_bytes": 150,
            "ext": "html",
            "family": "html",
            "doctype": "html",
            "parser_type": "markup",
            "status": "failure",
            "raw_status": "CONVERSION_STATUS_FAILURE",
            "rpc_error": "INTERNAL: parse error",
            "elapsed_ms": 10.0,
            "page_count": 0,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "fail"},
            "failed_checks": ["parse_succeeds"],
            "output_sha256": "",
            "skipped": "",
        },
        {
            "relpath": "d.png",
            "sha256": "sha_d",
            "size_bytes": 300,
            "ext": "png",
            "family": "image",
            "doctype": "png",
            "parser_type": "grparse-cv",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 100.0,
            "page_count": 1,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "pass"},
            "failed_checks": [],
            "output_sha256": "out_d_v1",
            "skipped": "",
        },
    ]


def _sample_candidate() -> list[dict[str, Any]]:
    return [
        # a.pdf: same input sha256, output_sha256 drifted, latency increased
        {
            "relpath": "a.pdf",
            "sha256": "sha_a",
            "size_bytes": 100,
            "ext": "pdf",
            "family": "pdf",
            "doctype": "pdf",
            "parser_type": "pdf",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 30.0,
            "page_count": 2,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "pass"},
            "failed_checks": [],
            "output_sha256": "out_a_v2",
            "skipped": "",
        },
        # b.docx: regressed from success to rpc_error
        {
            "relpath": "b.docx",
            "sha256": "sha_b",
            "size_bytes": 200,
            "ext": "docx",
            "family": "word",
            "doctype": "docx",
            "parser_type": "libreoffice",
            "status": "rpc_error",
            "raw_status": "rpc error: DEADLINE_EXCEEDED: timeout",
            "rpc_error": "DEADLINE_EXCEEDED: timeout",
            "elapsed_ms": 600.0,
            "page_count": 0,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "fail"},
            "failed_checks": ["parse_succeeds"],
            "output_sha256": "",
            "skipped": "",
        },
        # c.html: fixed in candidate
        {
            "relpath": "c.html",
            "sha256": "sha_c",
            "size_bytes": 150,
            "ext": "html",
            "family": "html",
            "doctype": "html",
            "parser_type": "markup",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 12.0,
            "page_count": 1,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "pass"},
            "failed_checks": [],
            "output_sha256": "out_c_v2",
            "skipped": "",
        },
        # d.png: status still success, but gained a new shape-check failure
        {
            "relpath": "d.png",
            "sha256": "sha_d",
            "size_bytes": 300,
            "ext": "png",
            "family": "image",
            "doctype": "png",
            "parser_type": "grparse-cv",
            "status": "success",
            "raw_status": "CONVERSION_STATUS_SUCCESS",
            "rpc_error": None,
            "elapsed_ms": 200.0,
            "page_count": 1,
            "warnings_count": 0,
            "checks": {"parse_succeeds": "pass", "integrity": "fail"},
            "failed_checks": ["integrity"],
            "output_sha256": "out_d_v1",
            "skipped": "",
        },
    ]


def test_single_run_analysis() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        j_path = _write_journal(root / "base" / "results.jsonl", _sample_baseline())
        out_json = root / "single.json"
        out_md = root / "single.md"

        code = main([str(j_path), "--out-json", str(out_json), "--out-md", str(out_md)])
        assert code == 0
        report = json.loads(out_json.read_text(encoding="utf-8"))
        assert report["mode"] == "single"
        ov = report["summary"]["overall"]
        assert ov["evaluated"] == 4
        assert ov["success_count"] == 3
        assert ov["error_count"] == 1
        assert ov["success_rate"] == 0.75
        assert ov["error_rate"] == 0.25
        assert "pdf" in report["summary"]["by_doctype"]
        assert "libreoffice" in report["summary"]["by_parser_type"]
        md = out_md.read_text(encoding="utf-8")
        assert "## By doctype" in md and "## By parser type" in md


def test_comparison_detects_regressions_fixes_latency_and_content_drift() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base_path = _write_journal(root / "baseline" / "results.jsonl", _sample_baseline())
        cand_path = _write_journal(root / "candidate" / "results.jsonl", _sample_candidate())
        out_json = root / "drift.json"
        out_md = root / "drift.md"

        code = main([
            "--baseline", str(base_path),
            "--candidate", str(cand_path),
            "--out-json", str(out_json),
            "--out-md", str(out_md),
        ])
        assert code == 0  # no gate flags passed
        report = json.loads(out_json.read_text(encoding="utf-8"))
        assert report["mode"] == "comparison"
        assert [f["relpath"] for f in report["new_failures"]] == ["b.docx"]
        assert [f["relpath"] for f in report["fixed_documents"]] == ["c.html"]
        assert sorted(f["relpath"] for f in report["new_check_failures"]) == ["b.docx", "d.png"]
        assert [d["relpath"] for d in report["content_drift"]] == ["a.pdf"]
        assert report["content_drift"][0]["baseline_output_sha256"] == "out_a_v1"
        assert report["content_drift"][0]["candidate_output_sha256"] == "out_a_v2"
        assert report["deltas"]["overall"]["latency_ms"]["p95_regression_pct"] > 0

        md = out_md.read_text(encoding="utf-8")
        assert "`b.docx`" in md and "`c.html`" in md and "`a.pdf`" in md


def test_regression_gates_return_nonzero_on_breach() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base_path = _write_journal(root / "baseline" / "results.jsonl", _sample_baseline())
        cand_path = _write_journal(root / "candidate" / "results.jsonl", _sample_candidate())
        clean_path = _write_journal(root / "clean" / "results.jsonl", _sample_baseline())

        # Gate 1: --fail-on-new-failures triggers on cand_path, passes on clean_path
        assert main(["--baseline", str(base_path), "--candidate", str(cand_path), "--fail-on-new-failures"]) == 1
        assert main(["--baseline", str(base_path), "--candidate", str(clean_path), "--fail-on-new-failures"]) == 0

        # Gate 2: --max-p95-regression-pct triggers on cand_path (p95 rose sharply), passes on clean_path
        assert main(["--baseline", str(base_path), "--candidate", str(cand_path), "--max-p95-regression-pct", "10.0"]) == 1
        assert main(["--baseline", str(base_path), "--candidate", str(clean_path), "--max-p95-regression-pct", "10.0"]) == 0

        # Gate 3: --max-error-rate-delta triggers when candidate error rate delta exceeds threshold
        all_ok_path = _write_journal(
            root / "all_ok" / "results.jsonl",
            [r for r in _sample_baseline() if r["status"] == "success"],
        )
        assert main(["--baseline", str(all_ok_path), "--candidate", str(cand_path), "--max-error-rate-delta", "0.05"]) == 1
        assert main(["--baseline", str(base_path), "--candidate", str(clean_path), "--max-error-rate-delta", "0.0"]) == 0
