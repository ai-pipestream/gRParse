"""Tests for run provenance (eval/corpus/provenance.py) through run.py and analyze.py."""

from __future__ import annotations

import hashlib
import json
import tempfile
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from s3.tests.fixtures import FakeClient, result, word_document

from corpus import analyze, provenance
from corpus.run import SKIP, main

COMMIT_A = "a" * 40
COMMIT_B = "b" * 40


def _write_files(root: Path, files: dict[str, bytes]) -> None:
    for relpath, content in files.items():
        target = root / relpath
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def fake_docker(revision: str = COMMIT_A, grparse_image_id: str = "sha256:" + "1" * 64) -> Any:
    """A docker CLI stand-in answering ps, inspect and image inspect for a two-service project."""
    containers = [
        {
            "Image": grparse_image_id,
            "Name": "/grparse-eval-grparse-1",
            "State": {"Status": "running"},
            "Config": {
                "Image": "pipestreamai/grparse:latest-cpu",
                "Labels": {provenance.COMPOSE_SERVICE_LABEL: "grparse"},
                "Env": [
                    "GRPARSE_PDF_BACKEND=pdfium:50069",
                    "GRPARSE_ORT_EP=cpu",
                    "GRPARSE_S3_SECRET_KEY=hunter2",
                    "PATH=/usr/bin",
                ],
            },
        },
        {
            "Image": "sha256:" + "2" * 64,
            "Name": "/grparse-eval-pdfium-1",
            "State": {"Status": "running"},
            "Config": {
                "Image": "pipestreamai/grpc-pdfium:latest",
                "Labels": {provenance.COMPOSE_SERVICE_LABEL: "pdfium"},
                "Env": [],
            },
        },
    ]
    images = [
        {
            "Id": grparse_image_id,
            "RepoDigests": ["pipestreamai/grparse@sha256:" + "c" * 64],
            "Config": {"Labels": {provenance.REVISION_LABEL: revision, provenance.VERSION_LABEL: "0.2.1"}},
        },
        {
            "Id": "sha256:" + "2" * 64,
            "RepoDigests": ["pipestreamai/grpc-pdfium@sha256:" + "d" * 64],
            "Config": {"Labels": {}},
        },
    ]
    calls: list[list[str]] = []

    def runner(args: Sequence[str]) -> str:
        calls.append(list(args))
        if args[0] == "ps":
            return "id-grparse\nid-pdfium\n"
        if args[0] == "inspect":
            return json.dumps(containers)
        if args[:2] == ["image", "inspect"]:
            return json.dumps(images)
        raise AssertionError(f"unexpected docker call {args}")

    runner.calls = calls  # type: ignore[attr-defined]
    return runner


def _run(corpus_dir: Path, out_dir: Path, label: str, *extra: str, docker: Any = None, answers=None) -> int:
    answers = answers or {"docx": result(word_document())}
    return main(
        ["--corpus-dir", str(corpus_dir), "--out", str(out_dir), "--label", label, "--no-save-outputs", *extra],
        env={},
        client_factory=lambda target: FakeClient(target, answers),
        docker_runner=docker,
    )


def test_stack_provenance_records_images_revision_and_safe_env() -> None:
    stack = provenance.stack_provenance("grparse-eval", fake_docker())
    assert isinstance(stack, provenance.StackProvenance)
    assert stack.recorded is True
    assert sorted(stack["services"]) == ["grparse", "pdfium"]
    grparse = stack["services"]["grparse"]
    assert isinstance(grparse, provenance.ServiceProvenance)
    assert grparse["revision"] == COMMIT_A
    assert list(grparse["repo_digests"]) == ["pipestreamai/grparse@sha256:" + "c" * 64]
    assert grparse["env"] == {"GRPARSE_ORT_EP": "cpu", "GRPARSE_PDF_BACKEND": "pdfium:50069"}
    assert "hunter2" not in json.dumps(stack.to_dict())
    assert provenance.grparse_commit(stack, None) == provenance.GrparseProvenance(
        commit=COMMIT_A, source=f"image label {provenance.REVISION_LABEL}",
    )
    assert provenance.grparse_commit(stack, "deadbeef")["commit"] == "deadbeef"
    assert provenance.grparse_commit({}, None)["commit"] is None


