#!/usr/bin/env python3
"""Run every test_* function in this directory without a test framework.

    uv run python eval/s3/tests/run_tests.py

The files are also plain pytest modules. Exit 0 means every test passed.
"""

from __future__ import annotations

import importlib
import sys
import traceback
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))


def main() -> int:
    here = Path(__file__).resolve().parent
    passed = failed = 0
    for module_path in sorted(here.glob("test_*.py")):
        module = importlib.import_module(f"s3.tests.{module_path.stem}")
        for name in sorted(dir(module)):
            if not name.startswith("test_"):
                continue
            try:
                getattr(module, name)()
                passed += 1
            except Exception:  # noqa: BLE001
                failed += 1
                print(f"FAIL {module_path.stem}.{name}")
                traceback.print_exc()
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
