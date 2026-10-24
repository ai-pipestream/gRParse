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
from dataclasses import asdict, is_dataclass
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


def harness_provenance(repo: Path) -> dict[str, Any]:
    """The eval code's own commit: a check that changed moves the numbers too."""
    commit = _git(repo, "rev-parse", "HEAD")
    status = _git(repo, "status", "--porcelain", "--", "eval")
    return {
        "commit": commit,
        "eval_dirty": bool(status) if status is not None else None,
    }


def host_provenance() -> dict[str, Any]:
    return {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
    }


def _safe_env(env_list: Iterable[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in env_list:
        name, _, value = item.partition("=")
        if name.startswith("GRPARSE_") and not SECRET_NAME.search(name):
            out[name] = value
    return dict(sorted(out.items()))


def stack_provenance(project: str, runner: Runner = docker_cli) -> dict[str, Any]:
    """Inspect every container of a compose project and the image each runs."""
    ids = [line.strip() for line in runner(
        ["ps", "-a", "--no-trunc", "--filter", f"label={COMPOSE_PROJECT_LABEL}={project}", "--format", "{{.ID}}"]
    ).splitlines() if line.strip()]
    if not ids:
        raise ProvenanceError(f"no containers in compose project {project!r}")
    containers = json.loads(runner(["inspect", *ids]) or "[]")
    image_ids = sorted({c.get("Image", "") for c in containers if c.get("Image")})
    images = {img.get("Id"): img for img in json.loads(runner(["image", "inspect", *image_ids]) or "[]")} if image_ids else {}

    services: dict[str, dict[str, Any]] = {}
    for container in containers:
        labels = (container.get("Config") or {}).get("Labels") or {}
        service = labels.get(COMPOSE_SERVICE_LABEL) or (container.get("Name") or "").lstrip("/")
        image = images.get(container.get("Image"), {})
        image_labels = (image.get("Config") or {}).get("Labels") or {}
        entry: dict[str, Any] = {
            "image": (container.get("Config") or {}).get("Image"),
            "image_id": container.get("Image"),
            "repo_digests": sorted(image.get("RepoDigests") or []),
            "revision": image_labels.get(REVISION_LABEL),
            "version": image_labels.get(VERSION_LABEL),
            "created": image_labels.get(CREATED_LABEL) or image.get("Created"),
            "state": (container.get("State") or {}).get("Status"),
        }
        if service == GRPARSE_SERVICE:
            entry["env"] = _safe_env((container.get("Config") or {}).get("Env") or [])
        services[service] = entry
    return {"recorded": True, "compose_project": project, "services": dict(sorted(services.items()))}


def grparse_commit(stack: Mapping[str, Any], explicit: str | None) -> dict[str, Any]:
    """The gRParse commit and where it came from; never guessed."""
    if explicit:
        return {"commit": explicit, "source": "--grparse-commit"}
    service = (stack.get("services") or {}).get(GRPARSE_SERVICE) or {}
    if service.get("revision"):
        return {"commit": service["revision"], "source": f"image label {REVISION_LABEL}"}
    return {"commit": None, "source": "not recorded: no image label and no --grparse-commit"}


def config_provenance(config: Any) -> dict[str, Any]:
    """The runner config, JSON-safe; passwords become a count."""
    raw = asdict(config) if is_dataclass(config) else dict(config)
    out: dict[str, Any] = {}
    for key, value in raw.items():
        if key == "document_passwords":
            out["document_password_count"] = len(value or ())
            continue
        out[key] = str(value) if isinstance(value, Path) else (list(value) if isinstance(value, tuple) else value)
    return out


def corpus_identity(records: Iterable[Mapping[str, Any]]) -> tuple[dict[str, Any], str]:
    """sha256 over the sorted ``relpath\\tsize\\tsha256`` lines of a run's documents.

    Returns the identity block and the manifest text it hashes, so the
    manifest can be written beside the run and rehashed by anyone."""
    rows = sorted({
        (str(r.get("relpath") or ""), int(r.get("size_bytes") or 0), str(r.get("sha256") or ""))
        for r in records
    })
    text = "".join(f"{relpath}\t{size}\t{sha}\n" for relpath, size, sha in rows)
    unhashed = sum(1 for _, _, sha in rows if not sha)
    identity = {
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "files": len(rows),
        "bytes": sum(size for _, size, _ in rows),
        "unhashed_files": unhashed,
        "manifest": CORPUS_MANIFEST_NAME,
    }
    return identity, text


def run_id(started_at: str, commit: str | None, corpus_sha: str | None) -> str:
    stamp = started_at.split(".", 1)[0].rstrip("Z").replace("-", "").replace(":", "") + "Z"
    return f"{stamp}-{(commit or 'nocommit')[:12]}-{(corpus_sha or 'nocorpus')[:12]}"


def stack_fingerprint(manifest: Mapping[str, Any]) -> dict[str, Any]:
    """What must not change inside one run: the gRParse commit and every image id."""
    services = (manifest.get("stack") or {}).get("services") or {}
    return {
        "grparse_commit": (manifest.get("grparse") or {}).get("commit"),
        "images": {name: svc.get("image_id") for name, svc in sorted(services.items())},
    }


def load_manifest(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def manifest_beside(journal: Path) -> dict[str, Any] | None:
    """run.json in the journal's directory, when the run wrote one."""
    return load_manifest(journal.parent / MANIFEST_NAME)


def write_manifest(run_out: Path, manifest: Mapping[str, Any], corpus_text: str | None = None) -> Path:
    run_out.mkdir(parents=True, exist_ok=True)
    path = run_out / MANIFEST_NAME
    tmp = run_out / f".{MANIFEST_NAME}.tmp.{os.getpid()}"
    tmp.write_text(json.dumps(manifest, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    if corpus_text is not None:
        (run_out / CORPUS_MANIFEST_NAME).write_text(corpus_text, encoding="utf-8")
    return path


def short(value: str | None, width: int = 12) -> str:
    if not value:
        return "-"
    return value.split(":", 1)[-1][:width] if value.startswith("sha256:") else value[:width]


def digest_of(service: Mapping[str, Any]) -> str:
    """The most portable identity of a service's image: a repo digest, else the image id."""
    digests = service.get("repo_digests") or []
    if digests:
        return str(digests[0])
    return str(service.get("image_id") or "")


def render_markdown(manifest: Mapping[str, Any]) -> list[str]:
    """The provenance block report.md and analysis.md open with."""
    grparse = manifest.get("grparse") or {}
    corpus = manifest.get("corpus") or {}
    harness = manifest.get("harness") or {}
    stack = manifest.get("stack") or {}
    lines = [
        f"- run: `{manifest.get('run_id', '-')}`, {manifest.get('started_at', '-')} to {manifest.get('finished_at', '-')}",
        f"- gRParse commit: `{grparse.get('commit') or 'not recorded'}` ({grparse.get('source', '-')}); "
        f"service `{manifest.get('service', '-')}`",
        f"- corpus: `{corpus.get('sha256', '-')}`, {corpus.get('files', 0)} files, {corpus.get('bytes', 0)} bytes "
        f"(`{manifest.get('config', {}).get('corpus_dir', '-')}`)",
        f"- eval harness: `{harness.get('commit') or '-'}`" + (" (eval/ has uncommitted changes)" if harness.get("eval_dirty") else ""),
    ]
    if stack.get("recorded"):
        lines += ["", "| service | image | digest | revision |", "|---|---|---|---|"]
        for name, svc in (stack.get("services") or {}).items():
            lines.append(
                f"| {name} | `{svc.get('image') or '-'}` | `{short(digest_of(svc).rsplit('@', 1)[-1], 19)}` | "
                f"`{short(svc.get('revision'))}` |"
            )
    else:
        lines.append(f"- stack images: not recorded ({stack.get('reason', 'no compose project given')})")
    return lines


CONFIG_KEYS_THAT_MOVE_RESULTS = (
    "repeat", "include", "exclude", "max_docs", "sniff_per_extension", "convert_timeout",
    "seconds_per_page", "document_password_count",
)




def compare(baseline: Mapping[str, Any] | None, candidate: Mapping[str, Any] | None) -> dict[str, Any]:
    """What differs between the code, images, corpus and config of two runs."""
    warnings: list[str] = []
    if baseline is None:
        warnings.append("baseline has no run.json: what produced it is not recorded")
    if candidate is None:
        warnings.append("candidate has no run.json: what produced it is not recorded")
    b = baseline or {}
    c = candidate or {}

    def pick(manifest: Mapping[str, Any], *path: str) -> Any:
        node: Any = manifest
        for part in path:
            node = node.get(part) if isinstance(node, Mapping) else None
        return node

    b_corpus, c_corpus = pick(b, "corpus", "sha256"), pick(c, "corpus", "sha256")
    same_corpus = (b_corpus == c_corpus) if b_corpus and c_corpus else None
    if same_corpus is False:
        warnings.append("the runs cover different corpora: rates are not comparable, only common documents are")

    b_commit, c_commit = pick(b, "grparse", "commit"), pick(c, "grparse", "commit")
    b_harness, c_harness = pick(b, "harness", "commit"), pick(c, "harness", "commit")
    if b_harness and c_harness and b_harness != c_harness:
        warnings.append("the eval harness commit differs: a changed check moves verdicts without a parser change")
    if pick(c, "harness", "eval_dirty") or pick(b, "harness", "eval_dirty"):
        warnings.append("a run was made with uncommitted changes under eval/")

    b_services = pick(b, "stack", "services") or {}
    c_services = pick(c, "stack", "services") or {}
    services = []
    for name in sorted(set(b_services) | set(c_services)):
        b_digest = digest_of(b_services[name]) if name in b_services else None
        c_digest = digest_of(c_services[name]) if name in c_services else None
        b_id = (b_services.get(name) or {}).get("image_id")
        c_id = (c_services.get(name) or {}).get("image_id")
        services.append({
            "service": name,
            "baseline": b_digest,
            "candidate": c_digest,
            "changed": (b_id or b_digest) != (c_id or c_digest),
        })

    b_config, c_config = pick(b, "config") or {}, pick(c, "config") or {}
    config_changes = {
        key: [b_config.get(key), c_config.get(key)]
        for key in CONFIG_KEYS_THAT_MOVE_RESULTS
        if (b_config or c_config) and b_config.get(key) != c_config.get(key)
    }
    if config_changes:
        warnings.append("runner config differs: " + ", ".join(sorted(config_changes)))

    b_env = pick(b, "stack", "services", GRPARSE_SERVICE, "env") or {}
    c_env = pick(c, "stack", "services", GRPARSE_SERVICE, "env") or {}
    env_changes = {
        name: [b_env.get(name), c_env.get(name)]
        for name in sorted(set(b_env) | set(c_env))
        if b_env.get(name) != c_env.get(name)
    }

    return {
        "baseline_run_id": b.get("run_id"),
        "candidate_run_id": c.get("run_id"),
        "grparse_commit": {"baseline": b_commit, "candidate": c_commit,
                           "changed": None if not (b_commit and c_commit) else b_commit != c_commit},
        "harness_commit": {"baseline": b_harness, "candidate": c_harness},
        "corpus": {
            "baseline": b_corpus, "candidate": c_corpus, "same": same_corpus,
            "baseline_files": pick(b, "corpus", "files"), "candidate_files": pick(c, "corpus", "files"),
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
        f"- runs: `{diff.get('baseline_run_id') or '-'}` -> `{diff.get('candidate_run_id') or '-'}`",
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


def find_runs(root: Path) -> list[tuple[Path, dict[str, Any]]]:
    """Every finished run under ``root`` (one directory per run), oldest first."""
    found = []
    for path in sorted(root.glob(f"*/{MANIFEST_NAME}")):
        manifest = load_manifest(path)
        if manifest and manifest.get("finished_at") and (path.parent / "results.jsonl").is_file():
            found.append((path.parent, manifest))
    return sorted(found, key=lambda item: str(item[1].get("started_at") or ""))


def latest_comparable(root: Path, candidate: Mapping[str, Any]) -> Path | None:
    """The newest earlier run over the same corpus, the default baseline."""
    corpus = (candidate.get("corpus") or {}).get("sha256")
    started = str(candidate.get("started_at") or "")
    best = None
    for run_dir, manifest in find_runs(root):
        if manifest.get("run_id") == candidate.get("run_id"):
            continue
        if (manifest.get("corpus") or {}).get("sha256") != corpus:
            continue
        if started and str(manifest.get("started_at") or "") >= started:
            continue
        best = run_dir
    return best
