#!/usr/bin/env python3
"""Run every object of an S3 bucket through gRParse and the shape battery.

    uv run --with boto3 --with grpcio --with grpcio-tools python eval/s3/run.py

Environment (nothing else configures the run):
  EVAL_S3_ENDPOINT, EVAL_S3_BUCKET, EVAL_S3_PREFIX (optional)
  AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (or EVAL_S3_ACCESS_KEY / EVAL_S3_SECRET_KEY)
  EVAL_S3_REGION (default us-east-1), GRPARSE_TARGET (default localhost:50051)
  EVAL_S3_MAX_OBJECTS, EVAL_S3_INCLUDE / EVAL_S3_EXCLUDE (comma-separated key globs)
  EVAL_S3_REPEAT (default 2), EVAL_S3_SNIFF_PER_EXTENSION (default 1)
  EVAL_OUT (default eval/out), EVAL_LABEL (default live), EVAL_REQUIRE=1 (a skip fails)
  EVAL_S3_PASSWORDS_FILE (optional): one candidate document password per line,
    sent with every conversion as call metadata, never printed or reported

Object bytes are fetched into memory and never written to disk. Outputs land
in EVAL_OUT/s3/<label>/report.md and report.json.

Exit codes: 0 every check passed on every object; 1 any failure (or a skip
under EVAL_REQUIRE); 77 when the configuration is missing, the bucket or
gRParse is unreachable, or the selection is empty.
"""

from __future__ import annotations

import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from s3.battery import Matrix, ObjectResult, evaluate, skipped  # noqa: E402
from s3.checks import ObjectContext  # noqa: E402
from s3.config import Config, ConfigError  # noqa: E402
from s3.document import View  # noqa: E402
from s3.formats import extension_of, family_of, strip_extension  # noqa: E402
from s3.report import build_report, write_report  # noqa: E402
from s3.source import ObjectRef, ObjectSource, SourceUnreachable, select_keys  # noqa: E402
from s3.sourcefacts import pdf_page_count, source_facts  # noqa: E402

SKIP = 77
REPO = EVAL_DIR.parent
FORMATS = ("MARKDOWN", "CANONICAL_JSON")
LAYOUT_SUFFIX = ".layout.json"


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def skip(reason: str, require: bool) -> int:
    if require:
        log(f"FAIL (EVAL_REQUIRE): {reason}")
        return 1
    log(f"skip: {reason}")
    return SKIP


def companion_layout(key: str, refs: list[ObjectRef]) -> str | None:
    """The EBCDIC layout beside an .ebc object: <stem>.layout.json in the listing."""
    stem = key[: -(len(extension_of(key)) + 1)] if extension_of(key) else key
    wanted = stem + LAYOUT_SUFFIX
    return wanted if any(ref.key == wanted for ref in refs) else None


def convert_once(client: Any, data: bytes, name: str, kwargs: dict[str, Any]) -> Any:
    """One conversion. gRParse answers UNAVAILABLE both when it is down and
    when a collector leg failed on the wire; the shared client raises for
    either, so the two are told apart here by asking the service again. A
    collector outage is this object's failure, a dead gRParse ends the run."""
    from scorecard.client import ConvertResult, Unreachable

    try:
        return client.convert_bytes(data, name, **kwargs)
    except Unreachable as error:
        try:
            client.service_info()
        except Unreachable:
            raise
        return ConvertResult(document={}, markdown="", status="RPC_ERROR", errors=[], elapsed_ms=0.0,
                             rpc_error=f"UNAVAILABLE: {str(error).split(': ', 1)[-1] or 'collector leg failed'}")


def ran_out_of_time(result: Any, timeout: float) -> bool:
    """The client deadline fired, or the parse used most of it: a repeat would
    wait as long again for the same answer, so one such run is evidence enough."""
    if (result.rpc_error or "").startswith("DEADLINE_EXCEEDED"):
        return True
    return result.elapsed_ms >= timeout * 1000.0 * 0.8


def over_unary_limit(result: Any) -> bool:
    """gRParse refused the unary response as too large, which it does before
    sending anything and names as the unary response limit."""
    error = result.rpc_error or ""
    return error.startswith("RESOURCE_EXHAUSTED") and "unary response limit" in error


