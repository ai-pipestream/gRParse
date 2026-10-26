"""The run.json manifest as typed records (schema ``grparse.corpus-run/v1``).

Every block of run.json has one frozen dataclass here. ``to_json`` writes the
same object shape run.json has always had, and ``from_json`` reads it back
strictly: a missing field or a field of the wrong type raises
``ManifestError`` naming the field, instead of reading as zero or empty. A run
that has not finished yet has no ``corpus`` or ``headline`` block and a null
``finished_at``; those are the only fields allowed to be absent.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import Any

from .strict_json import SchemaError, block, optional, require, str_list, typed_map

SCHEMA = "grparse.corpus-run/v1"

# A runner config value as run.json stores it: a scalar or a list of scalars.
ConfigValue = str | int | float | bool | None | list[str]


class ManifestError(SchemaError):
    """run.json is not a well-formed grparse.corpus-run/v1 manifest."""


@dataclass(frozen=True)
class ServiceImage:
    """One compose service's container and the image it runs."""

    image: str
    image_id: str
    repo_digests: tuple[str, ...]
    revision: str | None
    version: str | None
    created: str | None
    state: str
    # gRParse's non-secret GRPARSE_* environment; None for every other service.
    env: dict[str, str] | None = None

    def digest(self) -> str:
        """The most portable identity of the image: a repo digest, else the image id."""
        return self.repo_digests[0] if self.repo_digests else self.image_id

    def to_json(self) -> dict[str, Any]:
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

    @staticmethod
    def from_json(data: Mapping[str, Any], where: str) -> ServiceImage:
        return ServiceImage(
            image=require(data, "image", str, where),
            image_id=require(data, "image_id", str, where),
            repo_digests=str_list(data, "repo_digests", where),
            revision=optional(data, "revision", str, where),
            version=optional(data, "version", str, where),
            created=optional(data, "created", str, where),
            state=require(data, "state", str, where),
            env=typed_map(data, "env", str, where) if "env" in data else None,
        )


@dataclass(frozen=True)
class Stack:
    """The compose project the run's services ran as, or why it was not recorded."""

    recorded: bool
    compose_project: str | None = None
    services: dict[str, ServiceImage] = field(default_factory=dict)
    reason: str | None = None

    @staticmethod
    def not_recorded(reason: str) -> Stack:
        return Stack(recorded=False, reason=reason)

    def to_json(self) -> dict[str, Any]:
        if not self.recorded:
            return {"recorded": False, "reason": self.reason}
        return {
            "recorded": True,
            "compose_project": self.compose_project,
            "services": {name: svc.to_json() for name, svc in sorted(self.services.items())},
        }

    @staticmethod
    def from_json(data: Mapping[str, Any]) -> Stack:
        where = "stack"
        if not require(data, "recorded", bool, where):
            return Stack.not_recorded(require(data, "reason", str, where))
        services = block(data, "services", where)
        return Stack(
            recorded=True,
            compose_project=require(data, "compose_project", str, where),
            services={
                name: ServiceImage.from_json(block(services, name, f"{where}.services"), f"{where}.services.{name}")
                for name in sorted(services)
            },
        )


@dataclass(frozen=True)
class GrparseCommit:
    """The gRParse commit the run parsed with, and where that came from."""

    commit: str | None
    source: str

    def to_json(self) -> dict[str, Any]:
        return {"commit": self.commit, "source": self.source}

    @staticmethod
    def from_json(data: Mapping[str, Any]) -> GrparseCommit:
        return GrparseCommit(optional(data, "commit", str, "grparse"), require(data, "source", str, "grparse"))


@dataclass(frozen=True)
class Harness:
    """The eval code's own commit, and whether eval/ had uncommitted changes."""

    commit: str | None
    eval_dirty: bool | None

    def to_json(self) -> dict[str, Any]:
        return {"commit": self.commit, "eval_dirty": self.eval_dirty}

    @staticmethod
    def from_json(data: Mapping[str, Any]) -> Harness:
        return Harness(optional(data, "commit", str, "harness"), optional(data, "eval_dirty", bool, "harness"))


@dataclass(frozen=True)
class Host:
    hostname: str
    platform: str
    python: str

    def to_json(self) -> dict[str, Any]:
        return {"hostname": self.hostname, "platform": self.platform, "python": self.python}

    @staticmethod
    def from_json(data: Mapping[str, Any]) -> Host:
        return Host(
            require(data, "hostname", str, "host"),
            require(data, "platform", str, "host"),
            require(data, "python", str, "host"),
        )


