"""One line of results.jsonl as a typed record.

run.py writes a ``JournalRecord`` per document and reads them back when it
resumes, hashes the corpus or refills the metrics; analyze.py reads them for
its summaries. ``from_json`` is strict: a line missing a field, or with a
field of the wrong type, raises ``JournalError`` naming the line, because a
journal is this harness's own output and a gap in it is a bug, not a zero.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .strict_json import SchemaError, optional, require, str_list, typed_map


class JournalError(SchemaError):
    """A results.jsonl line is not a well-formed journal record."""


@dataclass(frozen=True)
class JournalRecord:
    relpath: str
    sha256: str  # empty when the runner could not read the file
    size_bytes: int
    ext: str
    family: str
    doctype: str
    parser_type: str
    status: str  # normalized: success, partial_success, failure, rpc_error, skipped, ...
    raw_status: str
    rpc_error: str | None
    elapsed_ms: float
    page_count: int
    warnings_count: int
    checks: dict[str, str]
    failed_checks: tuple[str, ...]
    output_sha256: str  # empty when the parse produced no output
    artifact_md: str | None
    artifact_json: str | None
    counts: dict[str, int]
    collectors: tuple[str, ...]
    failures: tuple[dict[str, Any], ...]
    skipped: str  # the reason a document was skipped; empty when it was evaluated

    def to_json(self) -> dict[str, Any]:
        return {
            "relpath": self.relpath,
            "sha256": self.sha256,
            "size_bytes": self.size_bytes,
            "ext": self.ext,
            "family": self.family,
            "doctype": self.doctype,
            "parser_type": self.parser_type,
            "status": self.status,
            "raw_status": self.raw_status,
            "rpc_error": self.rpc_error,
            "elapsed_ms": self.elapsed_ms,
            "page_count": self.page_count,
            "warnings_count": self.warnings_count,
            "checks": dict(self.checks),
            "failed_checks": list(self.failed_checks),
            "output_sha256": self.output_sha256,
            "artifact_md": self.artifact_md,
            "artifact_json": self.artifact_json,
            "counts": dict(self.counts),
            "collectors": list(self.collectors),
            "failures": list(self.failures),
            "skipped": self.skipped,
        }

    @staticmethod
    def from_json(data: Any, where: str) -> JournalRecord:
        if not isinstance(data, dict):
            raise JournalError(f"{where}: not a JSON object")
        try:
            failures = require(data, "failures", list, where)
            if not all(isinstance(f, dict) for f in failures):
                raise JournalError(f"{where}: 'failures' holds a non-object")
            return JournalRecord(
                relpath=require(data, "relpath", str, where),
                sha256=require(data, "sha256", str, where),
                size_bytes=require(data, "size_bytes", int, where),
                ext=require(data, "ext", str, where),
                family=require(data, "family", str, where),
                doctype=require(data, "doctype", str, where),
                parser_type=require(data, "parser_type", str, where),
                status=require(data, "status", str, where),
                raw_status=require(data, "raw_status", str, where),
                rpc_error=optional(data, "rpc_error", str, where),
                elapsed_ms=float(require(data, "elapsed_ms", (int, float), where)),
                page_count=require(data, "page_count", int, where),
                warnings_count=require(data, "warnings_count", int, where),
                checks=typed_map(data, "checks", str, where),
                failed_checks=str_list(data, "failed_checks", where),
                output_sha256=require(data, "output_sha256", str, where),
                artifact_md=optional(data, "artifact_md", str, where),
                artifact_json=optional(data, "artifact_json", str, where),
                counts=typed_map(data, "counts", int, where),
                collectors=str_list(data, "collectors", where),
                failures=tuple(failures),
                skipped=require(data, "skipped", str, where),
            )
        except JournalError:
            raise
        except SchemaError as error:
            raise JournalError(str(error)) from error
