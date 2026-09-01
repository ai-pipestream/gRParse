import json
import tempfile
from pathlib import Path

from s3.run import SKIP, companion_layout, main
from s3.source import ObjectRef, select_keys
from s3.tests.fixtures import FakeClient, FakeSource, result, scan_document, word_document

ENV = {"EVAL_S3_ENDPOINT": "http://127.0.0.1:1", "EVAL_S3_BUCKET": "b", "EVAL_S3_ACCESS_KEY": "k",
       "EVAL_S3_SECRET_KEY": "s", "GRPARSE_TARGET": "fake:1", "EVAL_LABEL": "unit"}


def _env(out: Path, **extra: str) -> dict[str, str]:
    return dict(ENV, EVAL_OUT=str(out), **extra)


def _run(objects: dict[str, bytes], answers: dict, env_extra: dict | None = None, fail: bool = False,
         sniff_fails: bool = False, outage_ext: str = "", die_after: int | None = None):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        source = FakeSource(objects)
        clients: list[FakeClient] = []

        def client_factory(target: str) -> FakeClient:
            clients.append(FakeClient(target, answers, fail=fail, sniff_fails=sniff_fails, outage_ext=outage_ext,
                                      die_after=die_after))
            return clients[-1]

        code = main([], _env(out, **(env_extra or {})), source_factory=lambda config: source, client_factory=client_factory)
        report_path = out / "s3" / "unit" / "report.json"
        report = json.loads(report_path.read_text()) if report_path.is_file() else None
        markdown = (out / "s3" / "unit" / "report.md").read_text() if report else ""
        written = sorted(str(p.relative_to(out)) for p in out.rglob("*") if p.is_file())
        return code, report, markdown, source, clients, written


def test_all_pass_exits_zero_and_writes_only_reports() -> None:
    objects = {"repo/a.docx": b"PK-docx", "repo/b.png": b"\x89PNG"}
    answers = {"docx": result(word_document()), "png": result(scan_document())}
    code, report, markdown, source, clients, written = _run(objects, answers)
    assert code == 0, report["findings"] if report else None
    assert written == ["s3/unit/report.json", "s3/unit/report.md"]
    assert report["totals"]["evaluated"] == 2 and report["totals"]["checks_failed"] == 0
    assert {row["parser_type"] for row in report["matrix"]} == {"libreoffice", "grparse-cv"}
    # two parses per object plus one extension-less sniff per extension
    names = [call[0] for call in clients[0].calls]
    assert names.count("a.docx") == 2 and names.count("a") == 1 and names.count("b") == 1
    assert "| libreoffice | docx | 1 |" in markdown and sorted(source.fetched) == ["repo/a.docx", "repo/b.png"]


def test_sniff_run_failure_is_its_own_finding() -> None:
    code, report, *_ = _run({"repo/a.docx": b"x"}, {"docx": result(word_document())}, sniff_fails=True)
    assert code == 1 and [f["check"] for f in report["findings"]] == ["sniff_route"]


def test_any_failure_exits_one_with_findings() -> None:
    broken = word_document()
    broken["body"]["children"].append({"ref": "#/texts/50"})
    code, report, markdown, *_ = _run({"repo/a.docx": b"x"}, {"docx": result(broken)})
    assert code == 1 and report["exit_code"] == 1
    checks = {f["check"] for f in report["findings"]}
    assert "integrity" in checks and "placement" in checks
    assert "### integrity" in markdown


def test_unreachable_and_empty_selection_skip() -> None:
    code, report, *_ = _run({"repo/a.docx": b"x"}, {}, fail=True)
    assert code == SKIP and report is None
    code, report, *_ = _run({}, {})
    assert code == SKIP and report is None
    code, *_ = _run({"repo/a.docx": b"x"}, {"docx": result(word_document())}, {"EVAL_S3_INCLUDE": "*.pdf"})
    assert code == SKIP


def test_missing_configuration_skips_or_fails_under_require() -> None:
    assert main([], {"EVAL_S3_ENDPOINT": "x"}, source_factory=lambda c: None, client_factory=lambda t: None) == SKIP
    assert main([], {"EVAL_S3_ENDPOINT": "x", "EVAL_REQUIRE": "1"}, source_factory=lambda c: None,
                client_factory=lambda t: None) == 1


def test_ebcdic_uses_the_companion_layout_or_skips() -> None:
    refs = [ObjectRef("r/statement.ebc", 1), ObjectRef("r/statement.layout.json", 1), ObjectRef("r/other.ebc", 1)]
    assert companion_layout("r/statement.ebc", refs) == "r/statement.layout.json"
    assert companion_layout("r/other.ebc", refs) is None
    objects = {"r/statement.ebc": b"\xc1", "r/statement.layout.json": b"{}", "r/other.ebc": b"\xc2"}
    code, report, markdown, source, clients, _ = _run(objects, {"ebc": result(word_document())})
    assert report["totals"]["skipped"] == 1 and "no <stem>.layout.json" in markdown
    layouts = [call for call in clients[0].calls if call[1] == ("EBCDIC",)]
    assert layouts and all(call[2] == b"{}" for call in layouts)
    assert not any(call[0] == "statement" for call in clients[0].calls), "no sniff run for EBCDIC"
    assert code == 0
    code, *_ = _run(objects, {"ebc": result(word_document())}, {"EVAL_REQUIRE": "1"})
    assert code == 1


def test_max_objects_and_globs() -> None:
    refs = [ObjectRef("a/x.pdf", 1), ObjectRef("a/y.docx", 1), ObjectRef("b/z.pdf", 1), ObjectRef("dir/", 0)]
    assert [r.key for r in select_keys(refs, ("*.pdf",), (), None)] == ["a/x.pdf", "b/z.pdf"]
    assert [r.key for r in select_keys(refs, (), ("b/*",), None)] == ["a/x.pdf", "a/y.docx"]
    assert len(select_keys(refs, (), (), 1)) == 1
    code, report, *_ = _run({"r/a.docx": b"1", "r/b.docx": b"2"}, {"docx": result(word_document())},
                            {"EVAL_S3_MAX_OBJECTS": "1"})
    assert code == 0 and report["totals"]["objects"] == 1


def test_collector_outage_is_the_objects_failure_not_the_runs() -> None:
    objects = {"r/a.docx": b"1", "r/b.html": b"2"}
    answers = {"docx": result(word_document()), "html": result(word_document())}
    code, report, markdown, *_ = _run(objects, answers, outage_ext="html")
    assert code == 1 and report["totals"]["evaluated"] == 2
    finding = next(f for f in report["findings"] if f["check"] == "parse_succeeds")
    assert finding["keys"] == ["r/b.html"] and "UNAVAILABLE" in finding["cause"]


def test_a_dead_service_ends_the_run_with_a_partial_report() -> None:
    objects = {"r/a.docx": b"1", "r/b.docx": b"2", "r/c.docx": b"3"}
    code, report, markdown, *_ = _run(objects, {"docx": result(word_document())}, die_after=3)
    assert code == SKIP and report is not None
    assert report["totals"]["evaluated"] == 1 and any("cut short" in note for note in report["notes"])
