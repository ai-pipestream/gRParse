"""What produced a corpus run: the typed run.json manifest written beside results.jsonl.

A run is only comparable with another when both say which code parsed the
documents, which documents they were, and how the runner was configured.
GetServiceInfo alone cannot say it (it reads the same version string for
every commit between two tags), so every run records a ``RunManifest``:

- the gRParse commit, from the image's ``org.opencontainers.image.revision``
  label (CI, publish.yml and scripts/build-image.sh set it), or from
  ``--grparse-commit`` for an image built without it
- every container of the compose project the stack runs as: image reference,
  image id, repo digests and OCI labels, plus the values of the gRParse
  environment variables named in ``RECORDED_GRPARSE_ENV`` and only the names
  of any other ``GRPARSE_*`` variable
- the corpus identity: a sha256 over the sorted ``relpath, size, sha256``
  lines of every document in the run, with the file count and total bytes
- the runner config (passwords counted, never stored), the eval harness
  commit, the host, and UTC start and finish times

The manifest is our schema, so reading it is strict: a missing key or a
value of the wrong type raises ``ManifestError``. Defaults apply only to the
raw ``docker inspect`` JSON, which is Docker's schema, where an unset field
is stored as absent.

Nothing here touches documents beyond the hashes the runner already took.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import subprocess
import sys
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

SCHEMA = "grparse.corpus-run/v2"
MANIFEST_NAME = "run.json"
CORPUS_MANIFEST_NAME = "corpus-manifest.tsv"
REVISION_LABEL = "org.opencontainers.image.revision"
VERSION_LABEL = "org.opencontainers.image.version"
CREATED_LABEL = "org.opencontainers.image.created"
COMPOSE_PROJECT_LABEL = "com.docker.compose.project"
COMPOSE_SERVICE_LABEL = "com.docker.compose.service"
GRPARSE_SERVICE = "grparse"

# The gRParse environment whose values run.json records: settings that move
# parse output and never hold a credential (backend targets are host:port).
# Any other GRPARSE_* variable is recorded by name only, so an endpoint URL,
# an S3 setting or a variable added later never lands in run.json by accident.
RECORDED_GRPARSE_ENV: frozenset[str] = frozenset({
    "GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES",
    "GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS",
    "GRPARSE_ALLOWED_OCR_KINDS",
    "GRPARSE_ALLOWED_OCR_PRESETS",
    "GRPARSE_ALLOWED_SOURCE_TYPES",
    "GRPARSE_ALLOWED_TARGET_TYPES",
    "GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG",
    "GRPARSE_ASR_DIARIZE",
    "GRPARSE_ASR_MODEL",
    "GRPARSE_ASR_TARGET",
    "GRPARSE_BARCODES",
    "GRPARSE_CALAMINE_TARGET",
    "GRPARSE_CHUNK_TOKENIZER",
    "GRPARSE_DEFAULT_CHART_EXTRACTION_PRESET",
    "GRPARSE_DEFAULT_OCR_KIND",
    "GRPARSE_DEFAULT_OCR_PRESET",
    "GRPARSE_EBCDIC_TARGET",
    "GRPARSE_EMAIL_TARGET",
    "GRPARSE_EMBEDDING_BACKEND",
    "GRPARSE_EMBEDDING_DEVICE",
    "GRPARSE_ENRICH_TARGET",
    "GRPARSE_EPUB_TARGET",
    "GRPARSE_FASTWARC_TARGET",
    "GRPARSE_FIGURE_CLASSES",
    "GRPARSE_LAYOUT",
    "GRPARSE_LAYOUT_MODEL",
    "GRPARSE_LIBREOFFICE_TARGET",
    "GRPARSE_LOL_HTML_TARGET",
    "GRPARSE_MARKUP_TARGET",
    "GRPARSE_MAX_IMAGE_PIXELS",
    "GRPARSE_MAX_RESPONSE_BYTES",
    "GRPARSE_OCR_BATCH",
    "GRPARSE_OCR_BOX_SCORE",
    "GRPARSE_OCR_BOX_THRESH",
    "GRPARSE_OCR_MAX_SIDE",
    "GRPARSE_OCR_PADDING",
    "GRPARSE_OCR_ROTATION",
    "GRPARSE_OCR_UNCLIP",
    "GRPARSE_OPENVINO_DEVICE",
    "GRPARSE_ORT_EP",
    "GRPARSE_PAGE_IMAGES",
    "GRPARSE_PDF_BACKEND",
    "GRPARSE_PDF_BACKEND_HANDSHAKE",
    "GRPARSE_PDF_TARGET",
    "GRPARSE_PICTURE_IMAGES",
    "GRPARSE_POI_TARGET",
    "GRPARSE_REPAIR",
    "GRPARSE_SOURCE_MAX_BYTES",
    "GRPARSE_TABLE_STRUCTURE",
    "GRPARSE_VLM_CONVERT_TARGET",
    "GRPARSE_XML_TARGET",
})

Runner = Callable[[Sequence[str]], str]


class ProvenanceError(RuntimeError):
    """Provenance was asked for and could not be read; the run must not start."""


class ManifestError(ValueError):
    """A run.json that is not a valid ``SCHEMA`` manifest."""


# Strict readers for the manifest's JSON. ``where`` names the object being
# read (``run.json stack.services.grparse``) so an error points at the key.


def _object(value: object, where: str) -> dict[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise ManifestError(f"{where}: expected a JSON object")
    return value


def _field(obj: dict[str, object], key: str, where: str) -> object:
    if key not in obj:
        raise ManifestError(f"{where}: missing key {key!r}")
    return obj[key]


def _str(obj: dict[str, object], key: str, where: str) -> str:
    value = _field(obj, key, where)
    if not isinstance(value, str):
        raise ManifestError(f"{where}.{key}: expected a string")
    return value


def _opt_str(obj: dict[str, object], key: str, where: str) -> str | None:
    value = _field(obj, key, where)
    if value is not None and not isinstance(value, str):
        raise ManifestError(f"{where}.{key}: expected a string or null")
    return value


def _bool(obj: dict[str, object], key: str, where: str) -> bool:
    value = _field(obj, key, where)
    if not isinstance(value, bool):
        raise ManifestError(f"{where}.{key}: expected true or false")
    return value


def _opt_bool(obj: dict[str, object], key: str, where: str) -> bool | None:
    value = _field(obj, key, where)
    if value is not None and not isinstance(value, bool):
        raise ManifestError(f"{where}.{key}: expected true, false or null")
    return value


def _int(obj: dict[str, object], key: str, where: str) -> int:
    value = _field(obj, key, where)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ManifestError(f"{where}.{key}: expected an integer")
    return value


def _opt_int(obj: dict[str, object], key: str, where: str) -> int | None:
    value = _field(obj, key, where)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ManifestError(f"{where}.{key}: expected an integer or null")
    return value


def _float(obj: dict[str, object], key: str, where: str) -> float:
    value = _field(obj, key, where)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ManifestError(f"{where}.{key}: expected a number")
    return float(value)


def _str_tuple(obj: dict[str, object], key: str, where: str) -> tuple[str, ...]:
    value = _field(obj, key, where)
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ManifestError(f"{where}.{key}: expected a list of strings")
    return tuple(value)


def _str_map(obj: dict[str, object], key: str, where: str) -> dict[str, str]:
    value = _object(_field(obj, key, where), f"{where}.{key}")
    out: dict[str, str] = {}
    for name, item in value.items():
        if not isinstance(item, str):
            raise ManifestError(f"{where}.{key}.{name}: expected a string")
        out[name] = item
    return out


@dataclass(frozen=True)
class HarnessProvenance:
    """The eval code's own commit; ``None`` when git could not say."""

    commit: str | None
    eval_dirty: bool | None

    def to_json(self) -> dict[str, object]:
        return {"commit": self.commit, "eval_dirty": self.eval_dirty}

    @classmethod
    def from_json(cls, value: object, where: str) -> HarnessProvenance:
        obj = _object(value, where)
        return cls(commit=_opt_str(obj, "commit", where), eval_dirty=_opt_bool(obj, "eval_dirty", where))


