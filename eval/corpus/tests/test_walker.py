"""Tests for the zero-stall streaming directory walker and O(1) companion lookup."""

from __future__ import annotations

import os
import tempfile
import time
from pathlib import Path

from corpus.run import walk_corpus


def test_walk_corpus_yields_first_file_immediately_without_full_scan() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for idx in range(25):
            sub = root / f"dir_{idx:02d}"
            sub.mkdir()
            (sub / f"doc_{idx:02d}.docx").write_bytes(b"PK-docx")

        scanned_dirs: list[Path] = []
        real_scandir = os.scandir

        def tracking_scandir(path: os.PathLike[str] | str):
            scanned_dirs.append(Path(path))
            return real_scandir(path)

        os.scandir = tracking_scandir  # type: ignore[assignment]
        try:
            started = time.perf_counter()
            iterator = walk_corpus(root)
            first = next(iterator)
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            assert first.ext == "docx"
            assert elapsed_ms < 50.0
            # Only the root directory and at most the first entered subdirectory
            # may have been opened by os.scandir before the first file is yielded.
            assert len(scanned_dirs) <= 2, f"expected lazy scan, but scanned {len(scanned_dirs)} dirs"
        finally:
            os.scandir = real_scandir  # type: ignore[assignment]


def test_walk_corpus_skips_ignored_dirs_hidden_files_and_layout_companions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / ".git").mkdir()
        (root / ".git" / "ignored.pdf").write_bytes(b"%PDF")
        (root / "node_modules").mkdir()
        (root / "node_modules" / "pkg.json").write_bytes(b"{}")
        (root / "__pycache__").mkdir()
        (root / "__pycache__" / "mod.pyc").write_bytes(b"0")
        (root / ".hidden_dir").mkdir()
        (root / ".hidden_dir" / "secret.docx").write_bytes(b"PK")
        (root / ".DS_Store").write_bytes(b"ds")
        (root / ".hidden.pdf").write_bytes(b"%PDF")
        (root / "standalone.layout.json").write_bytes(b"{}")
        (root / "valid").mkdir()
        (root / "valid" / "keep.pdf").write_bytes(b"%PDF-1.4")

        entries = list(walk_corpus(root))
        assert [e.relpath for e in entries] == ["valid/keep.pdf"]
        assert entries[0].size_bytes == 8
        assert entries[0].ext == "pdf"
        assert entries[0].family == "pdf"


def test_walk_corpus_ebcdic_companion_lookup_is_o1_on_disk() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "bank").mkdir()
        ebc_with = root / "bank" / "statement.ebc"
        layout = root / "bank" / "statement.layout.json"
        ebc_without = root / "bank" / "orphan.ebc"
        ebc_with.write_bytes(b"\xc1\xc2")
        layout.write_bytes(b'{"fields": []}')
        ebc_without.write_bytes(b"\xc3")

        by_rel = {e.relpath: e for e in walk_corpus(root)}
        assert sorted(by_rel) == ["bank/orphan.ebc", "bank/statement.ebc"]
        assert by_rel["bank/statement.ebc"].layout_path == layout.resolve()
        assert by_rel["bank/orphan.ebc"].layout_path is None


def test_walk_corpus_globs_max_docs_and_resume_keys() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "a.pdf").write_bytes(b"111")
        (root / "b.docx").write_bytes(b"2222")
        (root / "c.pdf").write_bytes(b"33333")

        pdf_only = list(walk_corpus(root, include=("*.pdf",)))
        assert sorted(e.relpath for e in pdf_only) == ["a.pdf", "c.pdf"]

        no_c = list(walk_corpus(root, exclude=("c.*",)))
        assert sorted(e.relpath for e in no_c) == ["a.pdf", "b.docx"]

        capped = list(walk_corpus(root, max_docs=2))
        assert len(capped) == 2

        resumed = list(walk_corpus(root, resume_keys={("a.pdf", 3), ("b.docx", 4)}))
        assert [e.relpath for e in resumed] == ["c.pdf"]
