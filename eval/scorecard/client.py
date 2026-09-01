"""Dial a running gRParse and convert one document at a time.

Reuses the proto staging and stub generation of ``eval/compare_vlm.py`` so
there is exactly one dial pattern in ``eval/``. The client returns the
Document as a plain dict (protobuf JSON mapping with proto field names) so
the summary and metric layers never import grpc or the generated stubs.
"""

from __future__ import annotations

import base64
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from compare_vlm import load_stubs, stage_protos  # noqa: E402

MAX_MESSAGE_BYTES = 512 * 1024 * 1024
CONVERT_TIMEOUT_SECONDS = 1800.0
HEALTH_TIMEOUT_SECONDS = 10.0


class Unreachable(RuntimeError):
    """gRParse did not answer; the caller turns this into the skip exit code."""


@dataclass
class ConvertResult:
    """What one ConvertSource call produced, already detached from protobuf."""

    document: dict[str, Any]
    markdown: str
    status: str
    errors: list[dict[str, str]]
    elapsed_ms: float
    timings: dict[str, float] = field(default_factory=dict)
    rpc_error: str | None = None
    # Every export the call asked for, by the OutputFormat name without its
    # prefix ("md", "canonical_json"); ``markdown`` stays the md shortcut.
    exports: dict[str, str] = field(default_factory=dict)


# The exports a plain convert asks for; callers that need more name them.
DEFAULT_FORMATS = ("MARKDOWN",)
EXPORT_FIELDS = {
    "TEXT": "text", "MARKDOWN": "md", "HTML": "html", "JSON": "json",
    "CANONICAL_JSON": "canonical_json", "GDOCS_JSON": "gdocs_json", "DOCTAGS": "doctags",
    "DOCLANG": "doclang", "VTT": "vtt", "HTML_SPLIT_PAGE": "html_split_page", "YAML": "yaml",
}


@dataclass
class ServiceInfo:
    name: str
    version: str
    target: str


class GrparseClient:
    """Context manager owning the staged protos, the channel and the stub."""

    def __init__(self, target: str) -> None:
        self.target = target
        self._staged: tempfile.TemporaryDirectory[str] | None = None
        self._channel = None
        self._stub = None
        self._parse_pb2 = None
        self._parse_types_pb2 = None

    def __enter__(self) -> "GrparseClient":
        import grpc

        self._staged = tempfile.TemporaryDirectory()
        staged = Path(self._staged.name)
        stage_protos(staged)
        parse_pb2, parse_pb2_grpc, parse_types_pb2 = load_stubs(staged)
        self._parse_pb2, self._parse_types_pb2 = parse_pb2, parse_types_pb2
        self._channel = grpc.insecure_channel(
            self.target, options=[("grpc.max_receive_message_length", MAX_MESSAGE_BYTES)])
        self._stub = parse_pb2_grpc.ParseServiceStub(self._channel)
        return self

    def __exit__(self, *exc: object) -> None:
        if self._channel is not None:
            self._channel.close()
        if self._staged is not None:
            self._staged.cleanup()

    def service_info(self) -> ServiceInfo:
        """GetServiceInfo, or Unreachable when the target does not answer."""
        import grpc

        try:
            info = self._stub.GetServiceInfo(self._parse_pb2.GetServiceInfoRequest(), timeout=HEALTH_TIMEOUT_SECONDS)
        except grpc.RpcError as error:
            raise Unreachable(f"{self.target}: {error.code().name}: {error.details()}") from error
        return ServiceInfo(name=info.name, version=info.version, target=self.target)

    def convert(self, path: Path, filename: str | None = None) -> ConvertResult:
        """One ConvertSource with markdown requested; RPC failures are returned, not raised."""
        return self.convert_bytes(path.read_bytes(), filename or path.name)

    def convert_bytes(self, data: bytes, filename: str, *, formats: tuple[str, ...] = DEFAULT_FORMATS,
                      collectors: tuple[str, ...] = (), ebcdic_layout_json: bytes | None = None,
                      timeout: float | None = None) -> ConvertResult:
        """One ConvertSource over in-memory bytes (nothing touches disk). ``formats``
        are OutputFormat names without the prefix, ``collectors`` Collector enum
        names without theirs; RPC failures are returned, not raised."""
        import grpc
        from google.protobuf.json_format import MessageToDict

        request = self._parse_pb2.ConvertSourceRequest()
        source = request.request.sources.add()
        source.file.filename = filename
        source.file.base64_string = base64.b64encode(data).decode()
        for name in formats:
            request.request.options.to_formats.append(
                self._parse_types_pb2.OutputFormat.Value(f"OUTPUT_FORMAT_{name}"))
        for name in collectors:
            request.request.options.collectors.append(
                self._parse_types_pb2.Collector.Value(f"COLLECTOR_{name}"))
        if ebcdic_layout_json is not None:
            request.request.options.ebcdic_layout_json = ebcdic_layout_json
        started = time.monotonic()
        try:
            response = self._stub.ConvertSource(request, timeout=timeout or CONVERT_TIMEOUT_SECONDS)
        except grpc.RpcError as error:
            elapsed = (time.monotonic() - started) * 1000.0
            if error.code() == grpc.StatusCode.UNAVAILABLE:
                raise Unreachable(f"{self.target}: {error.details()}") from error
            return ConvertResult(document={}, markdown="", status="RPC_ERROR", errors=[],
                                 elapsed_ms=elapsed, rpc_error=f"{error.code().name}: {error.details()}")
        elapsed = (time.monotonic() - started) * 1000.0
        body = response.response
        document = MessageToDict(body.document.doc, preserving_proto_field_name=True)
        errors = [{"module": e.module_name, "component": self._parse_types_pb2.ComponentType.Name(e.component_type),
                   "message": e.error_message} for e in body.errors]
        status = self._parse_types_pb2.ConversionStatus.Name(body.status)
        exports = {EXPORT_FIELDS[name]: getattr(body.document.exports, EXPORT_FIELDS[name]) or ""
                   for name in formats if name in EXPORT_FIELDS}
        return ConvertResult(document=document, markdown=body.document.exports.md or "", status=status,
                             errors=errors, elapsed_ms=elapsed, timings=dict(body.timings), exports=exports)
