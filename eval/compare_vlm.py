#!/usr/bin/env python3
"""Compare gRParse's own conversion with an open VLM served as an oracle.

For every PDF in the corpus the script asks a running gRParse for markdown
(ConvertSource, OUTPUT_FORMAT_MARKDOWN) and asks an OpenAI-compatible
vision endpoint for markdown page by page (pages rasterized with pdftoppm),
then scores the two against each other and records timing. The VLM is an
oracle, not ground truth: agreement says the two read the page the same
way, disagreement says where to look.

Environment (all required unless noted; missing means exit 77, the CTest
skip code, so the battery is opt-in):
  GRPARSE_TARGET   host:port of gRParse's gRPC (plaintext)
  VLM_ENDPOINT     http(s) base of the vision endpoint (/v1/chat/completions)
  EVAL_CORPUS      a directory of PDFs or a colon-separated list of PDFs
  EVAL_OUT         output directory (default: eval/out)
  VLM_PROMPT       optional prompt override
  EVAL_DPI         raster DPI for the VLM pages (default 150)
  EVAL_LABEL       a label for this run (e.g. "cuda", "xpu", "cpu")
  VLM_REFERENCE    optional directory of a finished run; its saved
                   <stem>.vlm.md files stand in for the endpoint, so the
                   gRParse legs of an accelerator matrix score against one
                   oracle output without regenerating it (VLM_ENDPOINT is
                   then only recorded, not called)
  GRPARSE_COLLECTORS  optional comma list of Collector enum names to force
                   (e.g. COLLECTOR_GRPARSE_CV to keep a PDF on the in-process
                   CV path instead of the inspector route), so the same
                   corpus times gRParse's own accelerator

Needs grpcio and grpcio-tools (uv run --with grpcio --with grpcio-tools).
"""

from __future__ import annotations

import base64
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SKIP = 77
DEFAULT_PROMPT = (
    "Convert this page to markdown. Keep the reading order, every heading, "
    "paragraph, list and table (tables as markdown tables), and transcribe the "
    "text exactly; do not summarize or describe."
)


def skip(reason: str) -> None:
    print(f"skip: {reason}", file=sys.stderr)
    sys.exit(SKIP)


def stage_protos(into: Path) -> None:
    """Lay the repo's protos out under their import paths."""
    layout = {
        "document.proto": "ai/pipestream/document/v1/document.proto",
        "parse.proto": "ai/pipestream/parse/v1/parse.proto",
        "parse_types.proto": "ai/pipestream/parse/v1/parse_types.proto",
        "parse_stream.proto": "ai/pipestream/parse/v1/parse_stream.proto",
    }
    for source, target in layout.items():
        destination = into / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / source, destination)


def load_stubs(staged: Path):
    import grpc_tools
    from grpc_tools import protoc

    include = Path(grpc_tools.__file__).parent / "_proto"
    out = staged / "gen"
    out.mkdir(exist_ok=True)
    args = [
        "protoc", f"-I{staged}", f"-I{include}", f"--python_out={out}",
        f"--grpc_python_out={out}",
        str(staged / "ai/pipestream/document/v1/document.proto"),
        str(staged / "ai/pipestream/parse/v1/parse_types.proto"),
        str(staged / "ai/pipestream/parse/v1/parse.proto"),
    ]
    if protoc.main(args) != 0:
        raise RuntimeError("protoc failed")
    sys.path.insert(0, str(out))
    from ai.pipestream.parse.v1 import parse_pb2, parse_pb2_grpc, parse_types_pb2

    return parse_pb2, parse_pb2_grpc, parse_types_pb2


def grparse_markdown(stub, parse_pb2, parse_types_pb2, pdf: Path) -> tuple[str, float, dict]:
    request = parse_pb2.ConvertSourceRequest()
    source = request.request.sources.add()
    source.file.filename = pdf.name
    source.file.base64_string = base64.b64encode(pdf.read_bytes()).decode()
    request.request.options.to_formats.append(parse_types_pb2.OUTPUT_FORMAT_MARKDOWN)
    for name in filter(None, os.environ.get("GRPARSE_COLLECTORS", "").split(",")):
        request.request.options.collectors.append(parse_types_pb2.Collector.Value(name.strip()))
    started = time.monotonic()
    response = stub.ConvertSource(request, timeout=3600)
    elapsed = time.monotonic() - started
    doc = response.response.document
    stats = {
        "texts": len(doc.doc.texts), "tables": len(doc.doc.tables),
        "pictures": len(doc.doc.pictures), "pages": len(doc.doc.pages),
        "status": parse_types_pb2.ConversionStatus.Name(response.response.status),
    }
    return doc.exports.md, elapsed, stats


