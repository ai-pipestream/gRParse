#!/usr/bin/env python3
"""Zero-stall streaming local-directory corpus runner for gRParse.

    uv run --with grpcio --with grpcio-tools python eval/corpus/run.py --corpus-dir /path/to/corpus

Streams documents lazily via recursive os.scandir without pre-listing or
sorting the directory tree, bounds in-flight worker tasks to 2 * workers,
journals every completed document immediately to results.jsonl, writes
sharded outputs under outputs/<sha256[:2]>/<sha256>.{md,json}, and exports
live Prometheus telemetry.

Exit codes: 0 every check passed on every evaluated document; 1 any failure
(or a skip under EVAL_REQUIRE / --require); 77 when configuration is missing,
the corpus directory is missing or empty, or gRParse is unreachable.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import signal
import sys
import threading
import time
from collections.abc import Callable, Iterator, Mapping
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from s3.battery import Matrix, ObjectResult, evaluate, findings, skipped  # noqa: E402
from s3.checks import CHECKS, ObjectContext  # noqa: E402
from s3.config import ConfigError, _flag, _globs, _int, _passwords  # noqa: E402
from s3.document import View  # noqa: E402
from s3.formats import extension_of, family_of  # noqa: E402
from s3.run import LAYOUT_SUFFIX, convert_object, skip  # noqa: E402
from s3.sourcefacts import pdf_page_count, source_facts  # noqa: E402

from corpus.metrics import MetricsCollector, MetricsServer, normalize_status  # noqa: E402

SKIP = 77
REPO = EVAL_DIR.parent

SKIPPED_DIRS: frozenset[str] = frozenset({
    ".git",
    "node_modules",
    "__pycache__",
    ".venv",
    "venv",
    ".tox",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
})
SKIPPED_FILES: frozenset[str] = frozenset({
    ".DS_Store",
    "Thumbs.db",
})


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.parent / f".{path.name}.tmp.{os.getpid()}.{threading.get_ident()}"
    tmp.write_text(content, encoding="utf-8")
    os.replace(tmp, path)


@dataclass(frozen=True)
class CorpusConfig:
    corpus_dir: Path
    target: str
    out: Path
    label: str
    workers: int = 4
    repeat: int = 1
    include: tuple[str, ...] = ()
    exclude: tuple[str, ...] = ()
    max_docs: int | None = None
    resume: bool = False
    save_outputs: bool = True
    metrics_port: int | None = 0
    sniff_per_extension: int = 0
    require: bool = False
    convert_timeout: float = 600.0
    seconds_per_page: float = 1.5
    checkpoint_interval: int = 50
    document_passwords: tuple[str, ...] = field(default=(), repr=False)

    @property
    def max_in_flight(self) -> int:
        return max(1, 2 * self.workers)

    def deadline_for(self, pages: int) -> float:
        return max(self.convert_timeout, pages * self.seconds_per_page)

    @classmethod
    def from_cli_and_env(
        cls,
        argv: list[str] | None = None,
        env: Mapping[str, str] | None = None,
        repo_root: Path = REPO,
    ) -> CorpusConfig:
        env = os.environ if env is None else env
        parser = argparse.ArgumentParser(
            description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument("pos_corpus_dir", nargs="?", help="path to local corpus directory")
        parser.add_argument("--corpus-dir", dest="corpus_dir", help="path to local corpus directory (or EVAL_CORPUS_DIR)")
        parser.add_argument("--target", help="gRParse host:port (or GRPARSE_TARGET, default localhost:50051)")
        parser.add_argument("--out", type=Path, help="output root directory (or EVAL_OUT, default eval/out)")
        parser.add_argument("--label", help="run label (or EVAL_LABEL, default live)")
        parser.add_argument("--workers", type=int, help="concurrent worker threads (or EVAL_CONCURRENCY, default 4)")
        parser.add_argument("--repeat", type=int, help="conversions per document (or EVAL_CORPUS_REPEAT, default 1)")
        parser.add_argument("--include", action="append", default=[], help="comma-separated glob patterns to include")
        parser.add_argument("--exclude", action="append", default=[], help="comma-separated glob patterns to exclude")
        parser.add_argument("--max-docs", type=int, help="stop after evaluating N documents (or EVAL_CORPUS_MAX_DOCS)")
        parser.add_argument(
            "--resume",
            action=argparse.BooleanOptionalAction,
            default=None,
            help="skip files already recorded in results.jsonl (or EVAL_CORPUS_RESUME)",
        )
        parser.add_argument(
            "--save-outputs",
            action=argparse.BooleanOptionalAction,
            default=None,
            help="write outputs/<sha256[:2]>/<sha256>.{md,json} (default: true)",
        )
        parser.add_argument(
            "--metrics-port",
            type=int,
            default=None,
            help="HTTP port for /metrics (0 for ephemeral, negative to disable; or EVAL_RUNNER_METRICS_PORT)",
        )
        parser.add_argument(
            "--sniff-per-extension",
            type=int,
            default=None,
            help="documents per extension also parsed without an extension (default 0)",
        )
        parser.add_argument("--convert-timeout", type=float, default=None, help="per-document timeout in seconds")
        parser.add_argument("--seconds-per-page", type=float, default=None, help="extra deadline per PDF page in seconds")
        parser.add_argument(
            "--require",
            action=argparse.BooleanOptionalAction,
            default=None,
            help="treat skipped documents or missing prerequisites as failure (or EVAL_REQUIRE)",
        )
        parser.add_argument("--passwords-file", help="optional UTF-8 file of candidate document passwords")

        args = parser.parse_args(argv or [])

        raw_dir = args.corpus_dir or args.pos_corpus_dir or (env.get("EVAL_CORPUS_DIR") or "").strip()
        if not raw_dir:
            raise ConfigError("missing corpus directory: pass --corpus-dir or set EVAL_CORPUS_DIR")
        corpus_dir = Path(raw_dir)

        target = (args.target or env.get("GRPARSE_TARGET") or "localhost:50051").strip()
        out = args.out or Path(env.get("EVAL_OUT") or (repo_root / "eval" / "out"))
        label = (args.label or env.get("EVAL_LABEL") or "live").strip()

        workers = args.workers if args.workers is not None else (_int(env, "EVAL_CONCURRENCY", 4) or 4)
        if workers < 1:
            raise ConfigError(f"--workers / EVAL_CONCURRENCY must be at least 1, got {workers}")

        repeat = args.repeat if args.repeat is not None else (_int(env, "EVAL_CORPUS_REPEAT", 1) or 1)
        if repeat < 1:
            raise ConfigError(f"--repeat / EVAL_CORPUS_REPEAT must be at least 1, got {repeat}")

        inc_parts = list(_globs(env.get("EVAL_CORPUS_INCLUDE")))
        for item in args.include:
            inc_parts.extend(_globs(item))
        exc_parts = list(_globs(env.get("EVAL_CORPUS_EXCLUDE")))
        for item in args.exclude:
            exc_parts.extend(_globs(item))

        max_docs = args.max_docs if args.max_docs is not None else _int(env, "EVAL_CORPUS_MAX_DOCS", None)
        if max_docs is not None and max_docs < 1:
            raise ConfigError(f"--max-docs must be positive, got {max_docs}")

        resume = args.resume if args.resume is not None else _flag(env, "EVAL_CORPUS_RESUME", False)
        save_outputs = args.save_outputs if args.save_outputs is not None else _flag(env, "EVAL_CORPUS_SAVE_OUTPUTS", True)
        require = args.require if args.require is not None else _flag(env, "EVAL_REQUIRE", False)

        if args.metrics_port is not None:
            metrics_port: int | None = args.metrics_port
        elif (env.get("EVAL_RUNNER_METRICS_PORT") or "").strip() != "":
            try:
                metrics_port = int(env["EVAL_RUNNER_METRICS_PORT"])
            except ValueError as err:
                raise ConfigError(
                    f"EVAL_RUNNER_METRICS_PORT must be an integer, got {env['EVAL_RUNNER_METRICS_PORT']!r}"
                ) from err
        else:
            metrics_port = 0

        sniff = (
            args.sniff_per_extension
            if args.sniff_per_extension is not None
            else (_int(env, "EVAL_CORPUS_SNIFF_PER_EXTENSION", 0) or 0)
        )
        timeout = (
            float(args.convert_timeout)
            if args.convert_timeout is not None
            else float(_int(env, "EVAL_CORPUS_CONVERT_TIMEOUT", 600) or 600)
        )
        spp = (
            float(args.seconds_per_page)
            if args.seconds_per_page is not None
            else float(env.get("EVAL_CORPUS_SECONDS_PER_PAGE") or 1.5)
        )

        pwd_env = dict(env)
        if args.passwords_file:
            pwd_env["EVAL_S3_PASSWORDS_FILE"] = args.passwords_file
        elif env.get("EVAL_CORPUS_PASSWORDS_FILE"):
            pwd_env["EVAL_S3_PASSWORDS_FILE"] = env["EVAL_CORPUS_PASSWORDS_FILE"]
        passwords = _passwords(pwd_env)

        return cls(
            corpus_dir=corpus_dir,
            target=target,
            out=out,
            label=label,
            workers=workers,
            repeat=repeat,
            include=tuple(inc_parts),
            exclude=tuple(exc_parts),
            max_docs=max_docs,
            resume=resume,
            save_outputs=save_outputs,
            metrics_port=metrics_port,
            sniff_per_extension=sniff,
            require=require,
            convert_timeout=timeout,
            seconds_per_page=spp,
            document_passwords=passwords,
        )


@dataclass(frozen=True)
class CorpusEntry:
    """One discovered file yielded lazily by walk_corpus."""

    path: Path
    relpath: str
    size_bytes: int
    ext: str
    family: str
    layout_path: Path | None = None


def _matches_any(relpath: str, name: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatch(relpath, pat) or fnmatch.fnmatch(name, pat) for pat in patterns)


def companion_layout_path(entry_path: Path) -> Path | None:
    """Check in O(1) directly on disk whether <stem>.layout.json sits beside an .ebc file."""
    candidate = entry_path.with_suffix(LAYOUT_SUFFIX)
    return candidate if candidate.is_file() else None


def walk_corpus(
    corpus_dir: Path,
    *,
    include: tuple[str, ...] = (),
    exclude: tuple[str, ...] = (),
    resume_keys: set[tuple[str, int]] | None = None,
    max_docs: int | None = None,
) -> Iterator[CorpusEntry]:
    """Stream files lazily from ``corpus_dir`` using recursive ``os.scandir``.

    Yields the first matching document immediately without pre-listing or
    sorting the full tree. Skips hidden files and directories, ``.git``,
    ``node_modules``, ``__pycache__``, ``.DS_Store``, and ``*.layout.json``
    companion files. Resolves ``.ebc`` companion layouts in O(1) via
    ``Path(entry.path).with_suffix(".layout.json").is_file()``.
    """
    root = corpus_dir.resolve()
    yielded = 0

    def _scan_dir(current: Path, rel_prefix: str) -> Iterator[CorpusEntry]:
        nonlocal yielded
        try:
            scanner = os.scandir(current)
        except OSError:
            return
        with scanner as it:
            for entry in it:
                if max_docs is not None and yielded >= max_docs:
                    return
                name = entry.name
                if name.startswith(".") or name in SKIPPED_FILES:
                    continue
                try:
                    is_dir = entry.is_dir(follow_symlinks=False)
                except OSError:
                    continue
                if is_dir:
                    if name in SKIPPED_DIRS:
                        continue
                    sub_prefix = f"{rel_prefix}/{name}" if rel_prefix else name
                    yield from _scan_dir(Path(entry.path), sub_prefix)
                    continue
                try:
                    if not entry.is_file(follow_symlinks=False):
                        continue
                    size_bytes = entry.stat(follow_symlinks=False).st_size
                except OSError:
                    continue
                if name.lower().endswith(LAYOUT_SUFFIX):
                    continue
                relpath = f"{rel_prefix}/{name}" if rel_prefix else name
                if include and not _matches_any(relpath, name, include):
                    continue
                if exclude and _matches_any(relpath, name, exclude):
                    continue
                if resume_keys is not None and (relpath, size_bytes) in resume_keys:
                    continue
                entry_path = Path(entry.path)
                ext = extension_of(name)
                family = family_of(ext)
                layout_path = companion_layout_path(entry_path) if family == "ebcdic" else None
                yielded += 1
                yield CorpusEntry(
                    path=entry_path,
                    relpath=relpath,
                    size_bytes=size_bytes,
                    ext=ext,
                    family=family,
                    layout_path=layout_path,
                )

    yield from _scan_dir(root, "")


def load_resume_journal(journal_path: Path) -> tuple[set[tuple[str, int]], list[dict[str, Any]]]:
    """Read existing results.jsonl in O(N), returning ((relpath, size_bytes) set, records)."""
    keys: set[tuple[str, int]] = set()
    records: list[dict[str, Any]] = []
    if not journal_path.is_file():
        return keys, records
    with journal_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            relpath = record.get("relpath")
            size_bytes = record.get("size_bytes")
            if isinstance(relpath, str) and isinstance(size_bytes, int):
                keys.add((relpath, size_bytes))
                records.append(record)
    return keys, records


def _record_to_object_result(record: dict[str, Any]) -> ObjectResult:
    return ObjectResult(
        key=str(record.get("relpath") or ""),
        extension=str(record.get("ext") or ""),
        family=str(record.get("family") or "other"),
        size=int(record.get("size_bytes") or 0),
        parser_type=str(record.get("parser_type") or "none"),
        collectors=list(record.get("collectors") or []),
        status=str(record.get("raw_status") or record.get("status") or ""),
        elapsed_ms=float(record.get("elapsed_ms") or 0.0),
        counts=dict(record.get("counts") or {}),
        checks=dict(record.get("checks") or {}),
        failures=list(record.get("failures") or []),
        skipped=str(record.get("skipped") or ""),
    )


def evaluate_corpus_entry(
    config: CorpusConfig,
    client: Any,
    entry: CorpusEntry,
    *,
    do_sniff: bool,
    run_out: Path,
) -> tuple[dict[str, Any], ObjectResult, str | None, str]:
    """Convert and evaluate one local file, writing sharded outputs when enabled."""
    doctype = entry.ext or entry.family or "unknown"
    if entry.family == "ebcdic" and entry.layout_path is None:
        obj_res = skipped(
            entry.relpath,
            entry.ext,
            entry.family,
            entry.size_bytes,
            "no <stem>.layout.json beside the .ebc file",
        )
        try:
            raw_bytes = entry.path.read_bytes()
            input_sha256 = hashlib.sha256(raw_bytes).hexdigest()
        except OSError:
            input_sha256 = ""
        record: dict[str, Any] = {
            "relpath": entry.relpath,
            "sha256": input_sha256,
            "size_bytes": entry.size_bytes,
            "ext": entry.ext,
            "family": entry.family,
            "doctype": doctype,
            "parser_type": "none",
            "status": "skipped",
            "raw_status": "skipped",
            "rpc_error": None,
            "elapsed_ms": 0.0,
            "page_count": 0,
            "warnings_count": 0,
            "checks": {},
            "failed_checks": [],
            "output_sha256": "",
            "artifact_md": None,
            "artifact_json": None,
            "counts": {},
            "collectors": [],
            "failures": [],
            "skipped": obj_res.skipped,
        }
        return record, obj_res, None, f"-- {entry.relpath}: skipped (no layout)"

    data = entry.path.read_bytes()
    input_sha256 = hashlib.sha256(data).hexdigest()
    layout = entry.layout_path.read_bytes() if entry.layout_path is not None else None
    pages_hint = pdf_page_count(data) if entry.ext == "pdf" else 0
    runs, sniff_res, dropped = convert_object(
        client,
        entry.relpath,
        data,
        entry.ext,
        repeat=config.repeat,
        sniff=do_sniff,
        layout=layout,
        timeout=config.deadline_for(pages_hint),
        passwords=config.document_passwords,
    )
    note = None
    if dropped:
        note = (
            f"{entry.relpath}: over the unary response limit with every export; "
            f"checked without {', '.join(dropped)}"
        )
    first = runs[0]
    doc_dict = first.document if (first.document and not first.rpc_error) else {}
    view = View(doc_dict) if doc_dict else None
    ctx = ObjectContext(
        key=entry.relpath,
        ext=entry.ext,
        family=entry.family,
        size=entry.size_bytes,
        facts=source_facts(entry.ext, entry.family, data),
        runs=runs,
        sniff=sniff_res,
        view=view,
    )
    del data
    obj_res = evaluate(ctx)

    md_text = first.markdown or (first.exports or {}).get("md", "") or ""
    canonical_export = (first.exports or {}).get("canonical_json", "")
    if canonical_export and not (canonical_export == "{}" and doc_dict):
        json_text = canonical_export if canonical_export.endswith("\n") else canonical_export + "\n"
    elif doc_dict:
        json_text = json.dumps(doc_dict, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
    else:
        json_text = ""

    if not first.rpc_error and (doc_dict or md_text):
        canonical_doc_str = json.dumps(doc_dict, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        output_sha256 = hashlib.sha256((canonical_doc_str + "\n" + md_text).encode("utf-8")).hexdigest()
    else:
        output_sha256 = ""

    artifact_md: str | None = None
    artifact_json: str | None = None
    if config.save_outputs and not first.rpc_error and (doc_dict or md_text):
        shard = input_sha256[:2]
        rel_md = f"outputs/{shard}/{input_sha256}.md"
        rel_json = f"outputs/{shard}/{input_sha256}.json"
        _atomic_write_text(run_out / rel_md, md_text)
        _atomic_write_text(run_out / rel_json, json_text or "{}\n")
        artifact_md = rel_md
        artifact_json = rel_json

    failed_checks = sorted(name for name, verdict in obj_res.checks.items() if verdict == "fail")
    norm_status = normalize_status(first.status, first.rpc_error, skipped=False)
    warnings_count = len((doc_dict.get("warnings") if isinstance(doc_dict, dict) else None) or [])
    page_count = int(obj_res.counts.get("pages", 0))

    record = {
        "relpath": entry.relpath,
        "sha256": input_sha256,
        "size_bytes": entry.size_bytes,
        "ext": entry.ext,
        "family": entry.family,
        "doctype": doctype,
        "parser_type": obj_res.parser_type,
        "status": norm_status,
        "raw_status": obj_res.status,
        "rpc_error": first.rpc_error,
        "elapsed_ms": obj_res.elapsed_ms,
        "page_count": page_count,
        "warnings_count": warnings_count,
        "checks": obj_res.checks,
        "failed_checks": failed_checks,
        "output_sha256": output_sha256,
        "artifact_md": artifact_md,
        "artifact_json": artifact_json,
        "counts": obj_res.counts,
        "collectors": obj_res.collectors,
        "failures": obj_res.failures,
        "skipped": "",
    }
    mark = "!!" if failed_checks else "=="
    suffix = f" [{', '.join(failed_checks)}]" if failed_checks else ""
    line = f"{mark} {entry.relpath}: {obj_res.parser_type} {obj_res.status} ({obj_res.elapsed_ms:.0f} ms){suffix}"
    return record, obj_res, note, line


def build_corpus_report(
    *,
    label: str,
    target: str,
    corpus_dir: Path,
    service: str,
    results: list[ObjectResult],
    matrix: Matrix,
    wall_seconds: float,
    notes: list[str],
    exit_code: int,
    resumed_count: int = 0,
    metrics_url: str | None = None,
) -> dict[str, Any]:
    evaluated = [r for r in results if not r.skipped]
    failed_objects = sum(1 for r in evaluated if r.failed)
    per_check = matrix.per_check()
    report: dict[str, Any] = {
        "label": label,
        "target": target,
        "corpus_dir": str(corpus_dir),
        "service": service,
        "wall_seconds": round(wall_seconds, 2),
        "exit_code": exit_code,
        "resumed": resumed_count,
        "totals": {
            "objects": len(results),
            "evaluated": len(evaluated),
            "skipped": len(results) - len(evaluated),
            "failed_objects": failed_objects,
            "checks_run": sum(c.files for c in per_check.values()),
            "checks_failed": sum(c.failed for c in per_check.values()),
        },
        "per_check": {name: {"files": c.files, "pass": c.passed, "fail": c.failed} for name, c in per_check.items()},
        "matrix": matrix.as_list(),
        "findings": findings(results),
        "notes": list(notes),
        "objects": [r.__dict__ for r in results],
        "checks": {entry.name: entry.doc for entry in CHECKS},
    }
    if metrics_url:
        report["metrics_url"] = metrics_url
    return report


def _md_escape(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_corpus_markdown(report: dict[str, Any]) -> str:
    totals = report["totals"]
    lines = [
        f"# gRParse corpus eval: {report['label']}",
        "",
        f"target `{report['target']}` service `{report['service']}` corpus `{report['corpus_dir']}`",
        "",
        f"documents: {totals['objects']} total ({report.get('resumed', 0)} resumed), "
        f"{totals['evaluated']} evaluated, {totals['skipped']} skipped, "
        f"{totals['failed_objects']} with failures; checks: {totals['checks_run']} run, "
        f"{totals['checks_failed']} failed; wall {report['wall_seconds']}s; exit {report['exit_code']}",
        "",
    ]
    for note in report.get("notes", []):
        lines.append(f"- {note}")
    if report.get("notes"):
        lines.append("")
    lines += ["## Checks", "", "| check | files | pass | fail |", "|---|---|---|---|"]
    for name, cell in report["per_check"].items():
        lines.append(f"| {name} | {cell['files']} | {cell['pass']} | {cell['fail']} |")
    lines += [
        "",
        "## Matrix: parser type x file type",
        "",
        "| parser type | ext | objects | failing checks (fail/files) |",
        "|---|---|---|---|",
    ]
    for row in report["matrix"]:
        failing = ", ".join(
            f"{name} {cell['failed']}/{cell['files']}"
            for name, cell in row["checks"].items()
            if cell["failed"]
        )
        lines.append(f"| {row['parser_type']} | {row['extension']} | {row['objects']} | {failing or '-'} |")
    lines += ["", "## Findings (failures grouped by check and cause)", ""]
    if not report["findings"]:
        lines.append("none")
    for finding in report["findings"]:
        lines.append(f"### {finding['check']}: {_md_escape(finding['cause'])}")
        lines.append("")
        lines.append(
            f"- objects: {finding['objects']}; parser types: {', '.join(finding['parser_types'])}; "
            f"extensions: {', '.join(finding['extensions'])}; "
            f"collectors: {', '.join(finding['collectors']) or '-'}"
        )
        lines.append("- keys: " + ", ".join(f"`{k}`" for k in finding["keys"]))
        if finding.get("owner"):
            lines.append(f"- owner: {finding['owner']}; {finding['note']}")
        else:
            lines.append("- owner: not yet triaged (eval/s3/owners.py names the known ones)")
        evidence = json.dumps(finding["evidence"], ensure_ascii=False, sort_keys=True)
        lines.append(f"- evidence (first object): `{_md_escape(evidence[:600])}`")
        lines.append("")
    lines += [
        "## Objects",
        "",
        "| relpath | ext | parser type | status | ms | texts | tables | pictures | pages | failed checks |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for obj in report["objects"]:
        counts = obj.get("counts") or {}
        failed = ", ".join(name for name, verdict in obj["checks"].items() if verdict == "fail")
        status = obj["skipped"] and f"skipped: {obj['skipped']}" or obj["status"]
        lines.append(
            f"| `{_md_escape(obj['key'])}` | {obj['extension']} | {obj['parser_type']} | "
            f"{_md_escape(status)[:60]} | {obj['elapsed_ms']:.0f} | {counts.get('texts', '')} | "
            f"{counts.get('tables', '')} | {counts.get('pictures', '')} | {counts.get('pages', '')} | "
            f"{failed or '-'} |"
        )
    skipped_objs = [o for o in report["objects"] if o["skipped"]]
    if skipped_objs:
        lines += ["", "## Skipped", ""]
        lines += [f"- `{_md_escape(o['key'])}`: {o['skipped']}" for o in skipped_objs]
    return "\n".join(lines) + "\n"


def write_corpus_report(out: Path, report: dict[str, Any], metrics: MetricsCollector) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    _atomic_write_text(
        out / "report.json",
        json.dumps(report, indent=1, sort_keys=True, ensure_ascii=False) + "\n",
    )
    _atomic_write_text(out / "report.md", render_corpus_markdown(report))
    metrics.write_prom_file(out / "metrics.prom")
    return out / "report.md"


def evaluate_corpus(
    config: CorpusConfig,
    client: Any,
    *,
    run_out: Path,
    results: list[ObjectResult],
    matrix: Matrix,
    notes: list[str],
    metrics: MetricsCollector,
    service: str,
    started: float,
    metrics_url: str | None = None,
    on_start: Callable[[str | None], None] | None = None,
) -> int:
    """Stream walk_corpus through a bounded ThreadPoolExecutor (max_in_flight = 2 * workers)."""
    run_out.mkdir(parents=True, exist_ok=True)
    journal_path = run_out / "results.jsonl"

    resume_keys: set[tuple[str, int]] = set()
    resumed_count = 0
    if config.resume and journal_path.is_file():
        resume_keys, prior_records = load_resume_journal(journal_path)
        for rec in prior_records:
            obj_res = _record_to_object_result(rec)
            results.append(obj_res)
            if not obj_res.skipped:
                matrix.add(obj_res)
            metrics.add_discovered(1)
            metrics.record_journal_entry(rec)
            resumed_count += 1
    elif not config.resume and journal_path.exists():
        journal_path.unlink()

    if on_start is not None:
        on_start(metrics_url)

    remaining_max: int | None = None
    if config.max_docs is not None:
        remaining_max = max(0, config.max_docs - resumed_count)

    walker = iter(
        walk_corpus(
            config.corpus_dir,
            include=config.include,
            exclude=config.exclude,
            resume_keys=resume_keys if config.resume else None,
            max_docs=remaining_max,
        )
    )

    sniffed_per_ext: dict[str, int] = {}
    max_in_flight = config.max_in_flight
    interrupted = threading.Event()

    prev_sigint = prev_sigterm = None
    in_main_thread = threading.current_thread() is threading.main_thread()
    if in_main_thread:
        def _handle_signal(signum: int, _frame: Any) -> None:
            interrupted.set()
            notes.append(f"run interrupted by signal {signum}")

        prev_sigint = signal.signal(signal.SIGINT, _handle_signal)
        prev_sigterm = signal.signal(signal.SIGTERM, _handle_signal)

    completed_since_checkpoint = 0
    walker_exhausted = remaining_max == 0

    try:
        with journal_path.open("a", encoding="utf-8") as journal_file:
            with ThreadPoolExecutor(max_workers=config.workers) as pool:
                in_flight: dict[Future[tuple[dict[str, Any], ObjectResult, str | None, str]], CorpusEntry] = {}

                def _top_up() -> None:
                    nonlocal walker_exhausted
                    while not walker_exhausted and not interrupted.is_set() and len(in_flight) < max_in_flight:
                        try:
                            entry = next(walker)
                        except StopIteration:
                            walker_exhausted = True
                            break
                        metrics.add_discovered(1)
                        do_sniff = (
                            sniffed_per_ext.get(entry.ext, 0) < config.sniff_per_extension
                            and entry.family != "ebcdic"
                        )
                        if do_sniff:
                            sniffed_per_ext[entry.ext] = sniffed_per_ext.get(entry.ext, 0) + 1
                        fut = pool.submit(
                            evaluate_corpus_entry,
                            config,
                            client,
                            entry,
                            do_sniff=do_sniff,
                            run_out=run_out,
                        )
                        in_flight[fut] = entry

                _top_up()
                while in_flight:
                    done, _ = wait(in_flight.keys(), return_when=FIRST_COMPLETED)
                    for fut in done:
                        in_flight.pop(fut, None)
                        if fut.cancelled():
                            continue
                        record, obj_res, note, line = fut.result()
                        journal_file.write(json.dumps(record, separators=(",", ":"), ensure_ascii=False) + "\n")
                        journal_file.flush()

                        results.append(obj_res)
                        if not obj_res.skipped:
                            matrix.add(obj_res)
                        if note:
                            notes.append(note)
                        metrics.record_journal_entry(record)
                        log(line.replace(" ", f" [{len(results)}] ", 1))

                        completed_since_checkpoint += 1
                        if completed_since_checkpoint >= config.checkpoint_interval:
                            completed_since_checkpoint = 0
                            snap = build_corpus_report(
                                label=config.label,
                                target=config.target,
                                corpus_dir=config.corpus_dir,
                                service=service,
                                results=results,
                                matrix=matrix,
                                wall_seconds=time.monotonic() - started,
                                notes=notes,
                                exit_code=0,
                                resumed_count=resumed_count,
                                metrics_url=metrics_url,
                            )
                            write_corpus_report(run_out, snap, metrics)

                    if interrupted.is_set():
                        for pending in list(in_flight.keys()):
                            pending.cancel()
                        break
                    _top_up()
    except BaseException:
        raise
    finally:
        if in_main_thread:
            if prev_sigint is not None:
                signal.signal(signal.SIGINT, prev_sigint)
            if prev_sigterm is not None:
                signal.signal(signal.SIGTERM, prev_sigterm)

    return resumed_count


def main(
    argv: list[str] | None = None,
    env: Mapping[str, str] | None = None,
    client_factory: Callable[[str], Any] | None = None,
    on_start: Callable[[str | None], None] | None = None,
) -> int:
    env = os.environ if env is None else env
    require = (env.get("EVAL_REQUIRE") or "").strip().lower() not in ("", "0", "off", "false", "no")
    try:
        config = CorpusConfig.from_cli_and_env(argv, env, REPO)
    except ConfigError as error:
        return skip(str(error), require)

    if not config.corpus_dir.is_dir():
        return skip(f"corpus directory does not exist: {config.corpus_dir}", config.require)

    if client_factory is None:
        try:
            import grpc  # noqa: F401
            import grpc_tools  # noqa: F401
        except ImportError:
            return skip(
                "grpcio and grpcio-tools are not importable (use uv run --with grpcio --with grpcio-tools)",
                config.require,
            )
        from scorecard.client import GrparseClient

        client_factory = GrparseClient
    from scorecard.client import Unreachable

    started = time.monotonic()
    run_out = config.out / "corpus" / config.label
    results: list[ObjectResult] = []
    matrix = Matrix()
    notes: list[str] = []
    metrics = MetricsCollector(config.label, started_monotonic=started)
    service = "unknown"
    exit_code = 0
    resumed_count = 0

    server: MetricsServer | None = None
    metrics_url: str | None = None
    if config.metrics_port is not None and config.metrics_port >= 0:
        server = MetricsServer(metrics, port=config.metrics_port).start()
        metrics_url = server.url

    try:
        with client_factory(config.target) as client:
            info = client.service_info()
            service = f"{info.name} {info.version}"
            resumed_count = evaluate_corpus(
                config,
                client,
                run_out=run_out,
                results=results,
                matrix=matrix,
                notes=notes,
                metrics=metrics,
                service=service,
                started=started,
                metrics_url=metrics_url,
                on_start=on_start,
            )
    except Unreachable as error:
        exit_code = skip(f"gRParse unreachable: {error}", config.require)
        if not results:
            if server is not None:
                server.close()
            return exit_code
        notes.append(f"run cut short: gRParse became unreachable ({error})")
    finally:
        metrics.mark_finished()
        if server is not None:
            server.close()

    if not results:
        return skip(f"no documents selected under {config.corpus_dir}", config.require)

    skipped_count = sum(1 for r in results if r.skipped)
    if exit_code == 0:
        if any(r.failed for r in results):
            exit_code = 1
        elif skipped_count and config.require:
            exit_code = 1
            notes.append("EVAL_REQUIRE: skipped documents count as failures")

    report = build_corpus_report(
        label=config.label,
        target=config.target,
        corpus_dir=config.corpus_dir,
        service=service,
        results=results,
        matrix=matrix,
        wall_seconds=time.monotonic() - started,
        notes=notes,
        exit_code=exit_code,
        resumed_count=resumed_count,
        metrics_url=metrics_url,
    )
    path = write_corpus_report(run_out, report, metrics)
    totals = report["totals"]
    log(
        f"documents: {totals['evaluated']} evaluated ({resumed_count} resumed), "
        f"{totals['skipped']} skipped, {totals['failed_objects']} with failures; "
        f"checks: {totals['checks_run']} run, {totals['checks_failed']} failed; report: {path}"
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