def convert_object(client: Any, key: str, data: bytes, ext: str, *, repeat: int, sniff: bool,
                   layout: bytes | None, timeout: float,
                   passwords: tuple[str, ...] = ()) -> tuple[list[Any], Any | None, tuple[str, ...]]:
    """The object's runs, its sniffed run, and the export formats dropped to
    fit the unary response limit. A document whose Document fits but whose
    exports do not (a sheet of a million cells renders a canonical JSON of
    about a gigabyte, its grid spelled out cell by cell) is refused whole;
    gRParse's own advice is to request fewer formats, so the battery does
    that, one format at a time, and the report names what it dropped."""
    name = key.rsplit("/", 1)[-1]
    formats = FORMATS
    dropped: tuple[str, ...] = ()
    kwargs: dict[str, Any] = {"formats": formats, "timeout": timeout}
    if passwords:
        kwargs["passwords"] = passwords
    if layout is not None:
        kwargs.update(collectors=("EBCDIC",), ebcdic_layout_json=layout)
    first = convert_once(client, data, name, kwargs)
    for fewer in ("CANONICAL_JSON", "MARKDOWN"):
        if not over_unary_limit(first) or fewer not in formats:
            break
        formats = tuple(f for f in formats if f != fewer)
        dropped += (fewer,)
        kwargs["formats"] = formats
        first = convert_once(client, data, name, kwargs)
    runs: list[Any] = [first]
    while len(runs) < repeat and not ran_out_of_time(runs[-1], timeout):
        runs.append(convert_once(client, data, name, kwargs))
    if ran_out_of_time(runs[0], timeout):
        return runs, None, dropped
    sniffed = convert_once(client, data, strip_extension(name), kwargs) if sniff and ext else None
    return runs, sniffed, dropped


@dataclass(frozen=True)
class Planned:
    """One selected object and what was decided about it before any
    conversion ran, in selection order, so a concurrent run sniffs and
    reports the same objects a serial one does."""
    index: int
    ref: ObjectRef
    ext: str
    family: str
    layout_key: str | None
    sniff: bool


def plan_objects(selected: list[ObjectRef], listing: list[ObjectRef], sniff_per_extension: int) -> list[Planned]:
    sniffed_per_ext: dict[str, int] = {}
    planned = []
    for index, ref in enumerate(selected, start=1):
        ext = extension_of(ref.key)
        family = family_of(ext)
        layout_key = companion_layout(ref.key, listing) if family == "ebcdic" else None
        do_sniff = sniffed_per_ext.get(ext, 0) < sniff_per_extension and family != "ebcdic"
        if do_sniff:
            sniffed_per_ext[ext] = sniffed_per_ext.get(ext, 0) + 1
        planned.append(Planned(index, ref, ext, family, layout_key, do_sniff))
    return planned


def evaluate_object(config: Config, source: ObjectSource, client: Any,
                    plan: Planned) -> tuple[ObjectResult, str | None, str]:
    """The object's result, the note it adds (if any) and its log line."""
    ref, ext, family = plan.ref, plan.ext, plan.family
    if family == "ebcdic" and plan.layout_key is None:
        return (skipped(ref.key, ext, family, ref.size, "no <stem>.layout.json beside the .ebc object"), None,
                f"-- {ref.key}: skipped (no layout)")
    layout = source.fetch(plan.layout_key) if plan.layout_key is not None else None
    data = source.fetch(ref.key)
    runs, sniff, dropped = convert_object(client, ref.key, data, ext, repeat=config.repeat, sniff=plan.sniff,
                                          layout=layout,
                                          timeout=config.deadline_for(pdf_page_count(data) if ext == "pdf" else 0),
                                          passwords=config.document_passwords)
    note = None
    if dropped:
        note = (f"{ref.key}: over the unary response limit with every export; checked without "
                f"{', '.join(dropped)}")
    first = runs[0]
    view = View(first.document) if first.document and not first.rpc_error else None
    ctx = ObjectContext(key=ref.key, ext=ext, family=family, size=ref.size,
                        facts=source_facts(ext, family, data), runs=runs, sniff=sniff, view=view)
    del data
    result = evaluate(ctx)
    failed = [name for name, verdict in result.checks.items() if verdict == "fail"]
    mark = "!!" if failed else "=="
    line = (f"{mark} {ref.key}: {result.parser_type} {result.status} "
            f"({result.elapsed_ms:.0f} ms){' [' + ', '.join(failed) + ']' if failed else ''}")
    return result, note, line


