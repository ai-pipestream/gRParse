#!/usr/bin/env python3
"""Fold the report.json of several compare_vlm.py runs into one matrix.

    python eval/summarize.py eval/out            # every <label>/report.json under it

One row per (label, file): who was timed, on what, how fast, and how well
the two markdowns agreed. Legs that reused a saved oracle (VLM_REFERENCE)
show the oracle column as the reference label rather than a time.
"""

import json
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else "eval/out")
rows = []
for report_path in sorted(root.glob("*/report.json")):
    report = json.loads(report_path.read_text())
    device = report.get("vlm", {}).get("device_name") or report.get("vlm", {}).get("status", "?")
    collectors = report.get("grparse_collectors") or "default route"
    reference = report.get("vlm_reference")
    for r in report["results"]:
        a = r["agreement"]
        vlm = f"ref: {Path(reference).name}" if reference else f"{r['vlm_seconds']}s @ {r['vlm_tokens_per_second']} tok/s"
        rows.append((report["label"], r["file"], r["pages"], collectors, f"{r['grparse_seconds']}s",
                     device if not reference else "(saved)", vlm, a["letter_similarity"], a["word_recall"],
                     a["word_precision"], f"{a['headings'][0]}/{a['headings'][1]}",
                     f"{a['table_rows'][0]}/{a['table_rows'][1]}"))
header = ("leg", "file", "pages", "gRParse route", "gRParse", "VLM device", "VLM", "letter sim",
          "word recall", "word precision", "headings g/v", "table rows g/v")
print("| " + " | ".join(header) + " |")
print("|" + "---|" * len(header))
for row in rows:
    print("| " + " | ".join(str(c) for c in row) + " |")
