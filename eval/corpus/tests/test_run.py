"""Integration tests for eval/corpus/run.py."""

from __future__ import annotations

import hashlib
import json
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

from s3.tests.fixtures import FakeClient, result, scan_document, word_document

from corpus.run import SKIP, load_resume_journal, main


def _write_files(root: Path, files: dict[str, bytes]) -> None:
    for relpath, content in files.items():
        target = root / relpath
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def test_run_writes_results_jsonl_sharded_outputs_and_reports() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir = base / "corpus"
        out_dir = base / "out"
        docx_bytes = b"PK-docx-sample"
        png_bytes = b"\x89PNG-sample"
        _write_files(corpus_dir, {"sub/a.docx": docx_bytes, "img/b.png": png_bytes})

        answers = {"docx": result(word_document()), "png": result(scan_document())}
        clients: list[FakeClient] = []

        def client_factory(target: str) -> FakeClient:
            clients.append(FakeClient(target, answers))
            return clients[-1]

        scraped_bodies: list[str] = []

        def on_start(metrics_url: str | None) -> None:
            assert metrics_url is not None
            with urllib.request.urlopen(metrics_url, timeout=5.0) as resp:
                scraped_bodies.append(resp.read().decode("utf-8"))

        code = main(
            [
                "--corpus-dir", str(corpus_dir),
                "--out", str(out_dir),
                "--label", "unit",
                "--workers", "2",
                "--metrics-port", "0",
            ],
            env={},
            client_factory=client_factory,
            on_start=on_start,
        )
        assert code == 0
        assert len(scraped_bodies) == 1
        assert "grparse_eval_progress_ratio" in scraped_bodies[0]

        run_out = out_dir / "corpus" / "unit"
        assert (run_out / "report.json").is_file()
        assert (run_out / "report.md").is_file()
        assert (run_out / "metrics.prom").is_file()
        assert (run_out / "results.jsonl").is_file()

        _, records = load_resume_journal(run_out / "results.jsonl")
        assert len(records) == 2
        by_rel = {r["relpath"]: r for r in records}
        assert sorted(by_rel) == ["img/b.png", "sub/a.docx"]

        docx_sha = hashlib.sha256(docx_bytes).hexdigest()
        docx_rec = by_rel["sub/a.docx"]
        assert docx_rec["sha256"] == docx_sha
        assert docx_rec["size_bytes"] == len(docx_bytes)
        assert docx_rec["ext"] == "docx"
        assert docx_rec["family"] == "word"
        assert docx_rec["doctype"] == "docx"
        assert docx_rec["parser_type"] == "libreoffice"
        assert docx_rec["status"] == "success"
        assert docx_rec["rpc_error"] is None
        assert docx_rec["page_count"] == 2
        assert docx_rec["warnings_count"] == 0
        assert docx_rec["failed_checks"] == []
        assert docx_rec["output_sha256"] != ""
        assert docx_rec["artifact_md"] == f"outputs/{docx_sha[:2]}/{docx_sha}.md"
        assert docx_rec["artifact_json"] == f"outputs/{docx_sha[:2]}/{docx_sha}.json"
        assert (run_out / docx_rec["artifact_md"]).is_file()
        assert (run_out / docx_rec["artifact_json"]).is_file()

        prom_text = (run_out / "metrics.prom").read_text(encoding="utf-8")
        assert 'grparse_eval_documents_total{run_label="unit",doctype="docx",parser_type="libreoffice",status="success"} 1' in prom_text
        assert 'grparse_eval_documents_total{run_label="unit",doctype="png",parser_type="grparse-cv",status="success"} 1' in prom_text


def test_resume_skips_already_journaled_documents() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir = base / "corpus"
        out_dir = base / "out"
        _write_files(corpus_dir, {"a.docx": b"doc-a", "b.docx": b"doc-b"})

        answers = {"docx": result(word_document())}
        clients: list[FakeClient] = []

        def client_factory(target: str) -> FakeClient:
            clients.append(FakeClient(target, answers))
            return clients[-1]

        code1 = main(
            ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", "res", "--no-save-outputs"],
            env={},
            client_factory=client_factory,
        )
        assert code1 == 0
        assert len(clients[0].calls) == 2

        # Add a third file and re-run with --resume.
        _write_files(corpus_dir, {"c.docx": b"doc-c"})
        code2 = main(
            ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", "res", "--resume", "--no-save-outputs"],
            env={},
            client_factory=client_factory,
        )
        assert code2 == 0
        # Only c.docx should have been sent to gRParse on the resumed run.
        assert [c[0] for c in clients[1].calls] == ["c.docx"]

        run_out = out_dir / "corpus" / "res"
        report = json.loads((run_out / "report.json").read_text(encoding="utf-8"))
        assert report["resumed"] == 2
        assert report["totals"]["objects"] == 3
        assert report["totals"]["evaluated"] == 3
        _, records = load_resume_journal(run_out / "results.jsonl")
        assert sorted(r["relpath"] for r in records) == ["a.docx", "b.docx", "c.docx"]
        assert all(r["artifact_md"] is None and r["artifact_json"] is None for r in records)


def test_bounded_in_flight_execution_respects_two_times_workers() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir = base / "corpus"
        out_dir = base / "out"
        _write_files(corpus_dir, {f"d_{i:02d}.docx": f"payload-{i}".encode() for i in range(16)})

        lock = threading.Lock()
        active_calls = 0
        max_active_calls = 0

        class SlowFakeClient(FakeClient):
            def convert_bytes(self, data: bytes, filename: str, **kwargs):
                nonlocal active_calls, max_active_calls
                with lock:
                    active_calls += 1
                    if active_calls > max_active_calls:
                        max_active_calls = active_calls
                try:
                    time.sleep(0.015)
                    return super().convert_bytes(data, filename, **kwargs)
                finally:
                    with lock:
                        active_calls -= 1

        code = main(
            [
                "--corpus-dir", str(corpus_dir),
                "--out", str(out_dir),
                "--label", "bounded",
                "--workers", "3",
                "--no-save-outputs",
            ],
            env={},
            client_factory=lambda target: SlowFakeClient(target, {"docx": result(word_document())}),
        )
        assert code == 0
        assert 1 <= max_active_calls <= 3


def test_ebcdic_companion_and_error_exit_codes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir = base / "corpus"
        out_dir = base / "out"
        _write_files(
            corpus_dir,
            {
                "with_layout.ebc": b"\xc1",
                "with_layout.layout.json": b'{"fields": []}',
                "orphan.ebc": b"\xc2",
            },
        )
        clients: list[FakeClient] = []

        def client_factory(target: str) -> FakeClient:
            clients.append(FakeClient(target, {"ebc": result(word_document())}))
            return clients[-1]

        code = main(
            ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", "ebc"],
            env={},
            client_factory=client_factory,
        )
        assert code == 0
        ebc_calls = [c for c in clients[0].calls if c[1] == ("EBCDIC",)]
        assert len(ebc_calls) == 1 and ebc_calls[0][2] == b'{"fields": []}'

        # With --require, the skipped orphan.ebc fails the run with exit code 1.
        code_req = main(
            ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", "ebc-req", "--require"],
            env={},
            client_factory=client_factory,
        )
        assert code_req == 1

        # Unreachable service returns SKIP (77).
        code_down = main(
            ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", "down"],
            env={},
            client_factory=lambda target: FakeClient(target, {}, fail=True),
        )
        assert code_down == SKIP