@dataclass(frozen=True)
class HostProvenance:
    hostname: str
    platform: str
    python: str

    def to_json(self) -> dict[str, object]:
        return {"hostname": self.hostname, "platform": self.platform, "python": self.python}

    @classmethod
    def from_json(cls, value: object, where: str) -> HostProvenance:
        obj = _object(value, where)
        return cls(
            hostname=_str(obj, "hostname", where),
            platform=_str(obj, "platform", where),
            python=_str(obj, "python", where),
        )


@dataclass(frozen=True)
class GrparseEnv:
    """gRParse's environment: values of ``RECORDED_GRPARSE_ENV``, names of the rest."""

    values: dict[str, str]
    unrecorded: tuple[str, ...]

    def to_json(self) -> dict[str, object]:
        return {"values": dict(sorted(self.values.items())), "unrecorded": list(self.unrecorded)}

    @classmethod
    def from_json(cls, value: object, where: str) -> GrparseEnv:
        obj = _object(value, where)
        return cls(values=_str_map(obj, "values", where), unrecorded=_str_tuple(obj, "unrecorded", where))


@dataclass(frozen=True)
class ServiceProvenance:
    """One container of the stack. ``None`` fields are absent from Docker's inspect JSON."""

    image: str | None
    image_id: str | None
    repo_digests: tuple[str, ...]
    revision: str | None
    version: str | None
    created: str | None
    state: str | None
    env: GrparseEnv | None

    def to_json(self) -> dict[str, object]:
        return {
            "image": self.image,
            "image_id": self.image_id,
            "repo_digests": list(self.repo_digests),
            "revision": self.revision,
            "version": self.version,
            "created": self.created,
            "state": self.state,
            "env": self.env.to_json() if self.env is not None else None,
        }

    @classmethod
    def from_json(cls, value: object, where: str) -> ServiceProvenance:
        obj = _object(value, where)
        env = _field(obj, "env", where)
        return cls(
            image=_opt_str(obj, "image", where),
            image_id=_opt_str(obj, "image_id", where),
            repo_digests=_str_tuple(obj, "repo_digests", where),
            revision=_opt_str(obj, "revision", where),
            version=_opt_str(obj, "version", where),
            created=_opt_str(obj, "created", where),
            state=_opt_str(obj, "state", where),
            env=GrparseEnv.from_json(env, f"{where}.env") if env is not None else None,
        )

    def digest(self) -> str | None:
        """The most portable identity of the image: a repo digest, else the image id."""
        return self.repo_digests[0] if self.repo_digests else self.image_id


