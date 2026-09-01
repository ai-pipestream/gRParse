"""Where the corpus comes from: an S3-compatible bucket, listed and fetched
into memory. Object bytes never touch the filesystem."""

from __future__ import annotations

import fnmatch
from dataclasses import dataclass
from typing import Protocol

from .config import Config


class SourceUnreachable(RuntimeError):
    """The bucket could not be listed; the run exits with the skip code."""


@dataclass(frozen=True)
class ObjectRef:
    key: str
    size: int


class ObjectSource(Protocol):
    def list_objects(self) -> list[ObjectRef]: ...
    def fetch(self, key: str) -> bytes: ...


def select_keys(refs: list[ObjectRef], include: tuple[str, ...], exclude: tuple[str, ...],
                limit: int | None) -> list[ObjectRef]:
    """Include globs (any matches, empty means all) minus exclude globs,
    sorted by key, cut at ``limit``."""
    chosen = []
    for ref in sorted(refs, key=lambda r: r.key):
        if ref.key.endswith("/"):
            continue
        if include and not any(fnmatch.fnmatch(ref.key, glob) for glob in include):
            continue
        if any(fnmatch.fnmatch(ref.key, glob) for glob in exclude):
            continue
        chosen.append(ref)
    return chosen[:limit] if limit is not None else chosen


class Boto3Source:
    """The real thing. boto3 is imported here so the tests never need it."""

    def __init__(self, config: Config) -> None:
        import boto3
        from botocore.config import Config as BotoConfig

        self._bucket = config.bucket
        self._prefix = config.prefix
        self._client = boto3.client(
            "s3", endpoint_url=config.endpoint, region_name=config.region,
            aws_access_key_id=config.access_key, aws_secret_access_key=config.secret_key,
            config=BotoConfig(s3={"addressing_style": "path"}, retries={"max_attempts": 3},
                              connect_timeout=10, read_timeout=120))

    def list_objects(self) -> list[ObjectRef]:
        from botocore.exceptions import BotoCoreError, ClientError

        refs: list[ObjectRef] = []
        try:
            paginator = self._client.get_paginator("list_objects_v2")
            for page in paginator.paginate(Bucket=self._bucket, Prefix=self._prefix):
                for entry in page.get("Contents", []) or []:
                    refs.append(ObjectRef(key=entry["Key"], size=int(entry.get("Size", 0))))
        except (BotoCoreError, ClientError, OSError) as error:
            raise SourceUnreachable(f"{self._bucket}: {error}") from error
        return refs

    def fetch(self, key: str) -> bytes:
        response = self._client.get_object(Bucket=self._bucket, Key=key)
        return response["Body"].read()
