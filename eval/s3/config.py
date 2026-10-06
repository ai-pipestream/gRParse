"""Configuration from the environment. The one file read is the optional
document password list EVAL_S3_PASSWORDS_FILE names.

Credentials and document passwords are read and handed to their clients,
never printed and never written into a report.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path


class ConfigError(ValueError):
    """A required variable is missing or malformed; the message names it."""


def _globs(raw: str | None) -> tuple[str, ...]:
    if not raw:
        return ()
    return tuple(part.strip() for part in raw.split(",") if part.strip())


def _int(env: Mapping[str, str], name: str, default: int | None) -> int | None:
    raw = env.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = int(raw)
    except ValueError as error:
        raise ConfigError(f"{name} must be an integer, got {raw!r}") from error
    if value < 0:
        raise ConfigError(f"{name} must not be negative, got {value}")
    return value


# gRParse's own bounds on one call's candidates (include/grparse/document_passwords.h).
MAX_DOCUMENT_PASSWORDS = 16
MAX_DOCUMENT_PASSWORD_BYTES = 1024


def _passwords(env: Mapping[str, str]) -> tuple[str, ...]:
    """Candidate passwords for the corpus's encrypted documents: one per line
    of the UTF-8 file EVAL_S3_PASSWORDS_FILE names, blank lines skipped. Every
    conversion sends them, and gRParse tries them only on a document that
    will not open without one. Errors name the file and the bound, never a
    password."""
    path = (env.get("EVAL_S3_PASSWORDS_FILE") or "").strip()
    if not path:
        return ()
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise ConfigError(f"EVAL_S3_PASSWORDS_FILE could not be read ({type(error).__name__})") from None
    candidates = tuple(line for line in text.splitlines() if line.strip())
    if len(candidates) > MAX_DOCUMENT_PASSWORDS:
        raise ConfigError(f"EVAL_S3_PASSWORDS_FILE lists {len(candidates)} passwords; gRParse takes at most "
                          f"{MAX_DOCUMENT_PASSWORDS} per call")
    if any(len(candidate.encode("utf-8")) > MAX_DOCUMENT_PASSWORD_BYTES for candidate in candidates):
        raise ConfigError(f"EVAL_S3_PASSWORDS_FILE has a password over {MAX_DOCUMENT_PASSWORD_BYTES} bytes")
    return candidates


def _flag(env: Mapping[str, str], name: str, default: bool) -> bool:
    raw = env.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("0", "off", "false", "no", "")


@dataclass(frozen=True)
class Config:
    endpoint: str
    bucket: str
    prefix: str
    access_key: str
    secret_key: str
    region: str
    target: str
    max_objects: int | None
    include: tuple[str, ...]
    exclude: tuple[str, ...]
    out: Path
    label: str
    # Conversions per object; the second one is the byte-identity check.
    repeat: int
    # Objects per extension that are parsed once more under a name with no
    # extension, so the origin has to come from the bytes.
    sniff_per_extension: int
    # A skipped object (no EBCDIC layout beside it) fails the run.
    require: bool
    # Client deadline per conversion, seconds. A document that runs past it
    # is that object's failure; the run moves on instead of waiting.
    convert_timeout: float
    # Candidate passwords sent with every conversion (EVAL_S3_PASSWORDS_FILE);
    # kept out of repr so no log line or report can print them.
    document_passwords: tuple[str, ...] = field(default=(), repr=False)

    @classmethod
    def from_env(cls, env: Mapping[str, str], repo_root: Path) -> Config:
        missing = [name for name in ("EVAL_S3_ENDPOINT", "EVAL_S3_BUCKET") if not env.get(name)]
        access = env.get("AWS_ACCESS_KEY_ID") or env.get("EVAL_S3_ACCESS_KEY") or ""
        secret = env.get("AWS_SECRET_ACCESS_KEY") or env.get("EVAL_S3_SECRET_KEY") or ""
        if not access:
            missing.append("AWS_ACCESS_KEY_ID (or EVAL_S3_ACCESS_KEY)")
        if not secret:
            missing.append("AWS_SECRET_ACCESS_KEY (or EVAL_S3_SECRET_KEY)")
        if missing:
            raise ConfigError("missing environment: " + ", ".join(missing))
        repeat = _int(env, "EVAL_S3_REPEAT", 2) or 1
        return cls(
            endpoint=env["EVAL_S3_ENDPOINT"].strip(),
            bucket=env["EVAL_S3_BUCKET"].strip(),
            prefix=(env.get("EVAL_S3_PREFIX") or "").strip(),
            access_key=access,
            secret_key=secret,
            region=(env.get("EVAL_S3_REGION") or "us-east-1").strip(),
            target=(env.get("GRPARSE_TARGET") or "localhost:50051").strip(),
            max_objects=_int(env, "EVAL_S3_MAX_OBJECTS", None),
            include=_globs(env.get("EVAL_S3_INCLUDE")),
            exclude=_globs(env.get("EVAL_S3_EXCLUDE")),
            out=Path(env.get("EVAL_OUT") or (repo_root / "eval" / "out")),
            label=(env.get("EVAL_LABEL") or "live").strip(),
            repeat=max(1, repeat),
            sniff_per_extension=_int(env, "EVAL_S3_SNIFF_PER_EXTENSION", 1) or 0,
            require=_flag(env, "EVAL_REQUIRE", False),
            convert_timeout=float(_int(env, "EVAL_S3_CONVERT_TIMEOUT", 600) or 600),
            document_passwords=_passwords(env),
        )

    def public_endpoint(self) -> str:
        """The endpoint as a report may print it: scheme and host only."""
        text = self.endpoint
        if "@" in text:
            text = text.rsplit("@", 1)[1]
        return text
