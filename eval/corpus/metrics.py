"""Prometheus metrics collector, atomic .prom writer, and HTTP /metrics exporter
for local corpus evaluation runs."""

from __future__ import annotations

import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .journal import JournalRecord

DURATION_BUCKETS: tuple[float, ...] = (
    0.01,
    0.025,
    0.05,
    0.1,
    0.25,
    0.5,
    1.0,
    2.5,
    5.0,
    10.0,
    30.0,
    60.0,
    120.0,
    300.0,
)

VALID_STATUSES: frozenset[str] = frozenset({
    "success",
    "partial_success",
    "failure",
    "rpc_error",
    "skipped",
})


def normalize_status(raw_status: str | None, rpc_error: str | None = None, *, skipped: bool = False) -> str:
    """Map gRParse conversion status or RPC error to one of the five canonical statuses."""
    if skipped or raw_status == "skipped":
        return "skipped"
    if rpc_error:
        return "rpc_error"
    text = (raw_status or "").strip().lower()
    if text in VALID_STATUSES:
        return text
    if text.endswith("partial_success"):
        return "partial_success"
    if text.endswith("success"):
        return "success"
    if "rpc_error" in text:
        return "rpc_error"
    return "failure"


def _escape_label(value: str) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def _format_float(value: float) -> str:
    if value == float("inf"):
        return "+Inf"
    return f"{value:.6g}"


