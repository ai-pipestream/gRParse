"""What produced a corpus run: the typed run.json manifest written beside results.jsonl.

A run is only comparable with another when both say which code parsed the
documents, which documents they were, and how the runner was configured.
GetServiceInfo alone cannot say it (it reads the same version string for
every commit between two tags), so every run records a typed ``RunManifest``:

- the gRParse commit, from the image's ``org.opencontainers.image.revision``
  label (publish.yml sets it), or from ``--grparse-commit`` for a local build
- every container of the compose project the stack runs as: image reference,
  image id, repo digests and OCI labels, plus gRParse's non-secret
  ``GRPARSE_*`` environment (backend list, OCR batch, execution provider)
- the corpus identity: a sha256 over the sorted ``relpath, size, sha256``
  lines of every document in the run, with the file count and total bytes
- the runner config (passwords counted, never stored), the eval harness
  commit, the host, and UTC start and finish times

Nothing here touches documents beyond the hashes the runner already took.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import asdict, dataclass, fields, is_dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

SCHEMA = "grparse.corpus-run/v1"
MANIFEST_NAME = "run.json"
CORPUS_MANIFEST_NAME = "corpus-manifest.tsv"
REVISION_LABEL = "org.opencontainers.image.revision"
VERSION_LABEL = "org.opencontainers.image.version"
CREATED_LABEL = "org.opencontainers.image.created"
COMPOSE_PROJECT_LABEL = "com.docker.compose.project"
COMPOSE_SERVICE_LABEL = "com.docker.compose.service"
GRPARSE_SERVICE = "grparse"
# Environment names that may carry a credential; never copied into run.json.
SECRET_NAME = re.compile(r"(KEY|SECRET|TOKEN|PASSWORD|PASSWD|CREDENTIAL|AUTH)", re.IGNORECASE)

Runner = Callable[[Sequence[str]], str]


class ProvenanceError(RuntimeError):
    """Provenance was asked for and could not be read; the run must not start."""


class _RecordMixin(Mapping[str, Any]):
    """Read-only mapping interface on frozen provenance dataclasses for JSON/dict callers."""

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {}
        for f in fields(self):  # type: ignore[arg-type]
            val = getattr(self, f.name)
            out[f.name] = _serialize_value(val)
        return out

    def __getitem__(self, key: str) -> Any:
        try:
            return getattr(self, key)
        except AttributeError as error:
            raise KeyError(key) from error

    def __iter__(self) -> Iterator[str]:
        return (f.name for f in fields(self))  # type: ignore[arg-type]

    def __len__(self) -> int:
        return len(fields(self))  # type: ignore[arg-type]


def _serialize_value(val: Any) -> Any:
    if isinstance(val, _RecordMixin):
        return val.to_dict()
    if isinstance(val, tuple):
        return [_serialize_value(item) for item in val]
    if isinstance(val, list):
        return [_serialize_value(item) for item in val]
    if isinstance(val, dict):
        return {k: _serialize_value(v) for k, v in val.items()}
    return val


@dataclass(frozen=True)
class HarnessProvenance(_RecordMixin):
    commit: str | None
    eval_dirty: bool | None

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> HarnessProvenance:
        return cls(commit=data["commit"], eval_dirty=data["eval_dirty"])


@dataclass(frozen=True)
class HostProvenance(_RecordMixin):
    hostname: str
    platform: str
    python: str

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> HostProvenance:
        return cls(
            hostname=str(data["hostname"]),
            platform=str(data["platform"]),
            python=str(data["python"]),
        )


@dataclass(frozen=True)
class ServiceProvenance(_RecordMixin):
    image: str | None
    image_id: str | None
    repo_digests: tuple[str, ...]
    revision: str | None
    version: str | None
    created: str | None
    state: str | None
    env: dict[str, str] | None = None

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "image": self.image,
            "image_id": self.image_id,
            "repo_digests": list(self.repo_digests),
            "revision": self.revision,
            "version": self.version,
            "created": self.created,
            "state": self.state,
        }
        if self.env is not None:
            out["env"] = dict(self.env)
        return out

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> ServiceProvenance:
        env_raw = data["env"] if "env" in data else None
        return cls(
            image=data["image"],
            image_id=data["image_id"],
            repo_digests=tuple(data["repo_digests"]),
            revision=data["revision"],
            version=data["version"],
            created=data["created"],
            state=data["state"],
            env=dict(env_raw) if isinstance(env_raw, Mapping) else None,
        )


@dataclass(frozen=True)
class StackProvenance(_RecordMixin):
    recorded: bool
    compose_project: str | None = None
    services: dict[str, ServiceProvenance] | None = None
    reason: str | None = None

    def to_dict(self) -> dict[str, Any]:
        if self.recorded:
            return {
                "recorded": True,
                "compose_project": self.compose_project,
                "services": {
                    name: svc.to_dict()
                    for name, svc in sorted((self.services or {}).items())
                },
            }
        return {"recorded": False, "reason": self.reason}

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> StackProvenance:
        if isinstance(data, StackProvenance):
            return data
        recorded = bool(data["recorded"])
        if recorded:
            raw_services = data["services"]
            return cls(
                recorded=True,
                compose_project=str(data["compose_project"]),
                services={
                    name: svc if isinstance(svc, ServiceProvenance) else ServiceProvenance.from_dict(svc)
                    for name, svc in raw_services.items()
                },
            )
        return cls(recorded=False, reason=str(data["reason"]))


@dataclass(frozen=True)
class GrparseProvenance(_RecordMixin):
    commit: str | None
    source: str

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> GrparseProvenance:
        return cls(commit=data["commit"], source=str(data["source"]))


@dataclass(frozen=True)
class CorpusIdentity(_RecordMixin):
    sha256: str
    files: int
    bytes: int
    unhashed_files: int
    manifest: str = CORPUS_MANIFEST_NAME

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> CorpusIdentity:
        return cls(
            sha256=str(data["sha256"]),
            files=int(data["files"]),
            bytes=int(data["bytes"]),
            unhashed_files=int(data["unhashed_files"]),
            manifest=str(data["manifest"]),
        )


@dataclass(frozen=True)
class RunHeadline(_RecordMixin):
    evaluated: int
    skipped: int
    status_success_count: int
    success_count: int
    error_count: int
    clean_count: int
    clean_battery_count: int

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> RunHeadline:
        return cls(
            evaluated=int(data["evaluated"]),
            skipped=int(data["skipped"]),
            status_success_count=int(data["status_success_count"]),
            success_count=int(data["success_count"]),
            error_count=int(data["error_count"]),
            clean_count=int(data["clean_count"]),
            clean_battery_count=int(data["clean_battery_count"]),
        )


@dataclass(frozen=True)
class StackFingerprint(_RecordMixin):
    grparse_commit: str | None
    images: dict[str, str | None]


@dataclass(frozen=True)
class RunManifest(_RecordMixin):
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
    config: dict[str, Any]
    corpus: CorpusIdentity | None = None
    headline: RunHeadline | None = None
    resumed_at: tuple[str, ...] = ()
    wall_seconds: float | None = None
    exit_code: int | None = None

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "schema": self.schema,
            "run_id": self.run_id,
            "label": self.label,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "target": self.target,
            "service": self.service,
            "grparse": self.grparse.to_dict(),
            "stack": self.stack.to_dict(),
            "harness": self.harness.to_dict(),
            "host": self.host.to_dict(),
            "config": dict(self.config),
        }
        if self.corpus is not None:
            out["corpus"] = self.corpus.to_dict()
        if self.headline is not None:
            out["headline"] = self.headline.to_dict()
        if self.resumed_at:
            out["resumed_at"] = list(self.resumed_at)
        if self.wall_seconds is not None:
            out["wall_seconds"] = self.wall_seconds
        if self.exit_code is not None:
            out["exit_code"] = self.exit_code
        return out

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> RunManifest:
        if isinstance(data, RunManifest):
            return data
        if data.get("schema") != SCHEMA:
            raise ValueError(f"unsupported manifest schema: {data.get('schema')!r}")
        corpus_raw = data["corpus"] if "corpus" in data and data["corpus"] is not None else None
        headline_raw = data["headline"] if "headline" in data and data["headline"] is not None else None
        resumed_raw = data["resumed_at"] if "resumed_at" in data and data["resumed_at"] is not None else ()
        wall_raw = data["wall_seconds"] if "wall_seconds" in data else None
        exit_raw = data["exit_code"] if "exit_code" in data else None
        return cls(
            schema=str(data["schema"]),
            run_id=str(data["run_id"]),
            label=str(data["label"]),
            started_at=str(data["started_at"]),
            finished_at=str(data["finished_at"]) if data["finished_at"] is not None else None,
            target=str(data["target"]),
            service=str(data["service"]) if data["service"] is not None else None,
            grparse=GrparseProvenance.from_dict(data["grparse"]),
            stack=StackProvenance.from_dict(data["stack"]),
            harness=HarnessProvenance.from_dict(data["harness"]),
            host=HostProvenance.from_dict(data["host"]),
            config=dict(data["config"]),
            corpus=CorpusIdentity.from_dict(corpus_raw) if corpus_raw is not None else None,
            headline=RunHeadline.from_dict(headline_raw) if headline_raw is not None else None,
            resumed_at=tuple(str(ts) for ts in resumed_raw),
            wall_seconds=float(wall_raw) if wall_raw is not None else None,
            exit_code=int(exit_raw) if exit_raw is not None else None,
        )


@dataclass(frozen=True)
class CommitDiff(_RecordMixin):
    baseline: str | None
    candidate: str | None
    changed: bool | None


@dataclass(frozen=True)
class HarnessDiff(_RecordMixin):
    baseline: str | None
    candidate: str | None


@dataclass(frozen=True)
class CorpusDiff(_RecordMixin):
    baseline: str | None
    candidate: str | None
    same: bool | None
    baseline_files: int | None
    candidate_files: int | None


@dataclass(frozen=True)
class ServiceDiff(_RecordMixin):
    service: str
    baseline: str | None
    candidate: str | None
    changed: bool


@dataclass(frozen=True)
class RunComparison(_RecordMixin):
    baseline_run_id: str | None
    candidate_run_id: str | None
    grparse_commit: CommitDiff
    harness_commit: HarnessDiff
    corpus: CorpusDiff
    services: tuple[ServiceDiff, ...]
    config_changes: dict[str, list[Any]]
    grparse_env_changes: dict[str, list[str | None]]
    warnings: tuple[str, ...]


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


def _safe_env(env_list: Iterable[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in env_list:
        name, _, value = item.partition("=")
        if name.startswith("GRPARSE_") and not SECRET_NAME.search(name):
            out[name] = value
    return dict(sorted(out.items()))


def stack_provenance(project: str, runner: Runner = docker_cli) -> StackProvenance:
    """Inspect every container of a compose project and the image each runs."""
    ids = [line.strip() for line in runner(
        ["ps", "-a", "--no-trunc", "--filter", f"label={COMPOSE_PROJECT_LABEL}={project}", "--format", "{{.ID}}"]
    ).splitlines() if line.strip()]
    if not ids:
        raise ProvenanceError(f"no containers in compose project {project!r}")
    containers = json.loads(runner(["inspect", *ids]))
    image_ids = sorted({c["Image"] for c in containers if c.get("Image")})
    images = {img["Id"]: img for img in json.loads(runner(["image", "inspect", *image_ids]))} if image_ids else {}

    # Raw Docker Engine inspect JSON serializes unset Go slices/maps (Labels,
    # RepoDigests, Env) as JSON null; normalize those upstream nulls here at
    # the boundary into typed ServiceProvenance records.
    services: dict[str, ServiceProvenance] = {}
    for container in containers:
        container_config = container.get("Config") or {}
        labels = container_config.get("Labels") or {}
        service = labels.get(COMPOSE_SERVICE_LABEL) or (container.get("Name") or "").lstrip("/")
        image_id = container.get("Image")
        image = images.get(image_id, {})
        image_labels = (image.get("Config") or {}).get("Labels") or {}
        env = _safe_env(container_config.get("Env") or []) if service == GRPARSE_SERVICE else None
        services[service] = ServiceProvenance(
            image=container_config.get("Image"),
            image_id=image_id,
            repo_digests=tuple(sorted(image.get("RepoDigests") or ())),
            revision=image_labels.get(REVISION_LABEL),
            version=image_labels.get(VERSION_LABEL),
            created=image_labels.get(CREATED_LABEL) or image.get("Created"),
            state=(container.get("State") or {}).get("Status"),
            env=env,
        )
    return StackProvenance(
        recorded=True,
        compose_project=project,
        services=dict(sorted(services.items())),
    )


def grparse_commit(stack: StackProvenance | Mapping[str, Any], explicit: str | None) -> GrparseProvenance:
    """The gRParse commit and where it came from; never guessed."""
    if explicit:
        return GrparseProvenance(commit=explicit, source="--grparse-commit")
    services = stack.services if isinstance(stack, StackProvenance) else stack.get("services")
    if services and GRPARSE_SERVICE in services:
        service = services[GRPARSE_SERVICE]
        revision = service.revision if isinstance(service, ServiceProvenance) else service.get("revision")
        if revision:
            return GrparseProvenance(commit=revision, source=f"image label {REVISION_LABEL}")
    return GrparseProvenance(commit=None, source="not recorded: no image label and no --grparse-commit")


def config_provenance(config: Any) -> dict[str, Any]:
    """The runner config, JSON-safe; passwords become a count."""
    raw = asdict(config) if is_dataclass(config) else dict(config)
    out: dict[str, Any] = {}
    for key, value in raw.items():
        if key == "document_passwords":
            out["document_password_count"] = len(value)
            continue
        out[key] = str(value) if isinstance(value, Path) else (list(value) if isinstance(value, tuple) else value)
    return out


def corpus_identity(records: Iterable[Mapping[str, Any]]) -> tuple[CorpusIdentity, str]:
    """sha256 over the sorted ``relpath\\tsize\\tsha256`` lines of a run's documents.

    Returns the typed identity block and the manifest text it hashes, so the
    manifest can be written beside the run and rehashed by anyone."""
    rows = sorted({
        (str(r["relpath"]), int(r["size_bytes"]), str(r["sha256"]))
        for r in records
    })
    text = "".join(f"{relpath}\t{size}\t{sha}\n" for relpath, size, sha in rows)
    unhashed = sum(1 for _, _, sha in rows if not sha)
    identity = CorpusIdentity(
        sha256=hashlib.sha256(text.encode("utf-8")).hexdigest(),
        files=len(rows),
        bytes=sum(size for _, size, _ in rows),
        unhashed_files=unhashed,
        manifest=CORPUS_MANIFEST_NAME,
    )
    return identity, text


def run_id(started_at: str, commit: str | None, corpus_sha: str | None) -> str:
    stamp = started_at.split(".", 1)[0].rstrip("Z").replace("-", "").replace(":", "") + "Z"
    commit_tag = commit[:12] if commit is not None else "nocommit"
    corpus_tag = corpus_sha[:12] if corpus_sha is not None else "nocorpus"
    return f"{stamp}-{commit_tag}-{corpus_tag}"


def stack_fingerprint(manifest: RunManifest | Mapping[str, Any]) -> StackFingerprint:
    """What must not change inside one run: the gRParse commit and every image id."""
    typed = manifest if isinstance(manifest, RunManifest) else RunManifest.from_dict(manifest)
    services = typed.stack.services or {}
    return StackFingerprint(
        grparse_commit=typed.grparse.commit,
        images={name: svc.image_id for name, svc in sorted(services.items())},
    )


def load_manifest(path: Path) -> RunManifest | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    try:
        return RunManifest.from_dict(data)
    except (KeyError, TypeError, ValueError):
        return None


def manifest_beside(journal: Path) -> RunManifest | None:
    """run.json in the journal's directory, when the run wrote one."""
    return load_manifest(journal.parent / MANIFEST_NAME)


