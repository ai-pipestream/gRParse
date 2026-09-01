"""Integrity of the scored corpus: what it holds, what it weighs, what it must never hold.

These tests never dial a service and never read a model. They compare the
manifest, the fixture directory and the pinned digests against each other, so
a fixture that changes byte for byte, an entry that points at nothing, a file
nothing references, or a personal document that wandered in fails the suite
instead of quietly changing what every score means.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.corpus import DEFAULT_MANIFEST, REPO, load_manifest  # noqa: E402

CORPUS_ROOT = REPO / "tests" / "golden" / "corpus"
FIXTURE_MANIFEST = REPO / "eval" / "scorecard" / "fixtures" / "manifest.json"
SCORECARD_DIR = REPO / "eval" / "scorecard"

# No single scored file has any business being megabytes long: the corpus is
# read whole by several tests and shipped in the repository.
MAX_FILE_BYTES = 2 * 1024 * 1024
# The whole in-repo corpus, pinned well above today's size (882,713 bytes as
# of the lane-3 fixtures) and well below the point where a checkout hurts.
MAX_CORPUS_BYTES = 2_621_440  # raised 2026-09-01 for the turned-scan PDFs (was 1_572_864 at 34 docs)
# Personal documents that must never enter the corpus. Names, because that is
# what a stray copy would arrive under.
DENIED_NAMES = frozenset({"1786127653702.pdf", "792387684640_OUTBOUND_LABEL.pdf"})

REGENERATE = (
    "regenerate the fixtures with eval/scorecard/fixtures/build_all.sh, then rewrite the "
    "pinned digests with eval/scorecard/fixtures/write_manifest.py and say in the commit "
    "why the bytes moved"
)


def _corpus_files() -> list[Path]:
    return sorted(path for path in CORPUS_ROOT.iterdir() if path.is_file())


def _fixture_manifest() -> dict[str, dict[str, object]]:
    return json.loads(FIXTURE_MANIFEST.read_text())["files"]


def test_every_manifest_entry_points_at_a_file() -> None:
    missing = [doc.doc_id for doc in load_manifest() if not doc.external and not doc.path.is_file()]
    assert not missing, f"corpus.json entries with no file under {CORPUS_ROOT}: {missing}"


def test_no_orphan_corpus_files() -> None:
    referenced = {doc.path.name for doc in load_manifest() if not doc.external}
    present = {path.name for path in _corpus_files()}
    orphans = sorted(present - referenced)
    assert not orphans, (
        f"files under {CORPUS_ROOT} that no corpus.json entry names: {orphans}; "
        "add an entry or delete the file"
    )


def test_manifest_document_ids_are_unique_and_complete() -> None:
    ids = [doc.doc_id for doc in load_manifest()]
    assert len(ids) == len(set(ids)), "duplicate document ids in corpus.json"
    assert len(ids) >= 34, f"the corpus had 34 documents; found {len(ids)} (removals are deliberate, say why)"
    pinned = set(_fixture_manifest())
    referenced = {doc.path.name for doc in load_manifest() if not doc.external}
    assert referenced == pinned, (
        f"corpus.json and fixtures/manifest.json disagree: only in corpus {sorted(referenced - pinned)}, "
        f"only in manifest {sorted(pinned - referenced)}; run fixtures/write_manifest.py"
    )


def test_fixture_digests_match_the_pinned_manifest() -> None:
    pinned = _fixture_manifest()
    present = {path.name: path for path in _corpus_files()}
    assert sorted(pinned) == sorted(present), (
        f"the pinned manifest and {CORPUS_ROOT} disagree on which fixtures exist: "
        f"only pinned {sorted(set(pinned) - set(present))}, "
        f"only present {sorted(set(present) - set(pinned))}; {REGENERATE}"
    )
    moved = []
    for name, entry in sorted(pinned.items()):
        data = present[name].read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != entry["sha256"] or len(data) != entry["bytes"]:
            moved.append(f"{name}: pinned {entry['sha256'][:12]} ({entry['bytes']} bytes), "
                         f"found {digest[:12]} ({len(data)} bytes)")
    assert not moved, (
        "corpus fixtures changed byte for byte, so every recorded baseline and truth "
        f"floor now describes different bytes:\n  " + "\n  ".join(moved) + f"\n{REGENERATE}"
    )


def test_no_corpus_file_is_oversized() -> None:
    fat = [f"{path.name} ({path.stat().st_size} bytes)"
           for path in _corpus_files() if path.stat().st_size > MAX_FILE_BYTES]
    assert not fat, f"corpus fixtures above {MAX_FILE_BYTES} bytes: {fat}"


def test_no_scorecard_file_is_oversized() -> None:
    fat = []
    for path in sorted(SCORECARD_DIR.rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        size = path.stat().st_size
        if size > MAX_FILE_BYTES:
            fat.append(f"{path.relative_to(REPO)} ({size} bytes)")
    assert not fat, f"scorecard files above {MAX_FILE_BYTES} bytes: {fat}"


def test_corpus_stays_under_its_size_ceiling() -> None:
    total = sum(path.stat().st_size for path in _corpus_files())
    assert total <= MAX_CORPUS_BYTES, (
        f"the in-repo corpus is {total} bytes, above the pinned ceiling of "
        f"{MAX_CORPUS_BYTES}; shrink a fixture rather than raising the ceiling by reflex"
    )


def test_personal_documents_never_enter_the_tree() -> None:
    found = []
    for root in (CORPUS_ROOT, SCORECARD_DIR):
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.name in DENIED_NAMES:
                found.append(str(path.relative_to(REPO)))
    assert not found, f"personal documents must never enter the corpus: {found}"


def test_denied_names_are_not_referenced_by_the_manifest() -> None:
    named = [doc.doc_id for doc in load_manifest() if doc.path.name in DENIED_NAMES]
    assert not named, f"corpus.json references a personal document: {named}"


def test_corpus_root_is_where_the_manifest_says() -> None:
    declared = json.loads(DEFAULT_MANIFEST.read_text())["corpus_root"]
    assert declared == "tests/golden/corpus", f"corpus_root moved to {declared}"
    assert json.loads(FIXTURE_MANIFEST.read_text())["corpus_root"] == declared, (
        "the fixture manifest pins digests under a different root than corpus.json reads"
    )
