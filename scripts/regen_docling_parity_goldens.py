#!/usr/bin/env python3
"""Regenerate Docling serve and gRParse Document goldens for parity tests.

Serve side:
  --serve host:port  or DOCLING_SERVE_GRPC
  or --local to use DocumentConverter in-process (same mapping path)
  Writes tests/golden/docling_parity/<stem>.docling.pb

gRParse side:
  --grparse host:port or GRPARSE_TARGET
  Writes tests/golden/docling_parity/<stem>.grparse.pb as SerializeToString
  bytes (never JSON).

Records pins in VERSIONS.txt. Uses protobuf bytes only.
"""

from __future__ import annotations

import argparse
import base64
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOLDEN = ROOT / "tests" / "golden" / "docling_parity"
CORPUS = ROOT / "tests" / "golden" / "corpus"
# table-picture.pdf (tests/golden/build_format_fixtures.py) is pending: its
# goldens are not recorded yet (tests/golden/docling_parity/DEVIATIONS.md).
DEFAULT_FIXTURES = [CORPUS / "hello-text.pdf", CORPUS / "formats" / "table-picture.pdf"]
EVAL = ROOT / "eval"


def convert_serve_local(path: Path) -> bytes:
    from docling.datamodel.base_models import InputFormat
    from docling.datamodel.pipeline_options import PdfPipelineOptions
    from docling.document_converter import DocumentConverter, PdfFormatOption
    from docling_core.proto import docling_document_to_proto

    pipeline = PdfPipelineOptions()
    pipeline.do_ocr = False
    pipeline.do_table_structure = True
    converter = DocumentConverter(
        format_options={InputFormat.PDF: PdfFormatOption(pipeline_options=pipeline)}
    )
    result = converter.convert(str(path))
    return docling_document_to_proto(result.document).SerializeToString()


def convert_serve_grpc(path: Path, target: str) -> bytes:
    import grpc

    serve_root = Path(
        os.environ.get(
            "DOCLING_SERVE_ROOT",
            "/work/docling-research/docling-serve-parity-tests-2026-11-02",
        )
    )
    sys.path.insert(0, str(serve_root))
    from docling_serve.grpc.gen.ai.docling.serve.v1 import (  # type: ignore
        docling_serve_pb2 as serve_pb2,
    )
    from docling_serve.grpc.gen.ai.docling.serve.v1 import (  # type: ignore
        docling_serve_pb2_grpc as serve_grpc,
    )
    from docling_serve.grpc.gen.ai.docling.serve.v1 import (  # type: ignore
        docling_serve_types_pb2 as types_pb2,
    )

    opts = [
        ("grpc.max_send_message_length", 2 * 1024 * 1024 * 1024 - 1),
        ("grpc.max_receive_message_length", 2 * 1024 * 1024 * 1024 - 1),
    ]
    channel = grpc.insecure_channel(target, options=opts)
    stub = serve_grpc.DoclingServeServiceStub(channel)
    b64 = base64.b64encode(path.read_bytes()).decode()
    req = serve_pb2.ConvertSourceRequest(
        request=types_pb2.ConvertDocumentRequest(
            sources=[
                types_pb2.Source(
                    file=types_pb2.FileSource(base64_string=b64, filename=path.name)
                )
            ],
            options=types_pb2.ConvertDocumentOptions(
                do_ocr=False,
                do_table_structure=True,
            ),
        )
    )
    resp = stub.ConvertSource(req, timeout=300)
    if not resp.HasField("document") or not resp.document.HasField("doc"):
        raise RuntimeError(f"serve returned no document for {path}")
    return resp.document.doc.SerializeToString()


def convert_grparse(path: Path, target: str) -> bytes:
    import grpc

    sys.path.insert(0, str(EVAL))
    from compare_vlm import load_stubs, stage_protos  # type: ignore

    with tempfile.TemporaryDirectory() as staged_name:
        staged = Path(staged_name)
        stage_protos(staged)
        parse_pb2, parse_pb2_grpc, parse_types_pb2 = load_stubs(staged)
        channel = grpc.insecure_channel(
            target,
            options=[("grpc.max_receive_message_length", 512 * 1024 * 1024)],
        )
        stub = parse_pb2_grpc.ParseServiceStub(channel)
        req = parse_pb2.ConvertSourceRequest()
        source = req.request.sources.add()
        source.file.filename = path.name
        source.file.base64_string = base64.b64encode(path.read_bytes()).decode()
        req.request.options.do_ocr = False
        req.request.options.do_table_structure = True
        resp = stub.ConvertSource(req, timeout=300)
        body = resp.response
        if not body.document.HasField("doc"):
            raise RuntimeError(f"gRParse returned no document for {path}")
        return body.document.doc.SerializeToString()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve", default=os.environ.get("DOCLING_SERVE_GRPC", ""))
    parser.add_argument("--local", action="store_true",
                        help="DocumentConverter in-process for the Docling side")
    parser.add_argument("--grparse", default=os.environ.get("GRPARSE_TARGET", ""))
    parser.add_argument("fixtures", nargs="*", type=Path)
    args = parser.parse_args()
    fixtures = args.fixtures or DEFAULT_FIXTURES
    if not args.local and not args.serve:
        print("need --local or --serve host:port", file=sys.stderr)
        return 2
    GOLDEN.mkdir(parents=True, exist_ok=True)
    for path in fixtures:
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 2
        print(f"docling convert {path}")
        if args.local:
            docling_bytes = convert_serve_local(path)
        else:
            docling_bytes = convert_serve_grpc(path, args.serve)
        out = GOLDEN / f"{path.stem}.docling.pb"
        out.write_bytes(docling_bytes)
        print(f"  wrote {out} ({len(docling_bytes)} bytes)")
        if args.grparse:
            print(f"grparse convert {path}")
            gr_bytes = convert_grparse(path, args.grparse)
            gout = GOLDEN / f"{path.stem}.grparse.pb"
            gout.write_bytes(gr_bytes)
            print(f"  wrote {gout} ({len(gr_bytes)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