@dataclass(frozen=True)
class RecordedStack:
    compose_project: str
    services: dict[str, ServiceProvenance]

    def to_json(self) -> dict[str, object]:
        return {
            "recorded": True,
            "compose_project": self.compose_project,
            "services": {name: svc.to_json() for name, svc in sorted(self.services.items())},
        }


@dataclass(frozen=True)
class UnrecordedStack:
    reason: str

    def to_json(self) -> dict[str, object]:
        return {"recorded": False, "reason": self.reason}


StackProvenance = RecordedStack | UnrecordedStack


def _stack_from_json(value: object, where: str) -> StackProvenance:
    obj = _object(value, where)
    if not _bool(obj, "recorded", where):
        return UnrecordedStack(reason=_str(obj, "reason", where))
    services = _object(_field(obj, "services", where), f"{where}.services")
    return RecordedStack(
        compose_project=_str(obj, "compose_project", where),
        services={
            name: ServiceProvenance.from_json(svc, f"{where}.services.{name}")
            for name, svc in services.items()
        },
    )


def stack_services(stack: StackProvenance) -> dict[str, ServiceProvenance]:
    return stack.services if isinstance(stack, RecordedStack) else {}


@dataclass(frozen=True)
class GrparseProvenance:
    commit: str | None
    source: str

    def to_json(self) -> dict[str, object]:
        return {"commit": self.commit, "source": self.source}

    @classmethod
    def from_json(cls, value: object, where: str) -> GrparseProvenance:
        obj = _object(value, where)
        return cls(commit=_opt_str(obj, "commit", where), source=_str(obj, "source", where))


@dataclass(frozen=True)
class RunConfig:
    """The runner config as run.json records it: CorpusConfig with the
    document passwords replaced by their count."""

    corpus_dir: str
    target: str
    out: str
    label: str
    workers: int
    repeat: int
    include: tuple[str, ...]
    exclude: tuple[str, ...]
    max_docs: int | None
    resume: bool
    save_outputs: bool
    metrics_port: int | None
    sniff_per_extension: int
    require: bool
    convert_timeout: float
    seconds_per_page: float
    checkpoint_interval: int
    compose_project: str | None
    grparse_commit: str | None
    wait_seconds: float
    document_password_count: int

    def to_json(self) -> dict[str, object]:
        return {
            "corpus_dir": self.corpus_dir,
            "target": self.target,
            "out": self.out,
            "label": self.label,
            "workers": self.workers,
            "repeat": self.repeat,
            "include": list(self.include),
            "exclude": list(self.exclude),
            "max_docs": self.max_docs,
            "resume": self.resume,
            "save_outputs": self.save_outputs,
            "metrics_port": self.metrics_port,
            "sniff_per_extension": self.sniff_per_extension,
            "require": self.require,
            "convert_timeout": self.convert_timeout,
            "seconds_per_page": self.seconds_per_page,
            "checkpoint_interval": self.checkpoint_interval,
            "compose_project": self.compose_project,
            "grparse_commit": self.grparse_commit,
            "wait_seconds": self.wait_seconds,
            "document_password_count": self.document_password_count,
        }

    @classmethod
    def from_json(cls, value: object, where: str) -> RunConfig:
        obj = _object(value, where)
        return cls(
            corpus_dir=_str(obj, "corpus_dir", where),
            target=_str(obj, "target", where),
            out=_str(obj, "out", where),
            label=_str(obj, "label", where),
            workers=_int(obj, "workers", where),
            repeat=_int(obj, "repeat", where),
            include=_str_tuple(obj, "include", where),
            exclude=_str_tuple(obj, "exclude", where),
            max_docs=_opt_int(obj, "max_docs", where),
            resume=_bool(obj, "resume", where),
            save_outputs=_bool(obj, "save_outputs", where),
            metrics_port=_opt_int(obj, "metrics_port", where),
            sniff_per_extension=_int(obj, "sniff_per_extension", where),
            require=_bool(obj, "require", where),
            convert_timeout=_float(obj, "convert_timeout", where),
            seconds_per_page=_float(obj, "seconds_per_page", where),
            checkpoint_interval=_int(obj, "checkpoint_interval", where),
            compose_project=_opt_str(obj, "compose_project", where),
            grparse_commit=_opt_str(obj, "grparse_commit", where),
            wait_seconds=_float(obj, "wait_seconds", where),
            document_password_count=_int(obj, "document_password_count", where),
        )