def write_manifest(run_out: Path, manifest: RunManifest | Mapping[str, Any], corpus_text: str | None = None) -> Path:
    run_out.mkdir(parents=True, exist_ok=True)
    path = run_out / MANIFEST_NAME
    tmp = run_out / f".{MANIFEST_NAME}.tmp.{os.getpid()}"
    payload = manifest.to_dict() if isinstance(manifest, RunManifest) else dict(manifest)
    tmp.write_text(json.dumps(payload, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    if corpus_text is not None:
        (run_out / CORPUS_MANIFEST_NAME).write_text(corpus_text, encoding="utf-8")
    return path


def short(value: str | None, width: int = 12) -> str:
    if not value:
        return "-"
    return value.split(":", 1)[-1][:width] if value.startswith("sha256:") else value[:width]


def digest_of(service: ServiceProvenance | Mapping[str, Any]) -> str:
    """The most portable identity of a service's image: a repo digest, else the image id."""
    svc = service if isinstance(service, ServiceProvenance) else ServiceProvenance.from_dict(service)
    if svc.repo_digests:
        return svc.repo_digests[0]
    return svc.image_id if svc.image_id is not None else ""


def render_markdown(manifest: RunManifest | Mapping[str, Any]) -> list[str]:
    """The provenance block report.md and analysis.md open with."""
    typed = manifest if isinstance(manifest, RunManifest) else RunManifest.from_dict(manifest)
    finished = typed.finished_at if typed.finished_at is not None else "-"
    commit = typed.grparse.commit if typed.grparse.commit is not None else "not recorded"
    service_name = typed.service if typed.service is not None else "-"
    corpus_dir = typed.config["corpus_dir"] if "corpus_dir" in typed.config else "-"
    if typed.corpus is not None:
        corpus_line = (
            f"- corpus: `{typed.corpus.sha256}`, {typed.corpus.files} files, {typed.corpus.bytes} bytes "
            f"(`{corpus_dir}`)"
        )
    else:
        corpus_line = f"- corpus: in progress (`{corpus_dir}`)"
    harness_commit = typed.harness.commit if typed.harness.commit is not None else "-"
    dirty_suffix = " (eval/ has uncommitted changes)" if typed.harness.eval_dirty else ""
    lines = [
        f"- run: `{typed.run_id}`, {typed.started_at} to {finished}",
        f"- gRParse commit: `{commit}` ({typed.grparse.source}); service `{service_name}`",
        corpus_line,
        f"- eval harness: `{harness_commit}`{dirty_suffix}",
    ]
    if typed.stack.recorded and typed.stack.services is not None:
        lines += ["", "| service | image | digest | revision |", "|---|---|---|---|"]
        for name, svc in typed.stack.services.items():
            image_ref = svc.image if svc.image is not None else "-"
            lines.append(
                f"| {name} | `{image_ref}` | `{short(digest_of(svc).rsplit('@', 1)[-1], 19)}` | "
                f"`{short(svc.revision)}` |"
            )
    else:
        reason = typed.stack.reason if typed.stack.reason is not None else "no compose project given"
        lines.append(f"- stack images: not recorded ({reason})")
    return lines


CONFIG_KEYS_THAT_MOVE_RESULTS = (
    "repeat", "include", "exclude", "max_docs", "sniff_per_extension", "convert_timeout",
    "seconds_per_page", "document_password_count",
)


def compare(
    baseline: RunManifest | Mapping[str, Any] | None,
    candidate: RunManifest | Mapping[str, Any] | None,
) -> RunComparison:
    """What differs between the code, images, corpus and config of two runs."""
    warnings: list[str] = []
    b = (baseline if isinstance(baseline, RunManifest) else RunManifest.from_dict(baseline)) if baseline is not None else None
    c = (candidate if isinstance(candidate, RunManifest) else RunManifest.from_dict(candidate)) if candidate is not None else None
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

    b_services = (b.stack.services or {}) if b is not None else {}
    c_services = (c.stack.services or {}) if c is not None else {}
    services: list[ServiceDiff] = []
    for name in sorted(set(b_services) | set(c_services)):
        b_svc = b_services.get(name)
        c_svc = c_services.get(name)
        b_digest = digest_of(b_svc) if b_svc is not None else None
        c_digest = digest_of(c_svc) if c_svc is not None else None
        b_id = b_svc.image_id if b_svc is not None else None
        c_id = c_svc.image_id if c_svc is not None else None
        services.append(ServiceDiff(
            service=name,
            baseline=b_digest,
            candidate=c_digest,
            changed=(b_id if b_id is not None else b_digest) != (c_id if c_id is not None else c_digest),
        ))

    b_config = b.config if b is not None else {}
    c_config = c.config if c is not None else {}
    config_changes = {
        key: [b_config.get(key), c_config.get(key)]
        for key in CONFIG_KEYS_THAT_MOVE_RESULTS
        if (b_config or c_config) and b_config.get(key) != c_config.get(key)
    }
    if config_changes:
        warnings.append("runner config differs: " + ", ".join(sorted(config_changes)))

    b_grparse_svc = b_services.get(GRPARSE_SERVICE)
    c_grparse_svc = c_services.get(GRPARSE_SERVICE)
    b_env = (b_grparse_svc.env or {}) if b_grparse_svc is not None else {}
    c_env = (c_grparse_svc.env or {}) if c_grparse_svc is not None else {}
    env_changes = {
        name: [b_env.get(name), c_env.get(name)]
        for name in sorted(set(b_env) | set(c_env))
        if b_env.get(name) != c_env.get(name)
    }

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


def render_comparison(diff: RunComparison | Mapping[str, Any]) -> list[str]:
    commit = diff["grparse_commit"]
    corpus = diff["corpus"]
    same = {True: "same", False: "DIFFERENT", None: "unknown"}[corpus["same"]]
    base_id = diff["baseline_run_id"] if diff["baseline_run_id"] is not None else "-"
    cand_id = diff["candidate_run_id"] if diff["candidate_run_id"] is not None else "-"
    base_commit = commit["baseline"] if commit["baseline"] is not None else "not recorded"
    cand_commit = commit["candidate"] if commit["candidate"] is not None else "not recorded"
    base_files = corpus["baseline_files"] if corpus["baseline_files"] is not None else "-"
    cand_files = corpus["candidate_files"] if corpus["candidate_files"] is not None else "-"
    lines = [
        "## What changed between the runs",
        "",
        f"- runs: `{base_id}` -> `{cand_id}`",
        f"- gRParse commit: `{base_commit}` -> `{cand_commit}`",
        f"- corpus: {same} (`{short(corpus['baseline'])}` {base_files} files -> "
        f"`{short(corpus['candidate'])}` {cand_files} files)",
        f"- eval harness: `{short(diff['harness_commit']['baseline'])}` -> `{short(diff['harness_commit']['candidate'])}`",
    ]
    for key, (old, new) in diff["config_changes"].items():
        lines.append(f"- config `{key}`: `{old}` -> `{new}`")
    for name, (old, new) in diff["grparse_env_changes"].items():
        lines.append(f"- gRParse env `{name}`: `{old}` -> `{new}`")
    if diff["services"]:
        lines += ["", "| service | baseline image | candidate image | changed |", "|---|---|---|---|"]
        for svc in diff["services"]:
            base_img = svc["baseline"] if svc["baseline"] is not None else "-"
            cand_img = svc["candidate"] if svc["candidate"] is not None else "-"
            lines.append(
                f"| {svc['service']} | `{short(base_img.rsplit('@', 1)[-1], 19)}` | "
                f"`{short(cand_img.rsplit('@', 1)[-1], 19)}` | {'yes' if svc['changed'] else 'no'} |"
            )
    if diff["warnings"]:
        lines += [""] + [f"- warning: {w}" for w in diff["warnings"]]
    return lines + [""]


def find_runs(root: Path) -> list[tuple[Path, RunManifest]]:
    """Every finished run under ``root`` (one directory per run), oldest first."""
    found: list[tuple[Path, RunManifest]] = []
    for path in sorted(root.glob(f"*/{MANIFEST_NAME}")):
        manifest = load_manifest(path)
        if (
            manifest is not None
            and manifest.finished_at is not None
            and manifest.corpus is not None
            and manifest.headline is not None
            and (path.parent / "results.jsonl").is_file()
        ):
            found.append((path.parent, manifest))
    return sorted(found, key=lambda item: item[1].started_at)


def latest_comparable(root: Path, candidate: RunManifest | Mapping[str, Any]) -> Path | None:
    """The newest earlier run over the same corpus, the default baseline."""
    cand = candidate if isinstance(candidate, RunManifest) else RunManifest.from_dict(candidate)
    if cand.corpus is None:
        return None
    corpus = cand.corpus.sha256
    started = cand.started_at
    best = None
    for run_dir, manifest in find_runs(root):
        if manifest.run_id == cand.run_id:
            continue
        if manifest.corpus is None or manifest.corpus.sha256 != corpus:
            continue
        if started and manifest.started_at >= started:
            continue
        best = run_dir
    return best
