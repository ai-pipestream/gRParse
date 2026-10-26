"""What produced a corpus run: the run.json manifest written beside results.jsonl.

A run is only comparable with another when both say which code parsed the
documents, which documents they were, and how the runner was configured.
GetServiceInfo alone cannot say it (it reads the same version string for
every commit between two tags), so every run records:

- the gRParse commit, from the image's ``org.opencontainers.image.revision``
  label (CI, publish.yml and scripts/build-image.sh set it), or from
  ``--grparse-commit`` for an image built without it
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
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from .run_manifest import (
    SCHEMA,
    ConfigValue,
    CorpusIdentity,
    GrparseCommit,
    Harness,
    Host,
    ManifestError,
    RunManifest,
    ServiceImage,
    Stack,
)

__all__ = ["SCHEMA", "ManifestError", "RunManifest"]

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


def harness_provenance(repo: Path) -> Harness:
    """The eval code's own commit: a check that changed moves the numbers too."""
    commit = _git(repo, "rev-parse", "HEAD")
    status = _git(repo, "status", "--porcelain", "--", "eval")
    return Harness(commit=commit, eval_dirty=bool(status) if status is not None else None)


def host_provenance() -> Host:
    return Host(hostname=platform.node(), platform=platform.platform(), python=sys.version.split()[0])