def rasterize(pdf: Path, dpi: int, into: Path) -> list[Path]:
    prefix = into / pdf.stem
    subprocess.run(["pdftoppm", "-r", str(dpi), "-png", str(pdf), str(prefix)], check=True)
    return sorted(into.glob(f"{pdf.stem}-*.png"), key=lambda p: int(p.stem.rsplit("-", 1)[1]))


def vlm_markdown(endpoint: str, prompt: str, page: Path) -> tuple[str, dict]:
    payload = {
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url", "image_url": {
                    "url": "data:image/png;base64," + base64.b64encode(page.read_bytes()).decode()}},
            ],
        }],
        "max_tokens": 8192,
        "temperature": 0,
    }
    request = urllib.request.Request(
        endpoint.rstrip("/") + "/v1/chat/completions", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    started = time.monotonic()
    with urllib.request.urlopen(request, timeout=7200) as response:
        body = json.load(response)
    usage = body.get("usage", {})
    usage["wall_seconds"] = round(time.monotonic() - started, 3)
    return body["choices"][0]["message"]["content"], usage


def plain(text: str) -> str:
    """The words a reader sees: markup, link targets, entities and image
    placeholders are rendering, not content, and score neither side."""
    import html

    text = re.sub(r"<!--.*?-->", " ", text, flags=re.S)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", " ", text)
    text = re.sub(r"\]\([^)]*\)", "]", text)
    text = re.sub(r"</?[a-zA-Z][a-zA-Z0-9-]*(?:\s[^<>]*)?/?>", " ", text)
    return html.unescape(text).lower()


def letters(text: str) -> str:
    return re.sub(r"[^0-9a-z]+", "", plain(text))


def words(text: str) -> list[str]:
    return re.findall(r"[0-9a-z]+", plain(text))


def table_rows(text: str) -> int:
    """Rows of tables in either spelling: markdown pipes or HTML <tr>."""
    return len(re.findall(r"^\|", text, re.M)) + len(re.findall(r"<tr\b", text, re.I))


def score(reference: str, candidate: str) -> dict:
    """How alike two renderings of one document are, markup ignored."""
    a, b = letters(reference), letters(candidate)
    ratio = difflib.SequenceMatcher(None, a, b, autojunk=False).ratio() if a and b else 0.0
    wa, wb = words(reference), words(candidate)
    from collections import Counter

    ca, cb = Counter(wa), Counter(wb)
    overlap = sum((ca & cb).values())
    recall = overlap / len(wa) if wa else 0.0
    precision = overlap / len(wb) if wb else 0.0
    return {
        "letter_similarity": round(ratio, 4),
        "word_recall": round(recall, 4),
        "word_precision": round(precision, 4),
        "letters": [len(a), len(b)],
        "headings": [len(re.findall(r"^#+ ", reference, re.M)), len(re.findall(r"^#+ ", candidate, re.M))],
        "table_rows": [table_rows(reference), table_rows(candidate)],
    }


def corpus_files(spec: str) -> list[Path]:
    path = Path(spec)
    if path.is_dir():
        return sorted(path.glob("*.pdf"))
    return [Path(p) for p in spec.split(":") if p]


def write_report(out: Path, report: dict) -> None:
    (out / "report.json").write_text(json.dumps(report, indent=2))
    vlm_model = report.get("vlm", {})
    lines = [f"# VLM oracle comparison: {report['label']}", "",
             f"gRParse `{report['grparse_target']}` vs `{report['vlm_endpoint']}` "
             f"({vlm_model.get('device_name', vlm_model.get('status'))})", "",
             "| file | pages | gRParse s | VLM s | VLM tok/s | letter sim | word recall | word precision | headings g/v | table rows g/v |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for r in report["results"]:
        a = r["agreement"]
        lines.append(
            f"| {r['file']} | {r['pages']} | {r['grparse_seconds']} | {r['vlm_seconds']} | {r['vlm_tokens_per_second']} | "
            f"{a['letter_similarity']} | {a['word_recall']} | {a['word_precision']} | {a['headings'][0]}/{a['headings'][1]} | {a['table_rows'][0]}/{a['table_rows'][1]} |")
    (out / "report.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


def rescore(out: Path) -> int:
    """Re-derive the scores of a finished run from its saved markdowns."""
    report = json.loads((out / "report.json").read_text())
    for r in report["results"]:
        stem = Path(r["file"]).stem
        r["agreement"] = score((out / f"{stem}.grparse.md").read_text(), (out / f"{stem}.vlm.md").read_text())
    write_report(out, report)
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--rescore":
        return rescore(Path(sys.argv[2]))
    target = os.environ.get("GRPARSE_TARGET")
    endpoint = os.environ.get("VLM_ENDPOINT")
    corpus = os.environ.get("EVAL_CORPUS")
    if not (target and endpoint and corpus):
        skip("GRPARSE_TARGET, VLM_ENDPOINT and EVAL_CORPUS must all be set")
    if shutil.which("pdftoppm") is None:
        skip("pdftoppm is not installed")
    try:
        import grpc  # noqa: F401
        import grpc_tools  # noqa: F401
    except ImportError:
        skip("grpcio and grpcio-tools are not importable")
    import grpc

    files = corpus_files(corpus)
    if not files:
        skip(f"no PDFs in {corpus}")
    label = os.environ.get("EVAL_LABEL", "run")
    out = Path(os.environ.get("EVAL_OUT", REPO / "eval" / "out")) / label
    out.mkdir(parents=True, exist_ok=True)
    prompt = os.environ.get("VLM_PROMPT", DEFAULT_PROMPT)
    dpi = int(os.environ.get("EVAL_DPI", "150"))

    with tempfile.TemporaryDirectory() as staged:
        stage_protos(Path(staged))
        parse_pb2, parse_pb2_grpc, parse_types_pb2 = load_stubs(Path(staged))
        channel = grpc.insecure_channel(target, options=[("grpc.max_receive_message_length", 512 * 1024 * 1024)])
        stub = parse_pb2_grpc.ParseServiceStub(channel)
        vlm_model = None
        try:
            with urllib.request.urlopen(endpoint.rstrip("/") + "/health", timeout=10) as response:
                vlm_model = json.load(response)
        except Exception:  # noqa: BLE001
            vlm_model = {"status": "unknown"}
        results = []
        for pdf in files:
            print(f"== {pdf.name}", file=sys.stderr)
            reference, grparse_seconds, stats = grparse_markdown(stub, parse_pb2, parse_types_pb2, pdf)
            (out / f"{pdf.stem}.grparse.md").write_text(reference)
            reference_dir = os.environ.get("VLM_REFERENCE")
            with tempfile.TemporaryDirectory() as pages_dir:
                pages = rasterize(pdf, dpi, Path(pages_dir))
                page_texts, usages = [], []
                if reference_dir:
                    candidate = (Path(reference_dir) / f"{pdf.stem}.vlm.md").read_text()
                else:
                    for page in pages:
                        text, usage = vlm_markdown(endpoint, prompt, page)
                        page_texts.append(text)
                        usages.append(usage)
                        print(f"   page {page.stem.rsplit('-', 1)[1]}: {usage}", file=sys.stderr)
                    candidate = "\n\n".join(page_texts)
            (out / f"{pdf.stem}.vlm.md").write_text(candidate)
            vlm_seconds = sum(u.get("wall_seconds", 0.0) for u in usages)
            generated = sum(u.get("completion_tokens", 0) for u in usages)
            record = {
                "file": pdf.name, "pages": len(pages), "grparse": stats,
                "grparse_seconds": round(grparse_seconds, 3),
                "vlm_seconds": round(vlm_seconds, 3),
                "vlm_tokens": generated,
                "vlm_tokens_per_second": round(generated / vlm_seconds, 1) if vlm_seconds else None,
                "agreement": score(reference, candidate),
            }
            results.append(record)
            print(json.dumps(record), file=sys.stderr)
        report = {"label": label, "grparse_target": target, "vlm_endpoint": endpoint,
                  "vlm_reference": os.environ.get("VLM_REFERENCE", ""),
                  "grparse_collectors": os.environ.get("GRPARSE_COLLECTORS", ""),
                  "vlm": vlm_model, "prompt": prompt, "dpi": dpi, "results": results}
        write_report(out, report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