@dataclass(frozen=True)
class CorpusIdentity:
    sha256: str
    files: int
    bytes: int
    unhashed_files: int
    manifest: str

    def to_json(self) -> dict[str, object]:
        return {
            "sha256": self.sha256,
            "files": self.files,
            "bytes": self.bytes,
            "unhashed_files": self.unhashed_files,
            "manifest": self.manifest,
        }

    @classmethod
    def from_json(cls, value: object, where: str) -> CorpusIdentity:
        obj = _object(value, where)
        return cls(
            sha256=_str(obj, "sha256", where),
            files=_int(obj, "files", where),
            bytes=_int(obj, "bytes", where),
            unhashed_files=_int(obj, "unhashed_files", where),
            manifest=_str(obj, "manifest", where),
        )


@dataclass(frozen=True)
class RunHeadline:
    evaluated: int
    skipped: int
    status_success_count: int
    success_count: int
    error_count: int
    clean_count: int
    clean_battery_count: int

    def to_json(self) -> dict[str, object]:
        return {
            "evaluated": self.evaluated,
            "skipped": self.skipped,
            "status_success_count": self.status_success_count,
            "success_count": self.success_count,
            "error_count": self.error_count,
            "clean_count": self.clean_count,
            "clean_battery_count": self.clean_battery_count,
        }

    @classmethod
    def from_json(cls, value: object, where: str) -> RunHeadline:
        obj = _object(value, where)
        return cls(
            evaluated=_int(obj, "evaluated", where),
            skipped=_int(obj, "skipped", where),
            status_success_count=_int(obj, "status_success_count", where),
            success_count=_int(obj, "success_count", where),
            error_count=_int(obj, "error_count", where),
            clean_count=_int(obj, "clean_count", where),
            clean_battery_count=_int(obj, "clean_battery_count", where),
        )


@dataclass(frozen=True)
class RunManifest:
    """run.json. ``corpus``, ``headline``, ``finished_at``, ``wall_seconds`` and
    ``exit_code`` are ``None`` until the run finishes."""

    schema: str
    run_id: str
    label: str
    started_at: str
    finished_at: str | None
    target: str
    service: str | None
    grparse: GrparseProvenance
    stack: StackProvenance
    harness: HarnessProvenance
    host: HostProvenance
    config: RunConfig
    corpus: CorpusIdentity | None
    headline: RunHeadline | None
    resumed_at: tuple[str, ...]
    wall_seconds: float | None
    exit_code: int | None

    def to_json(self) -> dict[str, object]:
        return {
            "schema": self.schema,
            "run_id": self.run_id,
            "label": self.label,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "target": self.target,
            "service": self.service,
            "grparse": self.grparse.to_json(),
            "stack": self.stack.to_json(),
            "harness": self.harness.to_json(),
            "host": self.host.to_json(),
            "config": self.config.to_json(),
            "corpus": self.corpus.to_json() if self.corpus is not None else None,
            "headline": self.headline.to_json() if self.headline is not None else None,
            "resumed_at": list(self.resumed_at),
            "wall_seconds": self.wall_seconds,
            "exit_code": self.exit_code,
        }

    @classmethod
    def from_json(cls, value: object, where: str = MANIFEST_NAME) -> RunManifest:
        obj = _object(value, where)
        schema = _str(obj, "schema", where)
        if schema != SCHEMA:
            raise ManifestError(f"{where}: schema {schema!r} is not {SCHEMA!r}")
        corpus = _field(obj, "corpus", where)
        headline = _field(obj, "headline", where)
        wall = _field(obj, "wall_seconds", where)
        return cls(
            schema=schema,
            run_id=_str(obj, "run_id", where),
            label=_str(obj, "label", where),
            started_at=_str(obj, "started_at", where),
            finished_at=_opt_str(obj, "finished_at", where),
            target=_str(obj, "target", where),
            service=_opt_str(obj, "service", where),
            grparse=GrparseProvenance.from_json(_field(obj, "grparse", where), f"{where} grparse"),
            stack=_stack_from_json(_field(obj, "stack", where), f"{where} stack"),
            harness=HarnessProvenance.from_json(_field(obj, "harness", where), f"{where} harness"),
            host=HostProvenance.from_json(_field(obj, "host", where), f"{where} host"),
            config=RunConfig.from_json(_field(obj, "config", where), f"{where} config"),
            corpus=CorpusIdentity.from_json(corpus, f"{where} corpus") if corpus is not None else None,
            headline=RunHeadline.from_json(headline, f"{where} headline") if headline is not None else None,
            resumed_at=_str_tuple(obj, "resumed_at", where),
            wall_seconds=_float(obj, "wall_seconds", where) if wall is not None else None,
            exit_code=_opt_int(obj, "exit_code", where),
        )


@dataclass(frozen=True)
class FinishedRun:
    """A run whose run.json says it finished, with its results.jsonl beside it."""

    run_dir: Path
    manifest: RunManifest
    corpus: CorpusIdentity
    headline: RunHeadline


@dataclass(frozen=True)
class StackFingerprint:
    grparse_commit: str | None
    images: dict[str, str | None]