def evaluate_bucket(config: Config, source: ObjectSource, client: Any, results: list[ObjectResult],
                    matrix: Matrix, notes: list[str]) -> None:
    """Appends to ``results``/``matrix``/``notes`` in selection order as
    objects finish, so an aborted run still reports every object it
    finished. EVAL_S3_CONCURRENCY objects convert at once; results, notes
    and log lines keep selection order whatever finishes first."""
    listing = source.list_objects()
    selected = [ref for ref in select_keys(listing, config.include, config.exclude, None)
                if not ref.key.endswith(LAYOUT_SUFFIX)]
    if config.max_objects is not None:
        selected = selected[: config.max_objects]
    if not selected:
        raise SourceUnreachable(f"no objects selected under s3://{config.bucket}/{config.prefix}")
    planned = plan_objects(selected, listing, config.sniff_per_extension)

    def record(plan: Planned, outcome: tuple[ObjectResult, str | None, str]) -> None:
        result, note, line = outcome
        results.append(result)
        if not result.skipped:
            matrix.add(result)
        if note:
            notes.append(note)
        log(line.replace(" ", f" [{plan.index}/{len(planned)}] ", 1))

    if config.concurrency <= 1:
        for plan in planned:
            record(plan, evaluate_object(config, source, client, plan))
        return
    with ThreadPoolExecutor(max_workers=config.concurrency) as pool:
        futures = [pool.submit(evaluate_object, config, source, client, plan) for plan in planned]
        try:
            for plan, future in zip(planned, futures):
                record(plan, future.result())
        except BaseException:
            for future in futures:
                future.cancel()
            raise


def main(argv: list[str] | None = None, env: Mapping[str, str] | None = None,
         source_factory: Callable[[Config], ObjectSource] | None = None,
         client_factory: Callable[[str], Any] | None = None) -> int:
    env = os.environ if env is None else env
    require = (env.get("EVAL_REQUIRE") or "").strip().lower() not in ("", "0", "off", "false", "no")
    try:
        config = Config.from_env(env, REPO)
    except ConfigError as error:
        return skip(str(error), require)
    if client_factory is None:
        try:
            import grpc  # noqa: F401
            import grpc_tools  # noqa: F401
        except ImportError:
            return skip("grpcio and grpcio-tools are not importable (use uv run --with grpcio --with grpcio-tools)",
                        config.require)
        from scorecard.client import GrparseClient
        client_factory = GrparseClient
    if source_factory is None:
        try:
            import boto3  # noqa: F401
        except ImportError:
            return skip("boto3 is not importable (use uv run --with boto3)", config.require)
        from s3.source import Boto3Source
        source_factory = Boto3Source
    from scorecard.client import Unreachable

    started = time.monotonic()
    out = config.out / "s3" / config.label
    results: list[ObjectResult] = []
    matrix = Matrix()
    notes: list[str] = []
    service = "unknown"
    exit_code = 0
    try:
        source = source_factory(config)
        with client_factory(config.target) as client:
            info = client.service_info()
            service = f"{info.name} {info.version}"
            evaluate_bucket(config, source, client, results, matrix, notes)
    except SourceUnreachable as error:
        return skip(f"bucket unreachable or empty: {error}", config.require)
    except Unreachable as error:
        exit_code = skip(f"gRParse unreachable: {error}", config.require)
        if not results:
            return exit_code
        notes.append(f"run cut short: gRParse became unreachable ({error})")
    skipped_count = sum(1 for r in results if r.skipped)
    if exit_code == 0:
        if any(r.failed for r in results):
            exit_code = 1
        elif skipped_count and config.require:
            exit_code = 1
            notes.append("EVAL_REQUIRE: skipped objects count as failures")
    report = build_report(label=config.label, target=config.target, endpoint=config.public_endpoint(),
                          bucket=config.bucket, prefix=config.prefix, service=service, results=results,
                          matrix=matrix, wall_seconds=time.monotonic() - started, notes=notes, exit_code=exit_code)
    path = write_report(out, report)
    totals = report["totals"]
    log(f"objects: {totals['evaluated']} evaluated, {totals['skipped']} skipped, {totals['failed_objects']} with "
        f"failures; checks: {totals['checks_run']} run, {totals['checks_failed']} failed; report: {path}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