class MetricsCollector:
    """Thread-safe accumulator for corpus evaluation telemetry."""

    def __init__(self, run_label: str, *, started_monotonic: float | None = None) -> None:
        self.run_label = run_label
        self._started = time.monotonic() if started_monotonic is None else started_monotonic
        self._lock = threading.Lock()
        # (doctype, parser_type, status) -> count
        self._docs_total: dict[tuple[str, str, str], int] = {}
        # (doctype, check) -> count
        self._check_failures: dict[tuple[str, str], int] = {}
        # doctype -> list of bucket counts corresponding to DURATION_BUCKETS
        self._duration_buckets: dict[str, list[int]] = {}
        self._duration_count: dict[str, int] = {}
        self._duration_sum: dict[str, float] = {}
        self._completed_docs: int = 0
        self._discovered_docs: int = 0
        self._finished: bool = False

    def set_discovered(self, count: int) -> None:
        with self._lock:
            if count > self._discovered_docs:
                self._discovered_docs = count

    def add_discovered(self, delta: int = 1) -> None:
        with self._lock:
            self._discovered_docs += delta

    def mark_finished(self) -> None:
        with self._lock:
            self._finished = True
            if self._completed_docs > self._discovered_docs:
                self._discovered_docs = self._completed_docs

    def record_document(
        self,
        *,
        doctype: str,
        parser_type: str,
        status: str,
        elapsed_ms: float,
        failed_checks: list[str] | tuple[str, ...] = (),
    ) -> None:
        """Record one completed document into counters and duration histogram."""
        dtype = doctype or "unknown"
        ptype = parser_type or "none"
        norm_status = normalize_status(status, skipped=(status == "skipped"))
        duration_s = max(0.0, float(elapsed_ms) / 1000.0)

        with self._lock:
            self._completed_docs += 1
            if self._completed_docs > self._discovered_docs:
                self._discovered_docs = self._completed_docs

            doc_key = (dtype, ptype, norm_status)
            self._docs_total[doc_key] = self._docs_total.get(doc_key, 0) + 1

            for check_name in failed_checks:
                chk_key = (dtype, check_name)
                self._check_failures[chk_key] = self._check_failures.get(chk_key, 0) + 1

            if norm_status != "skipped":
                buckets = self._duration_buckets.get(dtype)
                if buckets is None:
                    buckets = [0] * len(DURATION_BUCKETS)
                    self._duration_buckets[dtype] = buckets
                for idx, bound in enumerate(DURATION_BUCKETS):
                    if duration_s <= bound:
                        buckets[idx] += 1
                self._duration_count[dtype] = self._duration_count.get(dtype, 0) + 1
                self._duration_sum[dtype] = self._duration_sum.get(dtype, 0.0) + duration_s

    def record_journal_entry(self, entry: JournalRecord) -> None:
        """Fold a results.jsonl record into the collector."""
        self.record_document(
            doctype=entry.doctype,
            parser_type=entry.parser_type,
            status=entry.status,
            elapsed_ms=entry.elapsed_ms,
            failed_checks=entry.failed_checks,
        )

    def render(self, *, now_monotonic: float | None = None) -> str:
        """Render all metrics in Prometheus text exposition format (0.0.4)."""
        now = time.monotonic() if now_monotonic is None else now_monotonic
        label = _escape_label(self.run_label)

        with self._lock:
            docs_total = sorted(self._docs_total.items())
            check_failures = sorted(self._check_failures.items())
            duration_types = sorted(self._duration_count.keys())
            duration_buckets = {k: list(v) for k, v in self._duration_buckets.items()}
            duration_count = dict(self._duration_count)
            duration_sum = dict(self._duration_sum)
            completed = self._completed_docs
            discovered = self._discovered_docs
            finished = self._finished

        elapsed_wall = max(1e-6, now - self._started)
        throughput = completed / elapsed_wall if completed > 0 else 0.0
        if finished:
            progress = 1.0
        elif discovered > 0:
            progress = min(1.0, completed / discovered)
        else:
            progress = 0.0

        lines: list[str] = [
            "# HELP grparse_eval_documents_total Total documents evaluated by status.",
            "# TYPE grparse_eval_documents_total counter",
        ]
        for (dtype, ptype, status), count in docs_total:
            lines.append(
                f'grparse_eval_documents_total{{run_label="{label}",'
                f'doctype="{_escape_label(dtype)}",'
                f'parser_type="{_escape_label(ptype)}",'
                f'status="{_escape_label(status)}"}} {count}'
            )

        lines.extend([
            "# HELP grparse_eval_check_failures_total Total shape check failures by doctype and check name.",
            "# TYPE grparse_eval_check_failures_total counter",
        ])
        for (dtype, check_name), count in check_failures:
            lines.append(
                f'grparse_eval_check_failures_total{{run_label="{label}",'
                f'doctype="{_escape_label(dtype)}",'
                f'check="{_escape_label(check_name)}"}} {count}'
            )

        lines.extend([
            "# HELP grparse_eval_duration_seconds Document conversion duration in seconds.",
            "# TYPE grparse_eval_duration_seconds histogram",
        ])
        for dtype in duration_types:
            dtype_esc = _escape_label(dtype)
            buckets = duration_buckets[dtype]
            total_count = duration_count[dtype]
            total_sum = duration_sum[dtype]
            for bound, b_count in zip(DURATION_BUCKETS, buckets):
                lines.append(
                    f'grparse_eval_duration_seconds_bucket{{run_label="{label}",'
                    f'doctype="{dtype_esc}",le="{_format_float(bound)}"}} {b_count}'
                )
            lines.append(
                f'grparse_eval_duration_seconds_bucket{{run_label="{label}",'
                f'doctype="{dtype_esc}",le="+Inf"}} {total_count}'
            )
            lines.append(
                f'grparse_eval_duration_seconds_sum{{run_label="{label}",'
                f'doctype="{dtype_esc}"}} {total_sum:.6f}'
            )
            lines.append(
                f'grparse_eval_duration_seconds_count{{run_label="{label}",'
                f'doctype="{dtype_esc}"}} {total_count}'
            )

        lines.extend([
            "# HELP grparse_eval_progress_ratio Ratio of completed documents to discovered documents.",
            "# TYPE grparse_eval_progress_ratio gauge",
            f'grparse_eval_progress_ratio{{run_label="{label}"}} {progress:.6f}',
            "# HELP grparse_eval_throughput_docs_per_second Evaluation throughput in documents per second.",
            "# TYPE grparse_eval_throughput_docs_per_second gauge",
            f'grparse_eval_throughput_docs_per_second{{run_label="{label}"}} {throughput:.6f}',
        ])
        return "\n".join(lines) + "\n"

    def write_prom_file(self, path: Path) -> Path:
        """Atomically write the Prometheus textfile snapshot to ``path``."""
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.parent / f".{path.name}.tmp.{os.getpid()}.{threading.get_ident()}"
        tmp.write_text(self.render(), encoding="utf-8")
        os.replace(tmp, path)
        return path


class _ReusableThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


class MetricsServer:
    """Lightweight background HTTP server exposing GET /metrics."""

    def __init__(self, collector: MetricsCollector, port: int = 0, host: str = "127.0.0.1") -> None:
        self.collector = collector
        self.host = host
        self._RequestedPort = port
        self._server: _ReusableThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> MetricsServer:
        collector = self.collector

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # noqa: N802
                if self.path.split("?", 1)[0] not in ("/metrics", "/"):
                    self.send_response(404)
                    self.end_headers()
                    return
                payload = collector.render().encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, format: str, *args: object) -> None:  # noqa: A002
                return

        self._server = _ReusableThreadingHTTPServer((self.host, self._RequestedPort), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, name="eval-metrics-http", daemon=True)
        self._thread.start()
        return self

    @property
    def port(self) -> int:
        if self._server is None:
            return self._RequestedPort
        return int(self._server.server_port)

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/metrics"

    def close(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None

    def __enter__(self) -> MetricsServer:
        return self.start()

    def __exit__(self, *exc: object) -> None:
        self.close()
