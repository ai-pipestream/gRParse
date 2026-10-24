#!/usr/bin/env bash
# End to end receipt for Docling chart2summary and chart2code: starts an
# emulated OpenAI-compatible VLM and a real grpc-enrich container on a
# private docker network, then runs chart-outputs-e2e-test from the build
# directory, or from an image built with
# `docker build -f Dockerfile.cpu --target e2e`, against them.
#
# usage: scripts/test/chart-outputs-e2e-test.sh <build-dir|image> <grpc-enrich-image>
set -euo pipefail
build=${1:?build dir or image}
image=${2:?grpc-enrich image}
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/test/e2e-binary.sh
. "$here/e2e-binary.sh"
net=grparse-chart-e2e-$$
work=$(mktemp -d)
cleanup() {
  docker logs grparse-chart-e2e-vlm-$$ > "$work/vlm.log" 2>&1 || true
  echo "== fake VLM requests"; cat "$work/vlm.log" || true
  docker rm -f grparse-chart-e2e-vlm-$$ grparse-chart-e2e-enrich-$$ >/dev/null 2>&1 || true
  docker network rm "$net" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT
python3 "$here/chart-outputs-e2e/make_chart_png.py" "$work/chart.png"
docker network create "$net" >/dev/null
docker run -d --name grparse-chart-e2e-vlm-$$ --network "$net" --network-alias vlm \
  -v "$here/chart-outputs-e2e:/app:ro" python:3.12-slim python /app/fake_vlm.py 8000 >/dev/null
docker run -d --name grparse-chart-e2e-enrich-$$ --network "$net" -p 127.0.0.1::50056 \
  -e ENRICH_VLM_URL=http://vlm:8000 "$image" >/dev/null
port=$(docker port grparse-chart-e2e-enrich-$$ 50056/tcp | head -1 | sed 's/.*://')
echo "== grpc-enrich $image on 127.0.0.1:$port"
up=
for _ in $(seq 1 60); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then up=1; break; fi
  sleep 1
done
if [ -z "$up" ]; then
  echo "grpc-enrich never opened 127.0.0.1:$port" >&2
  docker logs grparse-chart-e2e-enrich-$$ >&2 || true
  exit 1
fi
sleep 2
echo "== chart-outputs-e2e-test"
run_e2e_binary "$build" "$work" chart-outputs-e2e-test \
  GRPARSE_E2E_ENRICH_TARGET="127.0.0.1:$port" GRPARSE_E2E_CHART_PNG="$work/chart.png"
