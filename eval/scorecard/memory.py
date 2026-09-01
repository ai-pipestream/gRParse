"""Informational RSS sampling from the service's Prometheus endpoint.

Never a gate: the report gets one line with the resident set size before and
after the run when ``process_resident_memory_bytes`` is exposed, and a line
saying memory was not sampled when it is not. ``EVAL_METRICS_URL`` names the
endpoint explicitly; without it, a local target (``localhost``,
``127.0.0.1``, ``0.0.0.0`` or no host) resolves to the stack's
``parse-stack-grparse-1`` container through a read-only ``docker inspect``,
and any failure there (no docker, no container, no network) silently means
"not sampled". A remote target without an explicit URL is never probed.
"""

from __future__ import annotations

import os
import subprocess
import urllib.request
from typing import Callable

RSS_METRIC = "process_resident_memory_bytes"
CONTAINER = "parse-stack-grparse-1"
METRICS_PORT = 9464
TIMEOUT_SECONDS = 3.0
LOCAL_HOSTS = ("localhost", "127.0.0.1", "0.0.0.0", "")
NOT_SAMPLED = "not sampled (no metrics endpoint; set EVAL_METRICS_URL)"

# Resolves a container's IP on its first network, or None. Injected into
# metrics_url so the policy is testable without docker.
ContainerIp = Callable[[str], str | None]


def parse_rss(text: str) -> int | None:
    """Value of process_resident_memory_bytes in Prometheus text, or None."""
    for line in text.splitlines():
        if line.startswith(RSS_METRIC) and not line.startswith("#"):
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(float(parts[-1]))
                except ValueError:
                    return None
    return None


def docker_container_ip(container: str) -> str | None:
    """The container's IP from ``docker inspect``; None when docker is absent,
    fails, times out, or the container has no network address."""
    try:
        completed = subprocess.run(
            ["docker", "inspect", container, "--format",
             "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}"],
            capture_output=True, text=True, timeout=5, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    ip = completed.stdout.strip()
    return ip or None


def target_host(target: str) -> str:
    """The host part of ``host:port`` (``[v6]:port`` keeps its brackets' content)."""
    if target.startswith("["):
        return target[1:target.find("]")] if "]" in target else target
    return target.rsplit(":", 1)[0] if ":" in target else target


def metrics_url(target: str, *, explicit: str | None = None,
                container_ip: ContainerIp = docker_container_ip) -> str | None:
    """The Prometheus endpoint for ``target``: ``explicit`` (defaults to
    ``EVAL_METRICS_URL``) when given, else the stack container's ``:9464``
    for a local target, else None."""
    explicit = os.environ.get("EVAL_METRICS_URL") if explicit is None else explicit
    if explicit:
        return explicit
    if target_host(target) not in LOCAL_HOSTS:
        return None
    ip = container_ip(CONTAINER)
    return f"http://{ip}:{METRICS_PORT}/metrics" if ip else None


def sample_rss(url: str | None) -> tuple[int | None, str]:
    """(bytes or None, note) from one scrape."""
    if not url:
        return None, NOT_SAMPLED
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT_SECONDS) as response:  # noqa: S310
            text = response.read().decode("utf-8", errors="replace")
    except OSError as error:
        return None, f"metrics endpoint {url} unreachable: {error}"
    rss = parse_rss(text)
    if rss is None:
        return None, f"metrics endpoint {url} exposes no {RSS_METRIC}"
    return rss, url


def memory_note(before: tuple[int | None, str], after: tuple[int | None, str]) -> str:
    if before[0] is None:
        return f"memory: {before[1]}"
    if after[0] is None:
        return f"memory: rss {before[0] / 2**20:.0f} MiB before the run; {after[1]}"
    return (f"memory: rss {before[0] / 2**20:.0f} MiB before, {after[0] / 2**20:.0f} MiB after "
            f"(delta {(after[0] - before[0]) / 2**20:+.0f} MiB), informational")
