"""Strict field readers for the harness's own JSON records (run.json, results.jsonl).

Each reader returns the field when it is present with the expected type and
raises ``SchemaError`` naming the record and field otherwise. Nothing reads
as a default: these files are written by this harness, so a gap is a bug.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


class SchemaError(ValueError):
    """A record the harness wrote does not have the shape it writes."""


def require[T](data: Mapping[str, Any], name: str, kind: type[T] | tuple[type, ...], where: str) -> T:
    if name not in data:
        raise SchemaError(f"{where}: missing {name!r}")
    value = data[name]
    # bool is an int subclass; a numeric field must not accept true/false.
    numeric = kind in (int, float) or kind == (int, float)
    if isinstance(value, bool) and numeric:
        raise SchemaError(f"{where}: {name!r} is a bool, not a number")
    if not isinstance(value, kind):
        raise SchemaError(f"{where}: {name!r} is {type(value).__name__}")
    return value  # type: ignore[return-value]


def optional[T](data: Mapping[str, Any], name: str, kind: type[T] | tuple[type, ...], where: str) -> T | None:
    """A field that may be null; it must still be present."""
    if name not in data:
        raise SchemaError(f"{where}: missing {name!r}")
    return None if data[name] is None else require(data, name, kind, where)


def block(data: Mapping[str, Any], name: str, where: str) -> Mapping[str, Any]:
    return require(data, name, dict, where)


def str_list(data: Mapping[str, Any], name: str, where: str) -> tuple[str, ...]:
    values = require(data, name, list, where)
    if not all(isinstance(v, str) for v in values):
        raise SchemaError(f"{where}: {name!r} holds a non-string")
    return tuple(values)


def typed_map(data: Mapping[str, Any], name: str, value_kind: type, where: str) -> dict[str, Any]:
    """A JSON object whose keys are strings and whose values are all ``value_kind``."""
    values = require(data, name, dict, where)
    for key, value in values.items():
        if not isinstance(key, str) or isinstance(value, bool) != (value_kind is bool) \
                or not isinstance(value, value_kind):
            raise SchemaError(f"{where}: {name!r} holds {key!r}: {value!r}")
    return dict(values)
