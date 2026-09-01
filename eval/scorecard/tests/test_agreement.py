"""Agreement section: descriptive leaves never count as conflicts."""

from __future__ import annotations

from scorecard.agreement import agreement_section


def _doc(evidence_a: str, evidence_b: str) -> dict:
    return {
        "claims": [
            {"source": {"collector": "grparse"},
             "origin": {"mimetype": "text/html", "mimetype_evidence": evidence_a}},
            {"source": {"collector": "lol-html"},
             "origin": {"mimetype": "text/html", "mimetype_evidence": evidence_b}},
        ],
        "origin": {"field_sources": [{"field": "mimetype", "source": {"collector": "grparse"}}]},
    }


def test_mimetype_evidence_is_not_a_conflict() -> None:
    section = agreement_section(_doc("magic", "requested"))
    assert section is not None
    assert section["conflicts"] == []
    assert section["shared"] == 1
    assert section["agreed"] == 1


def test_real_conflicts_still_count() -> None:
    doc = _doc("magic", "magic")
    doc["claims"][1]["origin"]["mimetype"] = "application/octet-stream"
    section = agreement_section(doc)
    assert section is not None
    assert [c["field"] for c in section["conflicts"]] == ["origin.mimetype"]
    assert section["conflicts"][0]["winner"] == "grparse"
