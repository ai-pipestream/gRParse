#!/usr/bin/env bash
# Validate the Prometheus + Grafana observability overlay, scrape config,
# provisioning files, and provisioned dashboards without starting containers.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"

if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
  rendered=$(docker compose -f compose.stack.yaml -f compose.stack.observability.yaml --profile observability config)
  grep -q 'prom/prometheus:v' <<<"$rendered" || {
    echo "observability-compose-test: prometheus image missing from rendered compose config" >&2
    exit 1
  }
  grep -q 'grafana/grafana:' <<<"$rendered" || {
    echo "observability-compose-test: grafana image missing from rendered compose config" >&2
    exit 1
  }
  grep -q 'GRPARSE_METRICS_PORT: "9464"' <<<"$rendered" || {
    echo "observability-compose-test: GRPARSE_METRICS_PORT not set to 9464 in rendered compose config" >&2
    exit 1
  }
  grep -q 'host.docker.internal' <<<"$rendered" || {
    echo "observability-compose-test: host.docker.internal extra_hosts missing from rendered compose config" >&2
    exit 1
  }
fi

python3 - "$root" <<'PY'
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])


def load_yaml_or_text(path: pathlib.Path) -> str:
    text = path.read_text(encoding="utf-8")
    try:
        import yaml  # type: ignore

        parsed = yaml.safe_load(text)
        assert isinstance(parsed, dict), f"{path} did not parse to a YAML mapping"
    except ImportError:
        pass
    return text


overlay_path = root / "compose.stack.observability.yaml"
assert overlay_path.is_file(), "compose.stack.observability.yaml is missing"
overlay_text = load_yaml_or_text(overlay_path)
for needle in (
    'GRPARSE_METRICS_PORT: "9464"',
    "prom/prometheus:v",
    "grafana/grafana:",
    "./compose/prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro",
    "./compose/grafana/provisioning:/etc/grafana/provisioning:ro",
    "./compose/grafana/dashboards:/var/lib/grafana/dashboards:ro",
    "host.docker.internal:host-gateway",
    "127.0.0.1:${GRAFANA_PORT:-3000}:3000",
    'GF_AUTH_ANONYMOUS_ENABLED: "true"',
    'GF_AUTH_ANONYMOUS_ORG_ROLE: "Admin"',
):
    assert needle in overlay_text, f"compose.stack.observability.yaml missing {needle!r}"

prom_path = root / "compose/prometheus/prometheus.yml"
assert prom_path.is_file(), "compose/prometheus/prometheus.yml is missing"
prom_text = load_yaml_or_text(prom_path)
for needle in (
    "scrape_interval: 5s",
    'job_name: "grparse"',
    "grparse:9464",
    'job_name: "grparse-corpus-eval"',
    "host.docker.internal:9465",
):
    assert needle in prom_text, f"prometheus.yml missing {needle!r}"

ds_path = root / "compose/grafana/provisioning/datasources/prometheus.yaml"
assert ds_path.is_file(), "datasources/prometheus.yaml is missing"
ds_text = load_yaml_or_text(ds_path)
for needle in (
    "uid: prometheus",
    "url: http://prometheus:9090",
    "isDefault: true",
):
    assert needle in ds_text, f"datasources/prometheus.yaml missing {needle!r}"

prov_path = root / "compose/grafana/provisioning/dashboards/grparse.yaml"
assert prov_path.is_file(), "dashboards/grparse.yaml is missing"
prov_text = load_yaml_or_text(prov_path)
assert "/var/lib/grafana/dashboards" in prov_text, "dashboards/grparse.yaml missing dashboard path"