def test_run_writes_run_json_with_commit_digests_corpus_hash_and_headline() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir, out_dir = base / "corpus", base / "out"
        files = {"set1/a.docx": b"doc-a", "set2/b.docx": b"doc-bb"}
        _write_files(corpus_dir, files)
        password_file = base / "pw.txt"
        password_file.write_text("napierone\nother\n", encoding="utf-8")

        code = _run(corpus_dir, out_dir, "stamped", "--compose-project", "grparse-eval",
                    "--passwords-file", str(password_file), docker=fake_docker())
        assert code == 0
        run_out = out_dir / "corpus" / "stamped"
        manifest = json.loads((run_out / "run.json").read_text(encoding="utf-8"))

        assert manifest["schema"] == provenance.SCHEMA
        assert manifest["grparse"]["commit"] == COMMIT_A
        assert manifest["stack"]["services"]["pdfium"]["repo_digests"] == ["pipestreamai/grpc-pdfium@sha256:" + "d" * 64]
        assert manifest["service"] == "gRParse test"
        assert manifest["started_at"].endswith("Z") and manifest["finished_at"].endswith("Z")
        assert manifest["config"]["repeat"] == 1
        assert manifest["config"]["document_password_count"] == 2
        assert "napierone" not in (run_out / "run.json").read_text(encoding="utf-8")

        lines = sorted(f"{rel}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\n" for rel, data in files.items())
        expected = hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()
        assert manifest["corpus"]["sha256"] == expected
        assert manifest["corpus"]["files"] == 2
        assert manifest["corpus"]["bytes"] == 11
        assert (run_out / provenance.CORPUS_MANIFEST_NAME).read_text(encoding="utf-8") == "".join(lines)
        assert manifest["run_id"].endswith(f"-{COMMIT_A[:12]}-{expected[:12]}")
        assert manifest["headline"]["evaluated"] == 2
        assert manifest["headline"]["clean_count"] == 2
        assert manifest["headline"]["status_success_count"] == 2

        report = json.loads((run_out / "report.json").read_text(encoding="utf-8"))
        assert report["provenance"]["run_id"] == manifest["run_id"]
        markdown = (run_out / "report.md").read_text(encoding="utf-8")
        assert f"gRParse commit: `{COMMIT_A}`" in markdown
        assert "Clean output (every check passes): 2/2 (100.0%)" in markdown


def test_run_without_compose_project_says_images_were_not_recorded() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        _write_files(base / "corpus", {"a.docx": b"doc-a"})
        assert _run(base / "corpus", base / "out", "plain") == 0
        manifest = json.loads((base / "out" / "corpus" / "plain" / "run.json").read_text(encoding="utf-8"))
        assert manifest["stack"] == {"recorded": False, "reason": "no --compose-project given"}
        assert manifest["grparse"]["commit"] is None


def test_unreadable_stack_provenance_stops_the_run() -> None:
    def broken(args: Sequence[str]) -> str:
        raise provenance.ProvenanceError("docker is not on PATH")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        _write_files(base / "corpus", {"a.docx": b"doc-a"})
        assert _run(base / "corpus", base / "out", "nodocker", "--compose-project", "x", docker=broken) == SKIP
        assert _run(base / "corpus", base / "out", "nodocker", "--compose-project", "x", "--require",
                    docker=broken) == 1


def test_resume_on_a_different_image_is_refused() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        corpus_dir, out_dir = base / "corpus", base / "out"
        _write_files(corpus_dir, {"a.docx": b"doc-a"})
        assert _run(corpus_dir, out_dir, "res", "--compose-project", "p", docker=fake_docker()) == 0
        _write_files(corpus_dir, {"b.docx": b"doc-b"})
        moved = fake_docker(revision=COMMIT_B, grparse_image_id="sha256:" + "9" * 64)
        assert _run(corpus_dir, out_dir, "res", "--resume", "--compose-project", "p", docker=moved) == SKIP
        # The same stack resumes and keeps the original start time.
        first = json.loads((out_dir / "corpus" / "res" / "run.json").read_text(encoding="utf-8"))
        assert _run(corpus_dir, out_dir, "res", "--resume", "--compose-project", "p", docker=fake_docker()) == 0
        resumed = json.loads((out_dir / "corpus" / "res" / "run.json").read_text(encoding="utf-8"))
        assert resumed["started_at"] == first["started_at"]
        assert len(resumed["resumed_at"]) == 1
        assert resumed["corpus"]["files"] == 2


def _two_runs(base: Path, *, second_files: dict[str, bytes] | None = None) -> tuple[Path, Path]:
    corpus_dir, out_dir = base / "corpus", base / "out"
    _write_files(corpus_dir, {"set1/a.docx": b"doc-a", "set1/b.docx": b"doc-b"})
    assert _run(corpus_dir, out_dir, "run1", "--compose-project", "p", docker=fake_docker()) == 0
    if second_files:
        _write_files(corpus_dir, second_files)
    failing = {"docx": result(None, status="CONVERSION_STATUS_FAILURE")}
    moved = fake_docker(revision=COMMIT_B, grparse_image_id="sha256:" + "9" * 64)
    assert _run(corpus_dir, out_dir, "run2", "--compose-project", "p", docker=moved, answers=failing) == 1
    return out_dir / "corpus" / "run1" / "results.jsonl", out_dir / "corpus" / "run2" / "results.jsonl"