@dataclass(frozen=True)
class CorpusIdentity:
    """sha256 over the sorted ``relpath, size, sha256`` lines of a run's documents."""

    sha256: str
    files: int
    bytes: int
    unhashed_files: int
    manifest: str

    def to_json(self) -> dict[str, Any]:
        return {
            "sha256": self.sha256,
            "files": self.files,
            "bytes": self.bytes,
            "unhashed_files": self.unhashed_files,
            "manifest": self.manifest,
        }

    @staticmethod
    def from_json(data: Mapping[str, Any]) -> CorpusIdentity:
        where = "corpus"
        return CorpusIdentity(
            sha256=require(data, "sha256", str, where),
            files=require(data, "files", int, where),
            bytes=require(data, "bytes", int, where),
            unhashed_files=require(data, "unhashed_files", int, where),
            manifest=require(data, "manifest", str, where),
        )


@dataclass(frozen=True)
class Headline:
    """The run's headline counts, from analyze.summarize_run's overall slice."""

    evaluated: int
    skipped: int
    status_success_count: int
    success_count: int
    error_count: int
    clean_count: int
    clean_battery_count: int

    KEYS = ("evaluated", "skipped", "status_success_count", "success_count", "error_count",
            "clean_count", "clean_battery_count")

    def to_json(self) -> dict[str, int]:
        return {key: getattr(self, key) for key in self.KEYS}

    @staticmethod
    def from_json(data: Mapping[str, Any], where: str = "headline") -> Headline:
        return Headline(**{key: require(data, key, int, where) for key in Headline.KEYS})

    @staticmethod
    def empty() -> Headline:
        """A run that evaluated nothing (its journal was never written)."""
        return Headline(**{key: 0 for key in Headline.KEYS})


@dataclass(frozen=True)
class RunManifest:
    """run.json: what produced a corpus run, written at its start and finish."""

    label: str
    started_at: str
    target: str
    grparse: GrparseCommit
    stack: Stack
    harness: Harness
    host: Host
    config: dict[str, ConfigValue]
    run_id: str
    service: str | None = None
    resumed_at: tuple[str, ...] = ()
    finished_at: str | None = None
    corpus: CorpusIdentity | None = None
    headline: Headline | None = None
    wall_seconds: float | None = None
    exit_code: int | None = None

    def to_json(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "schema": SCHEMA,
            "label": self.label,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "target": self.target,
            "service": self.service,
            "grparse": self.grparse.to_json(),
            "stack": self.stack.to_json(),
            "harness": self.harness.to_json(),
            "host": self.host.to_json(),
            "config": dict(self.config),
            "run_id": self.run_id,
        }
        if self.resumed_at:
            out["resumed_at"] = list(self.resumed_at)
        if self.corpus is not None:
            out["corpus"] = self.corpus.to_json()
        if self.headline is not None:
            out["headline"] = self.headline.to_json()
        if self.wall_seconds is not None:
            out["wall_seconds"] = self.wall_seconds
        if self.exit_code is not None:
            out["exit_code"] = self.exit_code
        return out

    @staticmethod
    def from_json(data: Any) -> RunManifest:
        try:
            return RunManifest._read(data)
        except ManifestError:
            raise
        except SchemaError as error:
            raise ManifestError(str(error)) from error

    @staticmethod
    def _read(data: Any) -> RunManifest:
        if not isinstance(data, dict):
            raise ManifestError("run.json is not an object")
        where = "run.json"
        schema = require(data, "schema", str, where)
        if schema != SCHEMA:
            raise ManifestError(f"{where}: schema {schema!r}, expected {SCHEMA!r}")
        return RunManifest(
            label=require(data, "label", str, where),
            started_at=require(data, "started_at", str, where),
            target=require(data, "target", str, where),
            grparse=GrparseCommit.from_json(block(data, "grparse", where)),
            stack=Stack.from_json(block(data, "stack", where)),
            harness=Harness.from_json(block(data, "harness", where)),
            host=Host.from_json(block(data, "host", where)),
            config=dict(block(data, "config", where)),
            run_id=require(data, "run_id", str, where),
            service=optional(data, "service", str, where),
            resumed_at=str_list(data, "resumed_at", where) if "resumed_at" in data else (),
            finished_at=optional(data, "finished_at", str, where),
            corpus=CorpusIdentity.from_json(block(data, "corpus", where)) if "corpus" in data else None,
            headline=Headline.from_json(block(data, "headline", where)) if "headline" in data else None,
            wall_seconds=optional(data, "wall_seconds", (int, float), where) if "wall_seconds" in data else None,
            exit_code=optional(data, "exit_code", int, where) if "exit_code" in data else None,
        )