@dataclass(frozen=True)
class CommitDiff:
    baseline: str | None
    candidate: str | None
    changed: bool | None

    def to_json(self) -> dict[str, object]:
        return {"baseline": self.baseline, "candidate": self.candidate, "changed": self.changed}


@dataclass(frozen=True)
class HarnessDiff:
    baseline: str | None
    candidate: str | None

    def to_json(self) -> dict[str, object]:
        return {"baseline": self.baseline, "candidate": self.candidate}


@dataclass(frozen=True)
class CorpusDiff:
    baseline: str | None
    candidate: str | None
    same: bool | None
    baseline_files: int | None
    candidate_files: int | None

    def to_json(self) -> dict[str, object]:
        return {
            "baseline": self.baseline,
            "candidate": self.candidate,
            "same": self.same,
            "baseline_files": self.baseline_files,
            "candidate_files": self.candidate_files,
        }


@dataclass(frozen=True)
class ServiceDiff:
    service: str
    baseline: str | None
    candidate: str | None
    changed: bool

    def to_json(self) -> dict[str, object]:
        return {
            "service": self.service,
            "baseline": self.baseline,
            "candidate": self.candidate,
            "changed": self.changed,
        }


ConfigValue = str | int | float | bool | tuple[str, ...] | None


@dataclass(frozen=True)
class ValueChange:
    """A config or environment value in the baseline and candidate; ``None`` is unset or not recorded."""

    baseline: ConfigValue
    candidate: ConfigValue

    def to_json(self) -> list[object]:
        def plain(value: ConfigValue) -> object:
            return list(value) if isinstance(value, tuple) else value

        return [plain(self.baseline), plain(self.candidate)]


@dataclass(frozen=True)
class RunComparison:
    baseline_run_id: str | None
    candidate_run_id: str | None
    grparse_commit: CommitDiff
    harness_commit: HarnessDiff
    corpus: CorpusDiff
    services: tuple[ServiceDiff, ...]
    config_changes: dict[str, ValueChange]
    grparse_env_changes: dict[str, ValueChange]
    warnings: tuple[str, ...]

    def to_json(self) -> dict[str, object]:
        return {
            "baseline_run_id": self.baseline_run_id,
            "candidate_run_id": self.candidate_run_id,
            "grparse_commit": self.grparse_commit.to_json(),
            "harness_commit": self.harness_commit.to_json(),
            "corpus": self.corpus.to_json(),
            "services": [svc.to_json() for svc in self.services],
            "config_changes": {key: change.to_json() for key, change in self.config_changes.items()},
            "grparse_env_changes": {key: change.to_json() for key, change in self.grparse_env_changes.items()},
            "warnings": list(self.warnings),
        }


def utc_now() -> str:
    """UTC to the millisecond, so runs started in the same second still order."""
    now = datetime.now(UTC)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def docker_cli(args: Sequence[str]) -> str:
    try:
        done = subprocess.run(["docker", *args], check=True, capture_output=True, text=True, timeout=60)
    except FileNotFoundError as error:
        raise ProvenanceError("docker is not on PATH") from error
    except subprocess.CalledProcessError as error:
        raise ProvenanceError(f"docker {' '.join(args[:2])} failed: {error.stderr.strip()[:300]}") from error
    except subprocess.TimeoutExpired as error:
        raise ProvenanceError(f"docker {' '.join(args[:2])} timed out") from error
    return done.stdout


def _git(repo: Path, *args: str) -> str | None:
    try:
        done = subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout.strip()


def harness_provenance(repo: Path) -> HarnessProvenance:
    """The eval code's own commit: a check that changed moves the numbers too."""
    commit = _git(repo, "rev-parse", "HEAD")
    status = _git(repo, "status", "--porcelain", "--", "eval")
    return HarnessProvenance(
        commit=commit,
        eval_dirty=bool(status) if status is not None else None,
    )


def host_provenance() -> HostProvenance:
    return HostProvenance(
        hostname=platform.node(),
        platform=platform.platform(),
        python=sys.version.split()[0],
    )


def grparse_env(env_list: Iterable[str]) -> GrparseEnv:
    """Split a container's ``NAME=value`` list into recorded values and unrecorded names."""
    values: dict[str, str] = {}
    unrecorded: set[str] = set()
    for item in env_list:
        name, _, value = item.partition("=")
        if name in RECORDED_GRPARSE_ENV:
            values[name] = value
        elif name.startswith("GRPARSE_"):
            unrecorded.add(name)
    return GrparseEnv(values=dict(sorted(values.items())), unrecorded=tuple(sorted(unrecorded)))


def _docker_json(text: str, what: str) -> list[Any]:
    """``docker inspect`` output: always a JSON array. Anything else is a broken docker call."""
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise ProvenanceError(f"docker {what} printed no JSON: {error}") from error
    if not isinstance(value, list):
        raise ProvenanceError(f"docker {what} printed {type(value).__name__}, not a JSON array")
    return value