def _safe_env(env_list: Iterable[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in env_list:
        name, _, value = item.partition("=")
        if name.startswith("GRPARSE_") and not SECRET_NAME.search(name):
            out[name] = value
    return dict(sorted(out.items()))


def stack_provenance(project: str, runner: Runner = docker_cli) -> Stack:
    """Inspect every container of a compose project and the image each runs.

    The docker inspect documents are the Docker Engine API's own; a field the
    engine leaves out reads as absent here, never as a made-up value."""
    ids = [line.strip() for line in runner(
        ["ps", "-a", "--no-trunc", "--filter", f"label={COMPOSE_PROJECT_LABEL}={project}", "--format", "{{.ID}}"]
    ).splitlines() if line.strip()]
    if not ids:
        raise ProvenanceError(f"no containers in compose project {project!r}")
    containers = json.loads(runner(["inspect", *ids]))
    image_ids = sorted({c["Image"] for c in containers})
    images = {img["Id"]: img for img in json.loads(runner(["image", "inspect", *image_ids]))}

    services: dict[str, ServiceImage] = {}
    for container in containers:
        config = container["Config"]
        labels = config.get("Labels") or {}
        service = labels.get(COMPOSE_SERVICE_LABEL) or container["Name"].lstrip("/")
        image = images[container["Image"]]
        image_labels = image["Config"].get("Labels") or {}
        services[service] = ServiceImage(
            image=config["Image"],
            image_id=container["Image"],
            repo_digests=tuple(sorted(image.get("RepoDigests") or [])),
            revision=image_labels.get(REVISION_LABEL),
            version=image_labels.get(VERSION_LABEL),
            created=image_labels.get(CREATED_LABEL) or image.get("Created"),
            state=container["State"]["Status"],
            env=_safe_env(config.get("Env") or []) if service == GRPARSE_SERVICE else None,
        )
    return Stack(recorded=True, compose_project=project, services=dict(sorted(services.items())))


def grparse_commit(stack: Stack, explicit: str | None) -> GrparseCommit:
    """The gRParse commit and where it came from; never guessed."""
    if explicit:
        return GrparseCommit(commit=explicit, source="--grparse-commit")
    service = stack.services.get(GRPARSE_SERVICE)
    if service is not None and service.revision:
        return GrparseCommit(commit=service.revision, source=f"image label {REVISION_LABEL}")
    return GrparseCommit(commit=None, source="not recorded: no image label and no --grparse-commit")


def config_provenance(config: Any) -> dict[str, ConfigValue]:
    """The runner config (a dataclass), JSON-safe; passwords become a count."""
    out: dict[str, ConfigValue] = {}
    for key, value in asdict(config).items():
        if key == "document_passwords":
            out["document_password_count"] = len(value)
            continue
        out[key] = str(value) if isinstance(value, Path) else (list(value) if isinstance(value, tuple) else value)
    return out


@dataclass(frozen=True)
class CorpusFile:
    """One document of a run as the journal records it."""

    relpath: str
    size_bytes: int
    sha256: str


def corpus_identity(files: Iterable[CorpusFile]) -> tuple[CorpusIdentity, str]:
    """sha256 over the sorted ``relpath\\tsize\\tsha256`` lines of a run's documents.

    Returns the identity block and the manifest text it hashes, so the
    manifest can be written beside the run and rehashed by anyone. A document
    the runner could not read has an empty sha256 and counts as unhashed."""
    rows = sorted({(f.relpath, f.size_bytes, f.sha256) for f in files})
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
    return f"{stamp}-{(commit or 'nocommit')[:12]}-{(corpus_sha or 'nocorpus')[:12]}"


def stack_fingerprint(manifest: RunManifest) -> tuple[str | None, dict[str, str]]:
    """What must not change inside one run: the gRParse commit and every image id."""
    return manifest.grparse.commit, {name: svc.image_id for name, svc in sorted(manifest.stack.services.items())}


def load_manifest(path: Path) -> RunManifest | None:
    """The manifest at ``path``; None when there is none, ManifestError when it is malformed."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise ManifestError(f"{path}: invalid JSON: {error}") from error
    try:
        return RunManifest.from_json(data)
    except ManifestError as error:
        raise ManifestError(f"{path}: {error}") from error


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


def render_markdown(manifest: RunManifest) -> list[str]:
    """The provenance block report.md and analysis.md open with."""
    grparse, corpus, harness, stack = manifest.grparse, manifest.corpus, manifest.harness, manifest.stack
    corpus_line = (
        f"- corpus: `{corpus.sha256}`, {corpus.files} files, {corpus.bytes} bytes"
        if corpus is not None else "- corpus: not hashed yet (the run has not finished)"
    )
    lines = [
        f"- run: `{manifest.run_id}`, {manifest.started_at} to {manifest.finished_at or 'unfinished'}",
        f"- gRParse commit: `{grparse.commit or 'not recorded'}` ({grparse.source}); "
        f"service `{manifest.service or '-'}`",
        f"{corpus_line} (`{manifest.config['corpus_dir']}`)",
        f"- eval harness: `{harness.commit or '-'}`" + (" (eval/ has uncommitted changes)" if harness.eval_dirty else ""),
    ]
    if stack.recorded:
        lines += ["", "| service | image | digest | revision |", "|---|---|---|---|"]
        for name, svc in stack.services.items():
            lines.append(
                f"| {name} | `{svc.image}` | `{short(svc.digest().rsplit('@', 1)[-1], 19)}` | `{short(svc.revision)}` |"
            )
    else:
        lines.append(f"- stack images: not recorded ({stack.reason})")
    return lines


CONFIG_KEYS_THAT_MOVE_RESULTS = (
    "repeat", "include", "exclude", "max_docs", "sniff_per_extension", "convert_timeout",
    "seconds_per_page", "document_password_count",
)


def compare(baseline: RunManifest | None, candidate: RunManifest | None) -> dict[str, Any]:
    """What differs between the code, images, corpus and config of two runs."""
    warnings: list[str] = []
    if baseline is None:
        warnings.append("baseline has no run.json: what produced it is not recorded")
    if candidate is None:
        warnings.append("candidate has no run.json: what produced it is not recorded")

    def corpus_of(manifest: RunManifest | None) -> CorpusIdentity | None:
        return manifest.corpus if manifest is not None else None

    b_corpus, c_corpus = corpus_of(baseline), corpus_of(candidate)
    same_corpus = (b_corpus.sha256 == c_corpus.sha256) if b_corpus and c_corpus else None
    if same_corpus is False:
        warnings.append("the runs cover different corpora: rates are not comparable, only common documents are")

    b_commit = baseline.grparse.commit if baseline else None
    c_commit = candidate.grparse.commit if candidate else None
    b_harness = baseline.harness if baseline else None
    c_harness = candidate.harness if candidate else None
    if b_harness and c_harness and b_harness.commit and c_harness.commit and b_harness.commit != c_harness.commit:
        warnings.append("the eval harness commit differs: a changed check moves verdicts without a parser change")
    if any(h is not None and h.eval_dirty for h in (b_harness, c_harness)):
        warnings.append("a run was made with uncommitted changes under eval/")

    b_services = baseline.stack.services if baseline else {}
    c_services = candidate.stack.services if candidate else {}
    services = []
    for name in sorted(set(b_services) | set(c_services)):
        b_svc, c_svc = b_services.get(name), c_services.get(name)
        services.append({
            "service": name,
            "baseline": b_svc.digest() if b_svc else None,
            "candidate": c_svc.digest() if c_svc else None,
            "changed": (b_svc.image_id if b_svc else None) != (c_svc.image_id if c_svc else None),
        })

    config_changes: dict[str, list[ConfigValue]] = {}
    if baseline is not None and candidate is not None:
        config_changes = {
            key: [baseline.config.get(key), candidate.config.get(key)]
            for key in CONFIG_KEYS_THAT_MOVE_RESULTS
            if baseline.config.get(key) != candidate.config.get(key)
        }
    if config_changes:
        warnings.append("runner config differs: " + ", ".join(sorted(config_changes)))

    def env_of(manifest: RunManifest | None) -> dict[str, str]:
        service = manifest.stack.services.get(GRPARSE_SERVICE) if manifest else None
        return dict(service.env) if service is not None and service.env is not None else {}

    b_env, c_env = env_of(baseline), env_of(candidate)
    env_changes = {
        name: [b_env.get(name), c_env.get(name)]
        for name in sorted(set(b_env) | set(c_env))
        if b_env.get(name) != c_env.get(name)
    }

    return {
        "baseline_run_id": baseline.run_id if baseline else None,
        "candidate_run_id": candidate.run_id if candidate else None,
        "grparse_commit": {"baseline": b_commit, "candidate": c_commit,
                           "changed": None if not (b_commit and c_commit) else b_commit != c_commit},
        "harness_commit": {"baseline": b_harness.commit if b_harness else None,
                           "candidate": c_harness.commit if c_harness else None},
        "corpus": {
            "baseline": b_corpus.sha256 if b_corpus else None,
            "candidate": c_corpus.sha256 if c_corpus else None,
            "same": same_corpus,
            "baseline_files": b_corpus.files if b_corpus else None,
            "candidate_files": c_corpus.files if c_corpus else None,
        },
        "services": services,
        "config_changes": config_changes,
        "grparse_env_changes": env_changes,
        "warnings": warnings,
    }


def render_comparison(diff: Mapping[str, Any]) -> list[str]:
    commit = diff["grparse_commit"]
    corpus = diff["corpus"]
    same = {True: "same", False: "DIFFERENT", None: "unknown"}[corpus["same"]]
    lines = [
        "## What changed between the runs",
        "",
        f"- runs: `{diff['baseline_run_id'] or '-'}` -> `{diff['candidate_run_id'] or '-'}`",
        f"- gRParse commit: `{commit['baseline'] or 'not recorded'}` -> `{commit['candidate'] or 'not recorded'}`",
        f"- corpus: {same} (`{short(corpus['baseline'])}` {corpus['baseline_files'] or '-'} files -> "
        f"`{short(corpus['candidate'])}` {corpus['candidate_files'] or '-'} files)",
        f"- eval harness: `{short(diff['harness_commit']['baseline'])}` -> `{short(diff['harness_commit']['candidate'])}`",
    ]
    for key, (old, new) in diff["config_changes"].items():
        lines.append(f"- config `{key}`: `{old}` -> `{new}`")
    for name, (old, new) in diff["grparse_env_changes"].items():
        lines.append(f"- gRParse env `{name}`: `{old}` -> `{new}`")
    if diff["services"]:
        lines += ["", "| service | baseline image | candidate image | changed |", "|---|---|---|---|"]
        for svc in diff["services"]:
            lines.append(
                f"| {svc['service']} | `{short((svc['baseline'] or '-').rsplit('@', 1)[-1], 19)}` | "
                f"`{short((svc['candidate'] or '-').rsplit('@', 1)[-1], 19)}` | {'yes' if svc['changed'] else 'no'} |"
            )
    if diff["warnings"]:
        lines += [""] + [f"- warning: {w}" for w in diff["warnings"]]
    return lines + [""]


def find_runs(root: Path) -> list[tuple[Path, RunManifest]]:
    """Every finished run under ``root`` (one directory per run), oldest first."""
    found = []
    for path in sorted(root.glob(f"*/{MANIFEST_NAME}")):
        manifest = load_manifest(path)
        if manifest is not None and manifest.finished_at and (path.parent / "results.jsonl").is_file():
            found.append((path.parent, manifest))
    return sorted(found, key=lambda item: item[1].started_at)


def latest_comparable(root: Path, candidate: RunManifest) -> Path | None:
    """The newest earlier run over the same corpus, the default baseline."""
    if candidate.corpus is None:
        return None
    best = None
    for run_dir, manifest in find_runs(root):
        if manifest.run_id == candidate.run_id or manifest.corpus is None:
            continue
        if manifest.corpus.sha256 != candidate.corpus.sha256:
            continue
        if manifest.started_at >= candidate.started_at:
            continue
        best = run_dir
    return best