def test_drift_report_names_what_changed_and_gates_on_clean_rate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        run1, run2 = _two_runs(base)
        out_json = base / "drift.json"
        code = analyze.main([
            "--baseline", str(run1), "--candidate", str(run2), "--max-clean-rate-drop", "0.5",
            "--out-json", str(out_json), "--out-md", str(base / "drift.md"),
        ])
        assert code == 1
        report = json.loads(out_json.read_text(encoding="utf-8"))
        diff = report["provenance"]
        assert diff["grparse_commit"] == {"baseline": COMMIT_A, "candidate": COMMIT_B, "changed": True}
        assert diff["corpus"]["same"] is True
        changed = {svc["service"]: svc["changed"] for svc in diff["services"]}
        assert changed == {"grparse": True, "pdfium": False}
        assert report["deltas"]["overall"]["clean_rate_delta"] == -1.0
        assert report["deltas"]["by_class"]["set1"]["candidate_clean_rate"] == 0.0
        assert any("clean rate fell 100.00 points" in g for g in report["gate_failures"])
        markdown = (base / "drift.md").read_text(encoding="utf-8")
        assert "## What changed between the runs" in markdown
        assert f"`{COMMIT_A}` -> `{COMMIT_B}`" in markdown


def test_drift_refuses_different_corpora_unless_allowed() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        run1, run2 = _two_runs(base, second_files={"set1/c.docx": b"doc-c"})
        assert analyze.main(["--baseline", str(run1), "--candidate", str(run2), "--out-md", str(base / "d.md")]) == 2
        assert analyze.main([
            "--baseline", str(run1), "--candidate", str(run2), "--allow-corpus-mismatch", "--out-md", str(base / "d.md"),
        ]) == 0
        assert "DIFFERENT" in (base / "d.md").read_text(encoding="utf-8")


def test_baseline_auto_picks_the_earlier_run_on_the_same_corpus_and_list_runs() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        run1, run2 = _two_runs(base)
        out_json = base / "auto.json"
        analyze.main([str(run2), "--baseline-auto", str(run2.parent.parent), "--out-json", str(out_json)])
        report = json.loads(out_json.read_text(encoding="utf-8"))
        assert report["mode"] == "comparison"
        assert report["baseline"]["label"] == "run1"
        # The oldest run has nothing earlier to compare with.
        analyze.main([str(run1), "--baseline-auto", str(run1.parent.parent), "--out-json", str(out_json)])
        report = json.loads(out_json.read_text(encoding="utf-8"))
        assert report["mode"] == "single"
        assert report["notes"] and "no earlier run" in report["notes"][0]
        table = analyze.render_run_list(provenance.find_runs(run1.parent.parent))
        assert "`run1`" in table and "`run2`" in table and COMMIT_B[:12] in table


def test_battery_report_json_is_a_baseline_and_the_battery_rule_ignores_two_checks() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        battery = {
            "objects": [
                {"key": "napierone/doc/1.doc", "extension": "doc", "family": "word", "size": 5,
                 "parser_type": "libreoffice", "status": "CONVERSION_STATUS_SUCCESS", "elapsed_ms": 10.0,
                 "checks": {"parse_succeeds": "pass", "warnings_typed": "fail"}, "skipped": ""},
                {"key": "napierone/doc/2.doc", "extension": "doc", "family": "word", "size": 5,
                 "parser_type": "none", "status": "rpc error: UNAVAILABLE: 12 bytes", "elapsed_ms": 1.0,
                 "checks": {"parse_succeeds": "fail"}, "skipped": ""},
                {"key": "court/3.pdf", "extension": "pdf", "family": "pdf", "size": 5,
                 "parser_type": "grparse-cv", "status": "CONVERSION_STATUS_SUCCESS", "elapsed_ms": 10.0,
                 "checks": {"parse_succeeds": "pass"}, "skipped": ""},
            ],
        }
        path = base / "s3" / "report.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(battery), encoding="utf-8")
        summary = analyze.summarize_run(analyze.load_journal(path))
        overall = summary["overall"]
        assert overall["status_success_count"] == 2
        assert overall["clean_count"] == 1
        assert overall["clean_battery_count"] == 2
        assert overall["failure_causes"] == {"rpc_error: rpc error: UNAVAILABLE: # bytes": 1}
        assert summary["by_class"]["napierone/doc"]["clean_battery_count"] == 1
        assert summary["by_class"]["court"]["clean_count"] == 1
        md = base / "a.md"
        assert analyze.main([str(path), "--out-md", str(md)]) == 0
        text = md.read_text(encoding="utf-8")
        assert "provenance: not recorded" in text
        assert "Clean output, battery rule (ignores custom_field_keys, warnings_typed): 2/3 (66.7%)" in text