def stack_provenance(project: str, runner: Runner = docker_cli) -> RecordedStack:
    """Inspect every container of a compose project and the image each runs."""
    ids = [line.strip() for line in runner(
        ["ps", "-a", "--no-trunc", "--filter", f"label={COMPOSE_PROJECT_LABEL}={project}", "--format", "{{.ID}}"]
    ).splitlines() if line.strip()]
    if not ids:
        raise ProvenanceError(f"no containers in compose project {project!r}")
    containers = _docker_json(runner(["inspect", *ids]), "inspect")
    image_ids = sorted({c["Image"] for c in containers if c.get("Image")})
    images = {img["Id"]: img for img in _docker_json(runner(["image", "inspect", *image_ids]), "image inspect")} \
        if image_ids else {}

    # Raw Docker Engine inspect JSON (Docker's schema, not ours) leaves unset
    # fields out and writes unset slices and maps (Labels, RepoDigests, Env)
    # as null; both read as absent here.
    services: dict[str, ServiceProvenance] = {}
    for container in containers:
        container_config = container.get("Config") or {}
        labels = container_config.get("Labels") or {}
        service = labels.get(COMPOSE_SERVICE_LABEL) or (container.get("Name") or "").lstrip("/")
        image_id = container.get("Image")
        image = images.get(image_id, {})
        image_labels = (image.get("Config") or {}).get("Labels") or {}
        services[service] = ServiceProvenance(
            image=container_config.get("Image"),
            image_id=image_id,
            repo_digests=tuple(sorted(image.get("RepoDigests") or ())),
            revision=image_labels.get(REVISION_LABEL),
            version=image_labels.get(VERSION_LABEL),
            created=image_labels.get(CREATED_LABEL) or image.get("Created"),
            state=(container.get("State") or {}).get("Status"),
            env=grparse_env(container_config.get("Env") or ()) if service == GRPARSE_SERVICE else None,
        )
    return RecordedStack(compose_project=project, services=dict(sorted(services.items())))


def grparse_commit(stack: StackProvenance, explicit: str | None) -> GrparseProvenance:
    """The gRParse commit and where it came from; never guessed."""
    if explicit:
        return GrparseProvenance(commit=explicit, source="--grparse-commit")
    service = stack_services(stack).get(GRPARSE_SERVICE)
    if service is not None and service.revision:
        return GrparseProvenance(commit=service.revision, source=f"image label {REVISION_LABEL}")
    return GrparseProvenance(commit=None, source="not recorded: no image label and no --grparse-commit")


def corpus_identity(records: Iterable[tuple[str, int, str]]) -> tuple[CorpusIdentity, str]:
    """sha256 over the sorted ``relpath\\tsize\\tsha256`` lines of a run's documents.

    ``records`` are ``(relpath, size_bytes, sha256)`` of each journal entry; an
    empty sha256 is a document the runner could not read, counted in
    ``unhashed_files``. Returns the identity block and the manifest text it
    hashes, so the manifest can be written beside the run and rehashed by anyone."""
    rows = sorted(set(records))
    text = "".join(f"{relpath}\t{size}\t{sha}\n" for relpath, size, sha in rows)
    identity = CorpusIdentity(
        sha256=hashlib.sha256(text.encode("utf-8")).hexdigest(),
        files=len(rows),
        bytes=sum(size for _, size, _ in rows),
        unhashed_files=sum(1 for _, _, sha in rows if not sha),
        manifest=CORPUS_MANIFEST_NAME,
    )
    return identity, text


def run_id(started_at: str, commit: str | None, corpus_sha: str | None) -> str:
    stamp = started_at.split(".", 1)[0].rstrip("Z").replace("-", "").replace(":", "") + "Z"
    commit_tag = commit[:12] if commit is not None else "nocommit"
    corpus_tag = corpus_sha[:12] if corpus_sha is not None else "nocorpus"
    return f"{stamp}-{commit_tag}-{corpus_tag}"


def stack_fingerprint(manifest: RunManifest) -> StackFingerprint:
    """What must not change inside one run: the gRParse commit and every image id."""
    return StackFingerprint(
        grparse_commit=manifest.grparse.commit,
        images={name: svc.image_id for name, svc in sorted(stack_services(manifest.stack).items())},
    )


def load_manifest(path: Path) -> RunManifest | None:
    """The run.json at ``path``; ``None`` when there is none. A run.json that
    is not valid JSON or not a valid manifest raises ``ManifestError``."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise ManifestError(f"{path}: invalid JSON: {error}") from error
    return RunManifest.from_json(data, str(path))


def manifest_beside(journal: Path) -> RunManifest | None:
    """run.json in the journal's directory, when the run wrote one."""
    return load_manifest(journal.parent / MANIFEST_NAME)


