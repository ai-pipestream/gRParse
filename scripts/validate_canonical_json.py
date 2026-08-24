#!/usr/bin/env python3
"""Validate the native canonical JSON renderer against the Python bridge.

Development-time harness, never built into the image. For each input
Document (binary protobuf by default, protobuf-JSON with ``--json``) it runs
BOTH conversion paths:

  1. the reference path: ``scripts/document_json_bridge.py`` in an
     environment providing the upstream schema bindings, and
  2. the native path: the ``grparse-canonical-json-tool`` binary built with
     the test suite (``ninja -C build grparse-canonical-json-tool``),

then compares (a) the parsed JSON trees for exact equality and (b) the two
outputs byte for byte. Tree equality is the correctness floor; byte
equality is expected for all real service output. The one known divergence
class is deliberate: the wire's custom-field and Struct maps are unordered,
the reference dump emits them in its runtime's hash-table order, and the
native renderer emits them in sorted key order instead, so inputs carrying
two or more custom fields (or multi-key Struct payloads) under one node can
be tree-equal yet byte-different.

Run from the schema-bindings checkout so the bridge resolves its imports:

  uv run --frozen python /path/to/gRParse/scripts/validate_canonical_json.py \\
      --tool /path/to/gRParse/build/grparse-canonical-json-tool \\
      doc1.bin doc2.bin
  uv run --frozen python .../validate_canonical_json.py --json doc.pb.json

Exit status: 0 when every input is tree-equal (byte differences are
reported but only fail the run under ``--require-bytes``), 1 on any tree
mismatch or conversion failure, 2 on argument errors.
"""

from __future__ import annotations

import argparse
import difflib
import json
import subprocess
import sys
from pathlib import Path

_SCRIPTS_DIR = Path(__file__).resolve().parent
_BRIDGE = _SCRIPTS_DIR / "document_json_bridge.py"
_DEFAULT_TOOL = _SCRIPTS_DIR.parent / "build" / "grparse-canonical-json-tool"


def _first_difference(a: str, b: str) -> str:
    limit = min(len(a), len(b))
    at = next((i for i in range(limit) if a[i] != b[i]), limit)
    context_a = a[max(0, at - 60) : at + 60]
    context_b = b[max(0, at - 60) : at + 60]
    return f"first difference at byte {at}:\n  bridge: ...{context_a!r}...\n  native: ...{context_b!r}..."


def _tree_diff(bridge: dict, native: dict) -> str:
    a = json.dumps(bridge, indent=2, sort_keys=True).splitlines(keepends=True)
    b = json.dumps(native, indent=2, sort_keys=True).splitlines(keepends=True)
    diff = list(difflib.unified_diff(a, b, "bridge", "native", n=2))
    head = "".join(diff[:80])
    if len(diff) > 80:
        head += f"... ({len(diff) - 80} more diff lines)\n"
    return head


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="validate_canonical_json",
        description="Diff the native canonical JSON renderer against the Python bridge.",
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="Document files to convert.")
    parser.add_argument(
        "--json",
        action="store_true",
        help="Treat inputs as protobuf-JSON instead of binary Documents.",
    )
    parser.add_argument(
        "--tool",
        type=Path,
        default=_DEFAULT_TOOL,
        help=f"Path to grparse-canonical-json-tool (default: {_DEFAULT_TOOL}).",
    )
    parser.add_argument(
        "--require-bytes",
        action="store_true",
        help="Fail the run when tree-equal outputs are not byte-equal.",
    )
    args = parser.parse_args(argv)

    if not args.tool.exists():
        print(f"error: native tool not found: {args.tool}", file=sys.stderr)
        return 2

    total = tree_equal = byte_equal = 0
    failed = False
    for input_path in args.inputs:
        total += 1
        mode = ["--json"] if args.json else []
        bridge_run = subprocess.run(
            [sys.executable, str(_BRIDGE), *mode, str(input_path)],
            capture_output=True,
            text=True,
        )
        if bridge_run.returncode != 0:
            print(f"FAIL {input_path}: bridge exited {bridge_run.returncode}: "
                  f"{bridge_run.stderr.strip()}")
            failed = True
            continue
        # The bridge prints the JSON plus a trailing newline; the tool writes
        # the bare rendering.
        bridge_text = bridge_run.stdout
        if bridge_text.endswith("\n"):
            bridge_text = bridge_text[:-1]

        native_run = subprocess.run(
            [str(args.tool), *mode, str(input_path)],
            capture_output=True,
            text=True,
        )
        if native_run.returncode != 0:
            print(f"FAIL {input_path}: native tool exited {native_run.returncode}: "
                  f"{native_run.stderr.strip()}")
            failed = True
            continue
        native_text = native_run.stdout

        try:
            bridge_tree = json.loads(bridge_text)
            native_tree = json.loads(native_text)
        except json.JSONDecodeError as exc:
            print(f"FAIL {input_path}: output is not valid JSON: {exc}")
            failed = True
            continue

        if bridge_tree != native_tree:
            print(f"FAIL {input_path}: trees differ")
            print(_tree_diff(bridge_tree, native_tree))
            failed = True
            continue
        tree_equal += 1

        if bridge_text == native_text:
            byte_equal += 1
            print(f"OK   {input_path}: tree-equal, byte-equal")
        else:
            print(f"WARN {input_path}: tree-equal, byte-DIFFERENT")
            print(_first_difference(bridge_text, native_text))
            if args.require_bytes:
                failed = True

    print(
        f"\nsummary: {total} input(s), {tree_equal} tree-equal, "
        f"{byte_equal} byte-equal"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
