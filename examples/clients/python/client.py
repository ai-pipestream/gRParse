#!/usr/bin/env python3
"""Streams one document to gRParse and prints each page event as it lands.

Run generate.sh first; see README.md.
"""
import os
import pathlib
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen"))

import grpc  # noqa: E402
from ai.pipestream.parse.v1 import parse_stream_pb2, parse_stream_pb2_grpc  # noqa: E402

CHUNK_BYTES = 1024 * 1024
CONTENT_TYPES = {
    ".pdf": "application/pdf",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".tif": "image/tiff",
    ".tiff": "image/tiff",
}


def chunk_stream(path: pathlib.Path):
    name = path.name
    content_type = CONTENT_TYPES.get(path.suffix.lower(), "")
    meta = dict(document_id=name, filename=name, content_type=content_type)
    with path.open("rb") as source:
        while data := source.read(CHUNK_BYTES):
            yield parse_stream_pb2.DocumentChunk(**meta, data=data)
    yield parse_stream_pb2.DocumentChunk(**meta, complete=True)


def describe_page(page) -> str:
    digital = sum(1 for o in page.text_offsets if o.source == parse_stream_pb2.TEXT_SOURCE_DIGITAL_PDF)
    ocr = sum(1 for o in page.text_offsets if o.source == parse_stream_pb2.TEXT_SOURCE_OCR)
    barcodes = sum(
        1
        for picture in page.pictures
        for annotation in picture.annotations
        if annotation.HasField("misc") and annotation.misc.kind == "barcode"
    )
    return (
        f"page={page.page_number} text_items={len(page.texts)} digital={digital} ocr={ocr} "
        f"tables={len(page.tables)} pictures={len(page.pictures)} barcodes={barcodes}"
    )


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"Usage: {sys.argv[0]} DOCUMENT_PATH [HOST:PORT]", file=sys.stderr)
        return 64
    document = pathlib.Path(sys.argv[1])
    target = sys.argv[2] if len(sys.argv) == 3 else "localhost:50051"

    pages = 0
    with grpc.insecure_channel(
        target, options=[("grpc.max_receive_message_length", 128 * 1024 * 1024)]
    ) as channel:
        stub = parse_stream_pb2_grpc.ParseStreamingServiceStub(channel)
        try:
            for event in stub.StreamProcessDocument(chunk_stream(document), timeout=600):
                which = event.WhichOneof("event")
                if which == "page":
                    pages += 1
                    print(describe_page(event.page))
                elif which == "complete":
                    print(f"complete total_pages={event.total_pages}")
                    for failure in event.complete.collector_failures:
                        print(f"collector_failure: {failure.error}", file=sys.stderr)
                elif which == "collector_document":
                    doc = event.collector_document.document
                    print(
                        f"collector_document texts={len(doc.texts)} "
                        f"tables={len(doc.tables)} pictures={len(doc.pictures)}"
                    )
        except grpc.RpcError as error:
            print(f"StreamProcessDocument failed: {error.details()}", file=sys.stderr)
            return 1
    return 0 if pages > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
