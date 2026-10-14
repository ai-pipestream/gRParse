"""Tests for the Prometheus exporter, HTTP /metrics endpoint, and .prom snapshot."""

from __future__ import annotations

import tempfile
import urllib.error
import urllib.request
from pathlib import Path

from corpus.metrics import MetricsCollector, MetricsServer, normalize_status


def test_normalize_status_variants() -> None:
    assert normalize_status("CONVERSION_STATUS_SUCCESS") == "success"
    assert normalize_status("CONVERSION_STATUS_PARTIAL_SUCCESS") == "partial_success"
    assert normalize_status("CONVERSION_STATUS_FAILURE") == "failure"
    assert normalize_status("RPC_ERROR", "DEADLINE_EXCEEDED: timeout") == "rpc_error"
    assert normalize_status("skipped", skipped=True) == "skipped"


def test_metrics_collector_renders_all_required_series() -> None:
    collector = MetricsCollector("batch-42", started_monotonic=100.0)
    collector.set_discovered(4)
    collector.record_document(
        doctype="pdf",
        parser_type="pdf",
        status="CONVERSION_STATUS_SUCCESS",
        elapsed_ms=45.0,
        failed_checks=[],
    )
    collector.record_document(
        doctype="docx",
        parser_type="libreoffice",
        status="CONVERSION_STATUS_SUCCESS",
        elapsed_ms=120.0,
        failed_checks=["integrity", "placement"],
    )
    collector.record_document(
        doctype="ebc",
        parser_type="none",
        status="skipped",
        elapsed_ms=0.0,
        failed_checks=[],
    )

    text = collector.render(now_monotonic=101.5)
    assert 'grparse_eval_documents_total{run_label="batch-42",doctype="pdf",parser_type="pdf",status="success"} 1' in text
    assert 'grparse_eval_documents_total{run_label="batch-42",doctype="docx",parser_type="libreoffice",status="success"} 1' in text
    assert 'grparse_eval_documents_total{run_label="batch-42",doctype="ebc",parser_type="none",status="skipped"} 1' in text
    assert 'grparse_eval_check_failures_total{run_label="batch-42",doctype="docx",check="integrity"} 1' in text
    assert 'grparse_eval_check_failures_total{run_label="batch-42",doctype="docx",check="placement"} 1' in text
    assert 'grparse_eval_duration_seconds_bucket{run_label="batch-42",doctype="pdf",le="0.05"} 1' in text
    assert 'grparse_eval_duration_seconds_bucket{run_label="batch-42",doctype="pdf",le="+Inf"} 1' in text
    assert 'grparse_eval_duration_seconds_count{run_label="batch-42",doctype="pdf"} 1' in text
    assert 'grparse_eval_progress_ratio{run_label="batch-42"} 0.750000' in text
    assert 'grparse_eval_throughput_docs_per_second{run_label="batch-42"} 2.000000' in text


def test_metrics_http_server_and_prom_snapshot() -> None:
    collector = MetricsCollector("http-test", started_monotonic=10.0)
    collector.record_document(
        doctype="png",
        parser_type="grparse-cv",
        status="success",
        elapsed_ms=250.0,
        failed_checks=[],
    )
    collector.mark_finished()

    with tempfile.TemporaryDirectory() as tmp:
        prom_path = Path(tmp) / "metrics.prom"
        collector.write_prom_file(prom_path)
        file_body = prom_path.read_text(encoding="utf-8")
        assert 'grparse_eval_progress_ratio{run_label="http-test"} 1.000000' in file_body

    with MetricsServer(collector, port=0) as server:
        assert server.port > 0
        with urllib.request.urlopen(server.url, timeout=5.0) as resp:
            assert resp.status == 200
            body = resp.read().decode("utf-8")
        assert 'grparse_eval_documents_total{run_label="http-test",doctype="png",parser_type="grparse-cv",status="success"} 1' in body

        try:
            urllib.request.urlopen(f"http://127.0.0.1:{server.port}/not-found", timeout=5.0)
            raise AssertionError("expected 404")
        except urllib.error.HTTPError as err:
            assert err.code == 404
