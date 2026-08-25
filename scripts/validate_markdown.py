#!/usr/bin/env python3
"""Validate the native Markdown renderer against the reference serializer.

Development-time harness, never built into the image. For each input
Document (binary protobuf by default, protobuf-JSON with ``--json``) it runs
BOTH rendering paths:

  1. the reference path: ``scripts/document_json_bridge.py`` produces the
     canonical JSON, the model layer loads it, and its Markdown export runs
     with default parameters, and
  2. the native path: the ``grparse-markdown-tool`` binary built with the
     test suite (``cmake --build build --target grparse-markdown-tool``),

then compares the two renderings byte for byte and reports the first
difference with surrounding context.

Loading through the model layer means the reference rendering reflects the
model's load-time normalizations (provenance clamping, migration of
misplaced list items into synthesized list groups); the native renderer
applies the same normalizations, so the two legs stay comparable.

One normalization happens on the reference leg only. The wire's custom-field
and Struct payloads are unordered maps, and the bridge emits them in its
runtime's hash order, which is randomized per process: rendering the same
Document twice can order two custom meta fields differently. The native
renderer instead emits them in sorted key order, the order its canonical JSON
export also uses. To make the comparison deterministic this harness sorts the
keys of every object in the bridge's JSON before the model loads it, which
leaves declared fields untouched (their order carries no meaning) and puts the
custom part in the same order the native renderer produces.

Run from the checkout providing the model bindings so both this script and
the bridge resolve their imports:

  uv run --frozen python /path/to/gRParse/scripts/validate_markdown.py \\
      --tool /path/to/gRParse/build/grparse-markdown-tool \\
      doc1.bin doc2.bin
  uv run --frozen python .../validate_markdown.py --json doc.pb.json

Exit status: 0 when every input renders byte-identically, 1 on any
difference or conversion failure, 2 on argument errors.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

_SCRIPTS_DIR = Path(__file__).resolve().parent
_BRIDGE = _SCRIPTS_DIR / "document_json_bridge.py"
_DEFAULT_TOOL = _SCRIPTS_DIR.parent / "build" / "grparse-markdown-tool"


def _first_difference(a: str, b: str) -> str:
    limit = min(len(a), len(b))
    at = next((i for i in range(limit) if a[i] != b[i]), limit)
    context_a = a[max(0, at - 80) : at + 80]
    context_b = b[max(0, at - 80) : at + 80]
    return (
        f"first difference at byte {at} "
        f"(reference {len(a)} bytes, native {len(b)} bytes):\n"
        f"  reference: ...{context_a!r}...\n"
        f"  native:    ...{context_b!r}..."
    )


def _key_sorted(node: Any) -> Any:
    """Recursively reorder every object's keys, leaving arrays as they are."""
    if isinstance(node, dict):
        return {key: _key_sorted(node[key]) for key in sorted(node)}
    if isinstance(node, list):
        return [_key_sorted(entry) for entry in node]
    return node


def _reference_markdown(input_path: Path, as_json: bool) -> str:
    """Render *input_path* the reference way: bridge, model load, export."""
    from docling_core.types.doc.document import DoclingDocument

    mode = ["--json"] if as_json else []
    bridge_run = subprocess.run(
        [sys.executable, str(_BRIDGE), *mode, str(input_path)],
        capture_output=True,
    )
    if bridge_run.returncode != 0:
        raise RuntimeError(
            f"bridge exited {bridge_run.returncode}: "
            f"{bridge_run.stderr.decode(errors='replace').strip()}"
        )
    canonical = json.dumps(_key_sorted(json.loads(bridge_run.stdout.decode())))
    doc = DoclingDocument.model_validate_json(canonical)
    return doc.export_to_markdown()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="validate_markdown",
        description="Diff the native Markdown renderer against the reference serializer.",
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="Document files to render.")
    parser.add_argument(
        "--json",
        action="store_true",
        help="Treat inputs as protobuf-JSON instead of binary Documents.",
    )
    parser.add_argument(
        "--tool",
        type=Path,
        default=_DEFAULT_TOOL,
        help=f"Path to grparse-markdown-tool (default: {_DEFAULT_TOOL}).",
    )
    parser.add_argument(
        "--write-dir",
        type=Path,
        default=None,
        help="Directory to write both renderings to, for offline inspection.",
    )
    args = parser.parse_args(argv)

    if not args.tool.exists():
        print(f"error: native tool not found: {args.tool}", file=sys.stderr)
        return 2
    if args.write_dir is not None:
        args.write_dir.mkdir(parents=True, exist_ok=True)

    total = byte_equal = 0
    failed = False
    for input_path in args.inputs:
        total += 1
        try:
            reference_text = _reference_markdown(input_path, args.json)
        except Exception as exc:  # noqa: BLE001 - one failing input must not stop the run
            print(f"FAIL {input_path}: reference leg failed: {exc}")
            failed = True
            continue

        mode = ["--json"] if args.json else []
        # Bytes, not text: universal-newline decoding would rewrite a carriage
        # return the renderer emitted verbatim and fake a difference.
        native_run = subprocess.run(
            [str(args.tool), *mode, str(input_path)],
            capture_output=True,
        )
        if native_run.returncode != 0:
            print(
                f"FAIL {input_path}: native tool exited {native_run.returncode}: "
                f"{native_run.stderr.decode(errors='replace').strip()}"
            )
            failed = True
            continue
        native_text = native_run.stdout.decode()

        if args.write_dir is not None:
            stem = input_path.stem
            (args.write_dir / f"{stem}.reference.md").write_text(
                reference_text, newline=""
            )
            (args.write_dir / f"{stem}.native.md").write_text(native_text, newline="")

        if reference_text == native_text:
            byte_equal += 1
            print(f"OK   {input_path}: byte-equal")
        else:
            print(f"FAIL {input_path}: renderings differ")
            print(_first_difference(reference_text, native_text))
            failed = True

    print(f"\nsummary: {total} input(s), {byte_equal} byte-equal")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
