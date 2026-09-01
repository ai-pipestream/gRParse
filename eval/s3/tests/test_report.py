from s3.battery import Matrix, evaluate, findings
from s3.report import build_report, render_markdown
from s3.tests.fixtures import context, word_document


def test_matrix_and_findings_group_by_check_and_cause() -> None:
    broken = word_document()
    broken["body"]["children"].append({"ref": "#/texts/50"})
    good = evaluate(context(word_document(), "r/a.docx"))
    bad = evaluate(context(broken, "r/b.docx"))
    matrix = Matrix()
    matrix.add(good)
    matrix.add(bad)
    rows = matrix.as_list()
    assert rows[0]["parser_type"] == "libreoffice" and rows[0]["objects"] == 2
    assert rows[0]["checks"]["integrity"] == {"files": 2, "passed": 1, "failed": 1}
    assert "sheet_tables" not in rows[0]["checks"]
    grouped = findings([good, bad])
    assert grouped and grouped[0]["objects"] == 1 and grouped[0]["keys"] == ["r/b.docx"]
    report = build_report(label="t", target="x", endpoint="e", bucket="b", prefix="", service="s",
                          results=[good, bad], matrix=matrix, wall_seconds=1.0, notes=["n"], exit_code=1)
    markdown = render_markdown(report)
    assert "| integrity | 2 | 1 | 1 |" in markdown and "## Findings" in markdown and "`r/b.docx`" in markdown
    assert report["per_check"]["integrity"] == {"files": 2, "pass": 1, "fail": 1}


def test_readme_documents_every_check() -> None:
    from pathlib import Path

    from s3.checks import CHECKS

    readme = (Path(__file__).resolve().parents[1] / "README.md").read_text()
    missing = [entry.name for entry in CHECKS if f"`{entry.name}`" not in readme]
    assert not missing, missing


def test_known_findings_name_their_owner() -> None:
    from s3.owners import KNOWN_FINDINGS, owner_of

    assert owner_of("warnings_typed", "collector warnings keyed as custom_fields strings").owner.startswith("gRParse")
    assert owner_of("table_grids", "grid rows have [#, #] cells, num_cols says #").owner == "grpc-markup"
    assert owner_of("integrity", "anything") is None
    assert all(entry.note for entry in KNOWN_FINDINGS)
    broken = word_document()
    broken["body"]["meta"] = {"custom_fields": {"collector_warnings:pdf": ["w"]}}
    result = evaluate(context(broken, "r/b.pdf"))
    grouped = findings([result])
    keyed = next(f for f in grouped if f["check"] == "warnings_typed")
    assert keyed["owner"] and "schema" in keyed["note"]
    report = build_report(label="t", target="x", endpoint="e", bucket="b", prefix="", service="s",
                          results=[result], matrix=Matrix(), wall_seconds=1.0, notes=[], exit_code=1)
    assert "- owner: gRParse (schema follow-on)" in render_markdown(report)
