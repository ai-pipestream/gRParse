"""Unit tests for the memory line: endpoint resolution with the docker call injected, parsing, notes."""

from __future__ import annotations

import os
import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.memory import (  # noqa: E402
    CONTAINER,
    NOT_SAMPLED,
    docker_container_ip,
    memory_note,
    metrics_url,
    parse_rss,
    sample_rss,
    target_host,
)


def _no_docker(_: str) -> str | None:
    raise AssertionError("docker must not be consulted")


def test_explicit_url_wins_without_docker() -> None:
    assert metrics_url("localhost:50051", explicit="http://x:1/metrics", container_ip=_no_docker) == "http://x:1/metrics"
    assert metrics_url("remote:50051", explicit="http://x:1/metrics", container_ip=_no_docker) == "http://x:1/metrics"


def test_environment_url_is_the_default_explicit() -> None:
    previous = os.environ.get("EVAL_METRICS_URL")
    os.environ["EVAL_METRICS_URL"] = "http://env:9/metrics"
    try:
        assert metrics_url("localhost:50051", container_ip=_no_docker) == "http://env:9/metrics"
    finally:
        if previous is None:
            del os.environ["EVAL_METRICS_URL"]
        else:
            os.environ["EVAL_METRICS_URL"] = previous


def test_local_target_resolves_the_stack_container() -> None:
    asked: list[str] = []

    def inspect(container: str) -> str | None:
        asked.append(container)
        return "172.26.0.18"

    for target in ("localhost:50051", "127.0.0.1:41234", "0.0.0.0:50051", ":50051"):
        assert metrics_url(target, explicit="", container_ip=inspect) == "http://172.26.0.18:9464/metrics", target
    assert asked == [CONTAINER] * 4


def test_remote_target_is_never_probed() -> None:
    assert metrics_url("grparse:50051", explicit="", container_ip=_no_docker) is None
    assert metrics_url("10.0.0.5:50051", explicit="", container_ip=_no_docker) is None


def test_docker_unavailable_means_not_sampled() -> None:
    assert metrics_url("localhost:50051", explicit="", container_ip=lambda _: None) is None
    assert sample_rss(None) == (None, NOT_SAMPLED)
    assert memory_note((None, NOT_SAMPLED), (None, "")) == "memory: " + NOT_SAMPLED


def test_real_docker_call_never_raises() -> None:
    # Whatever the host has (docker or not, container or not), the resolver
    # answers with a string or None instead of an exception.
    ip = docker_container_ip("grparse-container-that-does-not-exist-" + str(os.getpid()))
    assert ip is None


def test_target_host_forms() -> None:
    assert target_host("localhost:50051") == "localhost"
    assert target_host("127.0.0.1:1") == "127.0.0.1"
    assert target_host("[::1]:50051") == "::1"
    assert target_host("host-without-port") == "host-without-port"


def test_parse_rss_reads_the_gauge() -> None:
    text = "# HELP process_resident_memory_bytes Resident memory size in bytes.\n# TYPE process_resident_memory_bytes gauge\nprocess_resident_memory_bytes 1.5e+09\n"
    assert parse_rss(text) == 1_500_000_000
    assert parse_rss("other_metric 3\n") is None
    assert parse_rss("process_resident_memory_bytes nan-ish\n") is None


def test_memory_note_formats_delta() -> None:
    note = memory_note((512 * 2**20, "u"), (640 * 2**20, "u"))
    assert note == "memory: rss 512 MiB before, 640 MiB after (delta +128 MiB), informational"
    assert memory_note((512 * 2**20, "u"), (None, "metrics endpoint u unreachable: x")) == \
        "memory: rss 512 MiB before the run; metrics endpoint u unreachable: x"