def validate_dashboard(
    path: pathlib.Path,
    expected_title: str,
    expected_metrics: list[str],
    expected_variables: list[tuple[str, str]],
) -> None:
    assert path.is_file(), f"dashboard missing: {path}"
    raw = path.read_text(encoding="utf-8")
    data = json.loads(raw)
    assert data.get("title") == expected_title, (
        f"{path.name}: expected title {expected_title!r}, got {data.get('title')!r}"
    )
    panels = data.get("panels", [])
    assert panels, f"{path.name}: dashboard has no panels"

    panel_ids: set[int] = set()
    exprs: list[str] = []

    def walk_panels(panel_list: list[dict]) -> None:
        for panel in panel_list:
            pid = panel.get("id")
            assert isinstance(pid, int), f"{path.name}: panel missing integer id: {panel}"
            assert pid not in panel_ids, f"{path.name}: duplicate panel id {pid}"
            panel_ids.add(pid)
            ptype = panel.get("type")
            if ptype != "row":
                ds = panel.get("datasource", {})
                assert ds.get("uid") == "prometheus", (
                    f"{path.name}: panel {pid} datasource uid is {ds.get('uid')!r}, expected 'prometheus'"
                )
                targets = panel.get("targets", [])
                assert targets, f"{path.name}: non-row panel {pid} has no targets"
                for target in targets:
                    tds = target.get("datasource", {})
                    assert tds.get("uid") == "prometheus", (
                        f"{path.name}: panel {pid} target datasource uid is {tds.get('uid')!r}"
                    )
                    expr = target.get("expr", "")
                    assert expr, f"{path.name}: panel {pid} has empty expr"
                    exprs.append(expr)
            if "panels" in panel and isinstance(panel["panels"], list):
                walk_panels(panel["panels"])

    walk_panels(panels)
    joined_exprs = "\n".join(exprs)
    for metric in expected_metrics:
        assert re.search(r"\b" + re.escape(metric) + r"\b", joined_exprs), (
            f"{path.name}: metric {metric!r} not referenced in any panel target expr"
        )

    templating_list = data.get("templating", {}).get("list", [])
    vars_by_name = {v.get("name"): v for v in templating_list}
    for var_name, expected_query in expected_variables:
        assert var_name in vars_by_name, f"{path.name}: missing template variable {var_name!r}"
        var_obj = vars_by_name[var_name]
        assert var_obj.get("datasource", {}).get("uid") == "prometheus", (
            f"{path.name}: template variable {var_name!r} must use datasource uid 'prometheus'"
        )
        q = var_obj.get("query")
        q_str = q.get("query", "") if isinstance(q, dict) else str(q or "")
        defn = var_obj.get("definition", "")
        assert expected_query in q_str or expected_query in defn, (
            f"{path.name}: variable {var_name!r} query {q_str!r} / {defn!r} missing {expected_query!r}"
        )


validate_dashboard(
    root / "compose/grafana/dashboards/grparse-pipeline-overview.json",
    "gRParse Pipeline & Collector Telemetry",
    [
        "grparse_documents_submitted_total",
        "grparse_documents_queued",
        "grparse_documents_rejected_total",
        "grparse_documents_completed_total",
        "grparse_document_duration_seconds_bucket",
        "grparse_collector_calls_total",
        "grparse_collector_duration_seconds_bucket",
        "grparse_collector_warnings_total",
        "grparse_pages_waiting",
        "grparse_stage_busy_seconds_total",
        "grparse_stage_workers",
        "grparse_page_latency_seconds_bucket",
        "grparse_repair_changes_total",
        "grparse_data_changes_total",
        "grparse_office_cv_total",
        "grparse_ort_ep_fallbacks_total",
        "process_resident_memory_bytes",
        "process_cpu_seconds_total",
    ],
    [],
)

validate_dashboard(
    root / "compose/grafana/dashboards/grparse-corpus-eval.json",
    "gRParse Corpus Evaluation & Drift Tracker",
    [
        "grparse_eval_progress_ratio",
        "grparse_eval_throughput_docs_per_second",
        "grparse_eval_documents_total",
        "grparse_eval_check_failures_total",
        "grparse_eval_duration_seconds_bucket",
    ],
    [("run_label", "label_values(grparse_eval_documents_total, run_label)")],
)
PY

echo "observability-compose-test: OK"
