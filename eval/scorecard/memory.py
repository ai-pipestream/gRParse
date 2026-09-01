"""Informational RSS sampling from the service's Prometheus endpoint.

Never a gate: the report gets one line with the resident set size before and
after the run when ``process_resident_memory_bytes`` is exposed, and a line
saying it is not when it is not. ``EVAL_METRICS_URL`` names the endpoint;
without it the address is derived from the ``parse-stack-grparse-1``
container (read-only ``docker inspect``) when the target is local.
"""

from __future__ import annotations

import os
import subprocess
import urllib.request

RSS_METRIC = "process_resident_memory_bytes"
CONTAINER = "parse-stack-grparse-1"
METRICS_PORT = 9464
TIMEOUT_SECONDS = 3.0


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


def metrics_url(target: str) -> str | None:
    explicit = os.environ.get("EVAL_METRICS_URL")
    if explicit:
        return explicit
    host = target.rsplit(":", 1)[0]
    if host not in ("localhost", "127.0.0.1", "0.0.0.0", ""):
        return None
    try:
        ip = subprocess.run(["docker", "inspect", CONTAINER, "--format",
                             "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}"],
                            capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return f"http://{ip}:{METRICS_PORT}/metrics" if ip else None


def sample_rss(url: str | None) -> tuple[int | None, str]:
    """(bytes or None, note) from one scrape."""
    if not url:
        return None, "no metrics endpoint (set EVAL_METRICS_URL)"
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
