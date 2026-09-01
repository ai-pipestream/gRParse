#!/usr/bin/env python3
"""Seed an S3 bucket with the sibling repositories' own fixtures.

    uv run --with boto3 python eval/s3/seed_from_workspace.py [--bucket grparse-eval] [--dry-run] [DIR ...]

Uploads every document-looking file under the given directories (the
defaults are the family's fixture, sample and demo trees) as
``<repo>/<relative path>``. The S3 endpoint and credentials come from the
environment exactly as for run.py (EVAL_S3_ENDPOINT, EVAL_S3_REGION,
AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY or the EVAL_S3_* twins); the
bucket is created when it does not exist. Prints the count of objects
seeded per extension and per repository. Never reads outside the listed
directories.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORKSPACE = REPO.parent if REPO.parent.name != "worktrees" else REPO.parents[1]

DOCUMENT_EXTENSIONS = frozenset({
    "pdf", "docx", "doc", "xlsx", "xls", "pptx", "ppt", "odt", "ods", "odp", "rtf", "html", "htm", "xml", "md",
    "eml", "mbox", "epub", "png", "jpg", "jpeg", "tif", "tiff", "warc", "warc.gz", "wav", "mp3", "csv", "txt", "ebc",
})
# Companions a document needs to parse (an EBCDIC layout beside its .ebc).
COMPANION_SUFFIXES = (".layout.json",)
EXCLUDED_DIRS = frozenset({
    "node_modules", "target", "build", "build-cuda", "vendor", ".git", "out", "models", "docs", "_deps",
    "site-packages", ".venv", "venv", "Testing", "dist", "__pycache__", "renders", "baseline", "truth",
})
EXCLUDED_STEMS = frozenset({"readme", "agents", "changelog", "license", "contributing", "notice", "claude"})
EXCLUDED_NAMES = frozenset({"package.json", "package-lock.json", "cargo.lock", "uv.lock", "poetry.lock",
                            "requirements.txt", "cmakelists.txt", "pom.xml", "build.xml", "settings.xml",
                            "web.xml", "logback.xml", "log4j2.xml"})
MAX_BYTES = 64 * 1024 * 1024

DEFAULT_SOURCES = [
    "gRParse/tests/golden/corpus",
    "grpc-libreoffice/tests", "grpc-libreoffice/fixtures", "grpc-libreoffice/samples",
    "grpc-pdf-inspector/tests", "grpc-pdf-inspector/demos",
    "grpc-epub/tests", "grpc-epub/fixtures", "grpc-epub/demos",
    "grpc-email/tests", "grpc-email/fixtures", "grpc-email/demos",
    "grpc-xml/tests", "grpc-xml/fixtures", "grpc-xml/demos",
    "grpc-markup/tests", "grpc-markup/fixtures", "grpc-markup/demos",
    "grpc-lol-html/tests", "grpc-lol-html/fixtures", "grpc-lol-html/demos",
    "grpc-ebcdic/tests", "grpc-ebcdic/demos",
    "grpc-calamine/tests", "grpc-calamine/demos",
    "grpc-asr/tests", "grpc-asr/samples", "grpc-asr/demos",
    "fastwarc-grpc/tests", "fastwarc-grpc/demos",
    "grPOIc/tests", "grPOIc/samples", "grPOIc/grpoic-api/src/test",
]


def extension_of(name: str) -> str:
    lower = name.lower()
    for suffix in ("warc.gz", "tar.gz"):
        if lower.endswith("." + suffix):
            return suffix
    return lower.rsplit(".", 1)[1] if "." in lower else ""


def wanted(path: Path, root: Path) -> bool:
    """Whether a file under ``root`` is a document worth seeding."""
    relative = path.relative_to(root)
    parts = relative.parts[:-1]
    if any(part in EXCLUDED_DIRS or part.startswith(".") for part in parts):
        return False
    name = path.name
    lower = name.lower()
    if lower.startswith(".") or lower in EXCLUDED_NAMES:
        return False
    if lower.endswith(COMPANION_SUFFIXES):
        return True
    ext = extension_of(name)
    if ext not in DOCUMENT_EXTENSIONS:
        return False
    stem = lower[: -(len(ext) + 1)]
    if ext in ("md", "txt") and (stem in EXCLUDED_STEMS or stem.startswith(("readme", "changelog"))):
        return False
    return True


def repo_of(root: Path, workspace: Path) -> str:
    try:
        return root.relative_to(workspace).parts[0]
    except ValueError:
        return root.name


def collect(sources: list[Path], workspace: Path) -> list[tuple[str, Path]]:
    """(key, path) for every wanted file; keys are <repo>/<path relative to the repo>."""
    found: dict[str, Path] = {}
    for root in sources:
        if not root.is_dir():
            continue
        repo = repo_of(root, workspace)
        repo_root = workspace / repo if (workspace / repo).is_dir() else root
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.is_symlink() or not wanted(path, root):
                continue
            if path.stat().st_size == 0 or path.stat().st_size > MAX_BYTES:
                continue
            try:
                relative = path.relative_to(repo_root)
            except ValueError:
                relative = path.relative_to(root)
            found[f"{repo}/{relative.as_posix()}"] = path
    return sorted(found.items())


def summarize(entries: list[tuple[str, Path]]) -> tuple[Counter, Counter]:
    by_extension: Counter = Counter()
    by_repo: Counter = Counter()
    for key, _ in entries:
        name = key.rsplit("/", 1)[-1]
        by_extension[extension_of(name) if not name.lower().endswith(COMPANION_SUFFIXES) else "layout.json"] += 1
        by_repo[key.split("/", 1)[0]] += 1
    return by_extension, by_repo


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", nargs="*", type=Path, help="directories to scan (default: the family's fixtures)")
    parser.add_argument("--bucket", default=os.environ.get("EVAL_S3_BUCKET", "grparse-eval"))
    parser.add_argument("--workspace", type=Path, default=WORKSPACE)
    parser.add_argument("--dry-run", action="store_true", help="list what would be uploaded, upload nothing")
    args = parser.parse_args(argv)
    workspace = args.workspace.resolve()
    sources = [p.resolve() for p in args.sources] if args.sources else [workspace / s for s in DEFAULT_SOURCES]
    entries = collect(sources, workspace)
    by_extension, by_repo = summarize(entries)
    if not entries:
        print("nothing to seed", file=sys.stderr)
        return 1
    if not args.dry_run:
        endpoint = os.environ.get("EVAL_S3_ENDPOINT")
        access = os.environ.get("AWS_ACCESS_KEY_ID") or os.environ.get("EVAL_S3_ACCESS_KEY")
        secret = os.environ.get("AWS_SECRET_ACCESS_KEY") or os.environ.get("EVAL_S3_SECRET_KEY")
        if not endpoint or not access or not secret:
            print("EVAL_S3_ENDPOINT and the access/secret key pair must be set", file=sys.stderr)
            return 2
        import boto3
        from botocore.config import Config as BotoConfig
        from botocore.exceptions import ClientError

        client = boto3.client("s3", endpoint_url=endpoint, region_name=os.environ.get("EVAL_S3_REGION", "us-east-1"),
                              aws_access_key_id=access, aws_secret_access_key=secret,
                              config=BotoConfig(s3={"addressing_style": "path"}))
        try:
            client.head_bucket(Bucket=args.bucket)
        except ClientError:
            client.create_bucket(Bucket=args.bucket)
        for key, path in entries:
            with path.open("rb") as handle:
                client.put_object(Bucket=args.bucket, Key=key, Body=handle)
    print(f"{'would seed' if args.dry_run else 'seeded'} {len(entries)} objects into {args.bucket}")
    print("per extension: " + ", ".join(f"{ext or '(none)'}={n}" for ext, n in sorted(by_extension.items())))
    print("per repo: " + ", ".join(f"{repo}={n}" for repo, n in sorted(by_repo.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
