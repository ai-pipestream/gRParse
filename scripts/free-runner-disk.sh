#!/bin/sh
# Frees disk on a GitHub-hosted runner for the seeded image build without
# holding the job up (#245). The serial `sudo rm -rf` of four SDK trees took
# 1-3 min per job; `start` runs it in the background so it overlaps the
# logins, the seed lookup and the ccache restore, and `wait` blocks only
# right before the build needs the space.
#
#   sh scripts/free-runner-disk.sh start
#   sh scripts/free-runner-disk.sh wait
set -eu

marker="${RUNNER_TEMP:-/tmp}/free-runner-disk"
case "${1:?start or wait}" in
  start)
    df -h /
    rm -f "$marker.done"
    nohup sh -c "sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /opt/hostedtoolcache/CodeQL; touch '$marker.done'" \
      > "$marker.log" 2>&1 &
    echo "free-runner-disk: started in the background"
    ;;
  wait)
    waited=0
    while [ ! -f "$marker.done" ]; do
      if [ "$waited" -ge 600 ]; then
        echo "free-runner-disk: still deleting after 600 s" >&2
        cat "$marker.log" >&2 || true
        exit 1
      fi
      sleep 2
      waited=$((waited + 2))
    done
    echo "free-runner-disk: done (waited ${waited} s)"
    df -h /
    ;;
  *)
    echo "usage: free-runner-disk.sh start|wait" >&2
    exit 64
    ;;
esac