def write_manifest(run_out: Path, manifest: RunManifest, corpus_text: str | None = None) -> Path:
    run_out.mkdir(parents=True, exist_ok=True)
    path = run_out / MANIFEST_NAME
    tmp = run_out / f".{MANIFEST_NAME}.tmp.{os.getpid()}"
    tmp.write_text(json.dumps(manifest.to_json(), indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    if corpus_text is not None:
        (run_out / CORPUS_MANIFEST_NAME).write_text(corpus_text, encoding="utf-8")
    return path


def short(value: str | None, width: int = 12) -> str:
    if not value:
        return "-"
    return value.split(":", 1)[-1][:width] if value.startswith("sha256:") else value[:width]


def _short_digest(value: str | None) -> str:
    return short(value.rsplit("@", 1)[-1], 19) if value is not None else "-"


def render_markdown(manifest: RunManifest) -> list[str]:
    """The provenance block report.md and analysis.md open with."""
    finished = manifest.finished_at if manifest.finished_at is not None else "-"
    commit = manifest.grparse.commit if manifest.grparse.commit is not None else "not recorded"
    service_name = manifest.service if manifest.service is not None else "-"
    corpus_dir = manifest.config.corpus_dir
    if manifest.corpus is not None:
        corpus_line = (
            f"- corpus: `{manifest.corpus.sha256}`, {manifest.corpus.files} files, {manifest.corpus.bytes} bytes "
            f"(`{corpus_dir}`)"
        )
    else:
        corpus_line = f"- corpus: in progress (`{corpus_dir}`)"
    harness_commit = manifest.harness.commit if manifest.harness.commit is not None else "-"
    dirty_suffix = " (eval/ has uncommitted changes)" if manifest.harness.eval_dirty else ""
    lines = [
        f"- run: `{manifest.run_id}`, {manifest.started_at} to {finished}",
        f"- gRParse commit: `{commit}` ({manifest.grparse.source}); service `{service_name}`",
        corpus_line,
        f"- eval harness: `{harness_commit}`{dirty_suffix}",
    ]
    if isinstance(manifest.stack, RecordedStack):
        lines += ["", "| service | image | digest | revision |", "|---|---|---|---|"]
        for name, svc in manifest.stack.services.items():
            image_ref = svc.image if svc.image is not None else "-"
            lines.append(f"| {name} | `{image_ref}` | `{_short_digest(svc.digest())}` | `{short(svc.revision)}` |")
    else:
        lines.append(f"- stack images: not recorded ({manifest.stack.reason})")
    return lines


CONFIG_KEYS_THAT_MOVE_RESULTS = (
    "repeat", "include", "exclude", "max_docs", "sniff_per_extension", "convert_timeout",
    "seconds_per_page", "document_password_count",
)


def _config_value(config: RunConfig | None, key: str) -> ConfigValue:
    if config is None:
        return None
    value: ConfigValue = getattr(config, key)
    return value


def _env_value(env: GrparseEnv | None, name: str) -> ConfigValue:
    if env is None:
        return None
    if name in env.values:
        return env.values[name]
    return "(set, not recorded)" if name in env.unrecorded else None


def compare(baseline: RunManifest | None, candidate: RunManifest | None) -> RunComparison:
    """What differs between the code, images, corpus and config of two runs."""
    warnings: list[str] = []
    b, c = baseline, candidate
    if b is None:
        warnings.append("baseline has no run.json: what produced it is not recorded")
    if c is None:
        warnings.append("candidate has no run.json: what produced it is not recorded")

    b_corpus = b.corpus.sha256 if (b is not None and b.corpus is not None) else None
    c_corpus = c.corpus.sha256 if (c is not None and c.corpus is not None) else None
    same_corpus = (b_corpus == c_corpus) if (b_corpus is not None and c_corpus is not None) else None
    if same_corpus is False:
        warnings.append("the runs cover different corpora: rates are not comparable, only common documents are")

    b_commit = b.grparse.commit if b is not None else None
    c_commit = c.grparse.commit if c is not None else None
    b_harness = b.harness.commit if b is not None else None
    c_harness = c.harness.commit if c is not None else None
    if b_harness is not None and c_harness is not None and b_harness != c_harness:
        warnings.append("the eval harness commit differs: a changed check moves verdicts without a parser change")
    if (c is not None and c.harness.eval_dirty) or (b is not None and b.harness.eval_dirty):
        warnings.append("a run was made with uncommitted changes under eval/")

    b_services = stack_services(b.stack) if b is not None else {}
    c_services = stack_services(c.stack) if c is not None else {}
    services: list[ServiceDiff] = []
    for name in sorted(set(b_services) | set(c_services)):
        b_svc = b_services.get(name)
        c_svc = c_services.get(name)
        b_digest = b_svc.digest() if b_svc is not None else None
        c_digest = c_svc.digest() if c_svc is not None else None
        b_id = b_svc.image_id if b_svc is not None else None
        c_id = c_svc.image_id if c_svc is not None else None
        services.append(ServiceDiff(
            service=name,
            baseline=b_digest,
            candidate=c_digest,
            changed=(b_id if b_id is not None else b_digest) != (c_id if c_id is not None else c_digest),
        ))

    config_changes: dict[str, ValueChange] = {}
    if b is not None and c is not None:
        for key in CONFIG_KEYS_THAT_MOVE_RESULTS:
            change = ValueChange(_config_value(b.config, key), _config_value(c.config, key))
            if change.baseline != change.candidate:
                config_changes[key] = change
    if config_changes:
        warnings.append("runner config differs: " + ", ".join(sorted(config_changes)))

    b_grparse = b_services.get(GRPARSE_SERVICE)
    c_grparse = c_services.get(GRPARSE_SERVICE)
    b_env = b_grparse.env if b_grparse is not None else None
    c_env = c_grparse.env if c_grparse is not None else None
    env_names: set[str] = set()
    for env in (b_env, c_env):
        if env is not None:
            env_names |= set(env.values) | set(env.unrecorded)
    env_changes: dict[str, ValueChange] = {}
    for name in sorted(env_names):
        change = ValueChange(_env_value(b_env, name), _env_value(c_env, name))
        if change.baseline != change.candidate:
            env_changes[name] = change

    return RunComparison(
        baseline_run_id=b.run_id if b is not None else None,
        candidate_run_id=c.run_id if c is not None else None,
        grparse_commit=CommitDiff(
            baseline=b_commit,
            candidate=c_commit,
            changed=None if not (b_commit and c_commit) else b_commit != c_commit,
        ),
        harness_commit=HarnessDiff(baseline=b_harness, candidate=c_harness),
        corpus=CorpusDiff(
            baseline=b_corpus,
            candidate=c_corpus,
            same=same_corpus,
            baseline_files=b.corpus.files if (b is not None and b.corpus is not None) else None,
            candidate_files=c.corpus.files if (c is not None and c.corpus is not None) else None,
        ),
        services=tuple(services),
        config_changes=config_changes,
        grparse_env_changes=env_changes,
        warnings=tuple(warnings),
    )


def _shown(value: ConfigValue) -> str:
    if value is None:
        return "-"
    if isinstance(value, tuple):
        return ",".join(value)
    return str(value)


def render_comparison(diff: RunComparison) -> list[str]:
    same = {True: "same", False: "DIFFERENT", None: "unknown"}[diff.corpus.same]
    base_id = diff.baseline_run_id if diff.baseline_run_id is not None else "-"
    cand_id = diff.candidate_run_id if diff.candidate_run_id is not None else "-"
    commit = diff.grparse_commit
    base_commit = commit.baseline if commit.baseline is not None else "not recorded"
    cand_commit = commit.candidate if commit.candidate is not None else "not recorded"
    corpus = diff.corpus
    base_files = corpus.baseline_files if corpus.baseline_files is not None else "-"
    cand_files = corpus.candidate_files if corpus.candidate_files is not None else "-"
    lines = [
        "## What changed between the runs",
        "",
        f"- runs: `{base_id}` -> `{cand_id}`",
        f"- gRParse commit: `{base_commit}` -> `{cand_commit}`",
        f"- corpus: {same} (`{short(corpus.baseline)}` {base_files} files -> "
        f"`{short(corpus.candidate)}` {cand_files} files)",
        f"- eval harness: `{short(diff.harness_commit.baseline)}` -> `{short(diff.harness_commit.candidate)}`",
    ]
    for key, change in diff.config_changes.items():
        lines.append(f"- config `{key}`: `{_shown(change.baseline)}` -> `{_shown(change.candidate)}`")
    for name, change in diff.grparse_env_changes.items():
        lines.append(f"- gRParse env `{name}`: `{_shown(change.baseline)}` -> `{_shown(change.candidate)}`")
    if diff.services:
        lines += ["", "| service | baseline image | candidate image | changed |", "|---|---|---|---|"]
        for svc in diff.services:
            lines.append(
                f"| {svc.service} | `{_short_digest(svc.baseline)}` | "
                f"`{_short_digest(svc.candidate)}` | {'yes' if svc.changed else 'no'} |"
            )
    if diff.warnings:
        lines += [""] + [f"- warning: {w}" for w in diff.warnings]
    return lines + [""]


def find_runs(root: Path) -> list[FinishedRun]:
    """Every finished run under ``root`` (one directory per run), oldest first.

    A run still in progress (no corpus, headline or finish time yet) or
    without its results.jsonl is not listed; a malformed run.json raises."""
    found: list[FinishedRun] = []
    for path in sorted(root.glob(f"*/{MANIFEST_NAME}")):
        manifest = load_manifest(path)
        if (
            manifest is not None
            and manifest.finished_at is not None
            and manifest.corpus is not None
            and manifest.headline is not None
            and (path.parent / "results.jsonl").is_file()
        ):
            found.append(FinishedRun(path.parent, manifest, manifest.corpus, manifest.headline))
    return sorted(found, key=lambda run: run.manifest.started_at)


def latest_comparable(root: Path, candidate: RunManifest) -> Path | None:
    """The newest earlier run over the same corpus, the default baseline."""
    if candidate.corpus is None:
        return None
    best = None
    for run in find_runs(root):
        if run.manifest.run_id == candidate.run_id or run.corpus.sha256 != candidate.corpus.sha256:
            continue
        if run.manifest.started_at >= candidate.started_at:
            continue
        best = run.run_dir
    return best
