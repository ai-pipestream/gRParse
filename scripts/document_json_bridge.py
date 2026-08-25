#!/usr/bin/env python3
"""Convert a Document produced by this service into upstream-canonical JSON.

This script is the validation oracle for the native canonical JSON renderer
(``render_canonical_json``); ``scripts/validate_canonical_json.py`` diffs
the two paths.

Input is a serialized ``ai.pipestream.document.v1.Document`` message, either
as binary protobuf (default) or as protobuf-JSON (``--json``), e.g. the
``response.document.doc`` payload of a ConvertSource call. The service wire
schema is field-number-identical to the upstream schema dialect except for one
additive extension: the ``SourceType`` oneof carries an extra collector
attribution arm. Because of that, the bytes parse directly with the upstream
dialect's bindings; the extension entries surface as ``SourceType`` messages
whose oneof is unset and are dropped here.

Pipeline:
  1. Parse the input with the upstream dialect's protobuf bindings.
  2. Drop every ``SourceType`` entry whose oneof is unset (collector
     attribution extension entries unknown to the upstream dialect).
  3. Rewrite the identity header: ``schema_name`` becomes "DoclingDocument"
     (the wire value "docling_document_v2" is service-internal) and
     ``version`` becomes the bindings' current schema version.
  4. Convert the message to the model layer and export it as canonical JSON,
     using the same ``json.dumps`` arguments as the model's own
     ``save_as_json`` (``indent=2``, default ``ensure_ascii``).

The script needs a Python environment that provides the ``docling_core``
checkout with the proto bindings and converters, e.g.:

  uv run --project /work/worktrees/docling-core \\
      scripts/document_json_bridge.py document.bin -o document.json

  uv run --project /work/worktrees/docling-core \\
      scripts/document_json_bridge.py --json --validate doc.pb.json -o out.json

Exit status: 0 on success, 1 on conversion failure, 2 on argument errors,
3 when ``--validate`` fails to re-load the produced JSON.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from google.protobuf import json_format

from docling_core.proto import docling_document_pb2 as pb2
from docling_core.proto import proto_to_docling_document
from docling_core.types.doc.common.constants import CURRENT_VERSION
from docling_core.types.doc.document import DoclingDocument

_SOURCE_TYPE_FULL_NAME = pb2.SourceType.DESCRIPTOR.full_name


def _drop_unset_source_entries(msg) -> int:
    """Recursively remove SourceType entries whose oneof arm is unset.

    Such entries come from a producer that used an extension arm the upstream
    dialect does not know; keeping them would surface empty objects in the
    output. Returns the number of dropped entries.
    """
    dropped = 0
    for field, value in msg.ListFields():
        if field.type != field.TYPE_MESSAGE:
            continue
        # protobuf >= 7 removed FieldDescriptor.label; prefer is_repeated.
        repeated = getattr(field, "is_repeated", None)
        if repeated is None:
            repeated = field.label == field.LABEL_REPEATED
        if repeated:
            if field.message_type.GetOptions().map_entry:
                value_field = field.message_type.fields_by_name["value"]
                if value_field.type == value_field.TYPE_MESSAGE:
                    for entry in value.values():
                        dropped += _drop_unset_source_entries(entry)
                continue
            if field.message_type.full_name == _SOURCE_TYPE_FULL_NAME:
                for index in reversed(range(len(value))):
                    if value[index].WhichOneof("source") is None:
                        del value[index]
                        dropped += 1
                continue
            for entry in value:
                dropped += _drop_unset_source_entries(entry)
        else:
            dropped += _drop_unset_source_entries(value)
    return dropped


def _conforming_custom_name(key: str) -> bool:
    """True when the key already satisfies the namespace__field_name rule."""
    parts = key.split("__", 1)
    return len(parts) == 2 and bool(parts[0]) and bool(parts[1])


def _normalize_custom_field_names(msg) -> int:
    """Rekey custom meta fields into the namespace__field_name format.

    Fleet producers use free-form names ("collector_warnings:pdf",
    "epub.version", "bookmark:intro"); the upstream dialect requires a
    namespace prefix separated by a double underscore, so non-conforming
    keys move under the "pipestream" namespace with every character outside
    [A-Za-z0-9_] folded to an underscore. Returns the number of renames.
    """
    renamed = 0
    for field, value in msg.ListFields():
        if field.type != field.TYPE_MESSAGE:
            continue
        repeated = getattr(field, "is_repeated", None)
        if repeated is None:
            repeated = field.label == field.LABEL_REPEATED
        if repeated and field.message_type.GetOptions().map_entry:
            value_field = field.message_type.fields_by_name["value"]
            if field.name == "custom_fields":
                # Sorted so collision suffixes are assigned deterministically
                # (protobuf map iteration order is not); the native renderer
                # assigns them in the same sorted-original-key order.
                for key in sorted(k for k in value if not _conforming_custom_name(k)):
                    base = "pipestream__" + re.sub(r"[^A-Za-z0-9_]", "_", key)
                    new_key = base
                    suffix = 2
                    while new_key in value:
                        new_key = f"{base}_{suffix}"
                        suffix += 1
                    value[new_key].CopyFrom(value[key])
                    del value[key]
                    renamed += 1
            elif value_field.type == value_field.TYPE_MESSAGE:
                for entry in value.values():
                    renamed += _normalize_custom_field_names(entry)
            continue
        if repeated:
            for entry in value:
                renamed += _normalize_custom_field_names(entry)
        else:
            renamed += _normalize_custom_field_names(value)
    return renamed


def _parse_input(data: bytes, as_json: bool) -> pb2.DoclingDocument:
    msg = pb2.DoclingDocument()
    if as_json:
        # Field names are identical across the two dialects; unknown fields
        # (extension-only content) are ignored.
        json_format.Parse(
            data.decode("utf-8"), msg, ignore_unknown_fields=True
        )
    else:
        msg.ParseFromString(data)
    return msg


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="document_json_bridge",
        description=(
            "Convert a Document message from this service into "
            "upstream-canonical JSON."
        ),
    )
    parser.add_argument(
        "input",
        type=Path,
        help="Input file: binary Document (default) or protobuf-JSON (--json).",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Treat the input file as protobuf-JSON instead of binary.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output file for the canonical JSON (default: stdout).",
    )
    parser.add_argument(
        "--indent",
        type=int,
        default=2,
        help="JSON indent width; 2 matches the model's save_as_json default.",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help=(
            "Re-load the produced JSON through the model layer and exit "
            "nonzero if it does not validate."
        ),
    )
    args = parser.parse_args(argv)

    try:
        data = args.input.read_bytes()
    except OSError as exc:
        print(f"error: cannot read {args.input}: {exc}", file=sys.stderr)
        return 2

    try:
        msg = _parse_input(data, as_json=args.json)
        dropped = _drop_unset_source_entries(msg)
        renamed = _normalize_custom_field_names(msg)

        # Identity rewrite: the service wire header carries a
        # service-internal schema_name; the canonical output declares the
        # upstream identity and the bindings' current schema version.
        msg.schema_name = "DoclingDocument"
        msg.version = CURRENT_VERSION

        doc = proto_to_docling_document(msg)
        out_text = json.dumps(doc.export_to_dict(), indent=args.indent)
    except Exception as exc:  # noqa: BLE001 - single conversion failure exit
        print(f"error: conversion failed: {exc}", file=sys.stderr)
        return 1

    if args.output is not None:
        args.output.write_text(out_text, encoding="utf-8")
    else:
        print(out_text)

    if dropped:
        print(
            f"note: dropped {dropped} source entr"
            f"{'y' if dropped == 1 else 'ies'} with no representable arm",
            file=sys.stderr,
        )
    if renamed:
        print(
            f"note: renamed {renamed} custom meta field"
            f"{'' if renamed == 1 else 's'} into the pipestream namespace",
            file=sys.stderr,
        )

    if args.validate:
        try:
            DoclingDocument.model_validate_json(out_text)
        except Exception as exc:  # noqa: BLE001 - validation failure exit
            print(f"error: output failed validation: {exc}", file=sys.stderr)
            return 3
        print("validation: OK", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
