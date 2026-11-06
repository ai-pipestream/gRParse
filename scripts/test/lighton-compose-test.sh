#!/usr/bin/env bash
# Validate the optional LightOn deployment without starting the shared stack.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
unset LIGHTON_MODEL LIGHTON_MODELS_DIR STACK_TAG
compose=(docker compose --project-directory "$root" -f "$root/compose.stack.yaml")
"${compose[@]}" --profile heavy config --services > "$work/heavy"
if grep -qx lighton-ocr "$work/heavy"; then
  echo 'heavy must not require the unavailable LightOn image' >&2
  exit 1
fi
"${compose[@]}" --profile lighton config --format json > "$work/base.json"
LIGHTON_MODEL=4b LIGHTON_MODELS_DIR="$work/models" STACK_TAG=0.3.0 \
  "${compose[@]}" --profile lighton config --format json > "$work/custom.json"
"${compose[@]}" -f "$root/compose.stack.standalone.yaml" --profile lighton \
  config --format json > "$work/standalone.json"
python3 - "$root" "$work" <<'PY'
import json, pathlib, sys
root, work = map(pathlib.Path, sys.argv[1:])
configs = {name: json.loads((work / (name + '.json')).read_text())
           for name in ('base', 'custom', 'standalone')}
for config in configs.values():
    service = config['services']['lighton-ocr']
    assert service['profiles'] == ['lighton']
    assert service['read_only'] is True
    assert service['cap_drop'] == ['ALL']
    assert 'no-new-privileges:true' in service['security_opt']
    assert not service.get('ports'), 'do not expose an unauthenticated host port'
    assert service['expose'] == ['50074']
    mount, = service['volumes']
    assert mount['target'] == '/models' and mount['read_only'] is True
    assert mount.get('bind', {}).get('create_host_path', False) is False
    assert 'GRPARSE_LIGHTON_OCR_TARGET' not in config['services']['grparse']['environment']
base = configs['base']['services']['lighton-ocr']
assert base['environment']['GRPC_LIGHTON_MODEL'] == '0.8b'
assert base['image'] == 'pipestreamai/grpc-lighton-ocr:latest'
custom = configs['custom']['services']['lighton-ocr']
assert custom['environment']['GRPC_LIGHTON_MODEL'] == '4b'
assert custom['image'] == 'pipestreamai/grpc-lighton-ocr:0.3.0'
assert custom['volumes'][0]['source'] == str(work / 'models')
standalone = configs['standalone']['services']['lighton-ocr']
assert standalone['volumes'][0]['source'] == str(root / 'protos/grpc-lighton-ocr/models')
print('lighton-compose-test: three compose configurations passed')
PY
