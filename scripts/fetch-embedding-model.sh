#!/usr/bin/env bash
# Fetch pinned artifacts using bash, curl and coreutils. --verify is offline.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dir=$repo_root/models/embeddings/all-MiniLM-L6-v2
manifest=$repo_root/models/embeddings/MANIFEST
backend=all
verify=0

usage() {
  echo 'Usage: fetch-embedding-model.sh [--verify] [--backend onnx|openvino|all] [--dir DIR] [--manifest FILE]'
}
invalid() { echo "$*" >&2; exit 64; }
while (($#)); do
  case $1 in
    --verify) verify=1; shift ;;
    --backend|--dir|--manifest)
      (($# >= 2)) || invalid "missing value for $1"
      [[ -n $2 ]] || invalid "empty value for $1"
      case $1 in
        --backend) backend=$2 ;;
        --dir) dir=$2 ;;
        --manifest) manifest=$2 ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) invalid 'unknown argument' ;;
  esac
done
case $backend in onnx|openvino|all) ;; *) invalid 'invalid backend' ;; esac
[[ -r $manifest ]] || invalid 'manifest not readable'

# Validate the entire manifest before writing any artifacts.
paths=() digests=() sizes=() groups=() urls=()
while read -r path digest size group _license url extra || [[ -n $path ]]; do
  case $path in ''|'#'*) continue ;; esac
  [[ -n $url && -z $extra ]] || invalid 'manifest requires six columns'
  [[ $digest =~ ^[0-9a-f]{64}$ && $size =~ ^[1-9][0-9]*$ ]] || invalid 'invalid digest or size'
  case $group in common|onnx|openvino) ;; *) invalid 'invalid manifest group' ;; esac
  case /$path/ in //*|*/../*|*/./*|*//*) invalid 'unsafe artifact path' ;; esac
  for existing in "${paths[@]}"; do
    [[ $existing != "$path" ]] || invalid 'duplicate artifact path'
  done
  # file:// is reserved for offline fixtures; HF downloads require a full commit.
  if [[ $url != file:///* && ! $url =~ ^https://huggingface\.co/[^/]+/[^/]+/resolve/[0-9a-f]{40}/.+$ ]]; then
    invalid 'artifact URL must pin a full Hugging Face revision'
  fi
  paths+=("$path"); digests+=("$digest"); sizes+=("$size")
  groups+=("$group"); urls+=("$url")
done < "$manifest"
((${#paths[@]})) || invalid 'empty manifest'

matches() {
  local actual
  [[ -f $1 && $(stat -c %s -- "$1") == "$3" ]] || return 1
  actual=$(sha256sum < "$1") || return 1
  [[ ${actual%% *} == "$2" ]]
}

part=
cleanup() { if [[ -n $part ]]; then rm -f -- "$part"; fi; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
failures=0
for index in "${!paths[@]}"; do
  group=${groups[index]}
  [[ $group == common || $backend == all || $group == "$backend" ]] || continue
  path=${paths[index]}
  target=$dir/$path
  if matches "$target" "${digests[index]}" "${sizes[index]}"; then
    echo "OK   $path"
    continue
  fi
  if ((verify)); then
    echo "FAIL $path: missing or size/hash mismatch" >&2
    failures=1
    continue
  fi
  # A unique temporary file on the same filesystem makes publication atomic
  # and leaves an existing target intact when download or verification fails.
  if ! mkdir -p -- "$(dirname "$target")"; then
    echo "FAIL $path: cannot create directory" >&2
    failures=1
    continue
  fi
  part=$(mktemp -- "$target.part.XXXXXX")
  if curl --fail --location --silent --show-error --retry 3 \
      --output "$part" "${urls[index]}" &&
      matches "$part" "${digests[index]}" "${sizes[index]}" &&
      chmod 644 -- "$part" && mv -f -- "$part" "$target"; then
    part=
    echo "DOWN $path"
  else
    echo "FAIL $path: download or checksum verification failed" >&2
    cleanup
    part=
    failures=1
  fi
done
exit "$failures"
