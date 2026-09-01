"""Corpus manifest: which documents the scorecard runs and where their bytes live.

In-repo documents live under ``corpus_root`` (relative to the repository);
external documents are referenced by absolute path and never copied in.
``EVAL_EXTERNAL_CORPUS`` names a directory that overrides the directory part
of every external path, so a checkout on another machine can point at its
own copy of the same files.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path(__file__).resolve().parent / "corpus.json"


@dataclass(frozen=True)
class CorpusDocument:
    """One manifest entry, resolved to a path that may or may not exist."""

    doc_id: str
    format: str
    content_type: str
    path: Path
    external: bool
    note: str = ""

    @property
    def present(self) -> bool:
        return self.path.is_file()

    @property
    def skip_reason(self) -> str | None:
        if self.present:
            return None
        kind = "external file" if self.external else "in-repo fixture"
        return f"{kind} missing: {self.path}"


def _resolve_external(raw: str, override_dir: str | None) -> Path:
    path = Path(raw)
    if override_dir:
        return Path(override_dir) / path.name
    return path


def load_manifest(manifest: Path = DEFAULT_MANIFEST, repo: Path = REPO) -> list[CorpusDocument]:
    """Read the manifest and resolve every entry; nothing is filtered here."""
    data = json.loads(manifest.read_text())
    root = repo / data.get("corpus_root", "tests/golden/corpus")
    override_dir = os.environ.get("EVAL_EXTERNAL_CORPUS") or None
    documents: list[CorpusDocument] = []
    seen: set[str] = set()
    for entry in data["documents"]:
        doc_id = entry["id"]
        if doc_id in seen:
            raise ValueError(f"duplicate document id in manifest: {doc_id}")
        seen.add(doc_id)
        if "external_path" in entry:
            path = _resolve_external(entry["external_path"], override_dir)
            external = True
        else:
            path = root / entry["path"]
            external = False
        documents.append(CorpusDocument(
            doc_id=doc_id, format=entry["format"], content_type=entry["content_type"],
            path=path, external=external, note=entry.get("note", ""),
        ))
    return documents


def select(documents: list[CorpusDocument], only: list[str] | None) -> list[CorpusDocument]:
    """Restrict to the ids in ``only`` (a document id or a format name)."""
    if not only:
        return documents
    wanted = set(only)
    return [d for d in documents if d.doc_id in wanted or d.format in wanted]
