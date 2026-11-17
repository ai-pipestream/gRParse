"""Run gRParse's CPU OCR over page images and keep its per-line boxes and text.

Usage: python3 fetch_lines.py PAGES_DIR OUT_DIR [TARGET]

Each page PNG goes to a gRParse CPU server (default 127.0.0.1:50291) with
force_ocr and a JSON export; the document's text items carry one prov entry
per OCR line (bbox + charspan), which prep_calib.py turns into line crops and
reference text. Needs grpcurl on PATH; the server must serve reflection.
"""
import base64
import json
import subprocess
import sys
from pathlib import Path


def main() -> None:
    pages, out = Path(sys.argv[1]), Path(sys.argv[2])
    target = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1:50291"
    out.mkdir(parents=True, exist_ok=True)
    for page in sorted(pages.glob("*.png")):
        body = base64.b64encode(page.read_bytes()).decode()
        req = {"request": {"sources": [{"file": {"base64_string": body, "filename": page.name}}],
                           "options": {"to_formats": ["OUTPUT_FORMAT_JSON"], "force_ocr": True}}}
        run = subprocess.run(["grpcurl", "-plaintext", "-max-msg-sz", "600000000", "-max-time", "600", "-d", "@",
                              target, "ai.pipestream.parse.v1.ParseService/ConvertSource"],
                             input=json.dumps(req), capture_output=True, text=True, check=False)
        if run.returncode != 0:
            raise SystemExit(f"{page.name}: grpcurl rc={run.returncode}: {run.stderr.strip()[:400]}")
        doc = json.loads(json.loads(run.stdout)["response"]["document"]["exports"]["json"])
        lines = []
        for item in doc.get("texts", []):
            base = next(iter(item.values()))
            base = base.get("base", base)
            text = base.get("text", "")
            source = (base.get("source") or [{}])[0].get("collector", {})
            if source.get("model") != "rapidocr":
                continue
            for prov in base.get("prov", []):
                span = prov.get("charspan", {})
                bb = prov["bbox"]
                lines.append({"page_no": prov["page_no"], "bbox": [bb["l"], bb["t"], bb["r"], bb["b"]],
                              "origin": bb.get("coord_origin"), "polygon": prov.get("polygon"),
                              "text": text[span.get("start", 0):span.get("end", len(text))]})
        sizes = {k: v["size"] for k, v in doc.get("pages", {}).items()}
        (out / f"{page.stem}.json").write_text(json.dumps({"page": page.name, "sizes": sizes, "lines": lines},
                                                          ensure_ascii=False), encoding="utf-8")
        print(f"{page.name}: {len(lines)} OCR lines")


if __name__ == "__main__":
    main()
