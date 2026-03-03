#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

tmp_home=$(mktemp -d)
export HOME="$tmp_home"
export XDG_CONFIG_HOME="$tmp_home/.config"
mkdir -p "$XDG_CONFIG_HOME/hxm"

cleanup() {
  if [ -n "${HXM_PID:-}" ] && kill -0 "$HXM_PID" 2>/dev/null; then
    kill "$HXM_PID"
    wait "$HXM_PID" || true
  fi
  rm -rf "$tmp_home"
}
trap cleanup EXIT

if [ -z "${DISPLAY:-}" ]; then
  echo "SKIP: DISPLAY is not set; run on a real X11 display" >&2
  exit 77
fi

hxm_bin=${HXM_BIN:-}
if [ -z "$hxm_bin" ]; then
  if [ -x "$repo_root/build/hxm" ]; then
    hxm_bin="$repo_root/build/hxm"
  elif [ -x "$repo_root/hxm" ]; then
    hxm_bin="$repo_root/hxm"
  else
    echo "hxm binary not found; set HXM_BIN" >&2
    exit 1
  fi
fi

integration_client=${INTEGRATION_CLIENT:-}
if [ -z "$integration_client" ]; then
  if [ -x "$repo_root/build/integration_client" ]; then
    integration_client="$repo_root/build/integration_client"
  elif [ -x "$repo_root/tests/integration_client" ]; then
    integration_client="$repo_root/tests/integration_client"
  else
    echo "integration_client not found; set INTEGRATION_CLIENT" >&2
    exit 1
  fi
fi

timeout_ms=5000
if [[ -n "${TSAN_OPTIONS:-}" ]]; then
  timeout_ms=30000
elif [[ -n "${ASAN_OPTIONS:-}" || -n "${UBSAN_OPTIONS:-}" ]]; then
  timeout_ms=15000
fi
export HXM_TEST_TIMEOUT_MS="$timeout_ms"

wm_log="${repo_root}/build/hxm-real-x11.log"
mkdir -p "$(dirname "$wm_log")"

echo "Running integration client on real X11 DISPLAY=${DISPLAY}..."
"$hxm_bin" >"$wm_log" 2>&1 &
HXM_PID=$!
sleep 2

if ! kill -0 "$HXM_PID" 2>/dev/null; then
  echo "hxm failed to start on DISPLAY=${DISPLAY}" >&2
  echo "WM log tail:" >&2
  tail -n 200 "$wm_log" >&2 || true
  exit 1
fi

if ! "$integration_client"; then
  echo "Integration test failed on DISPLAY=${DISPLAY}" >&2
  echo "WM log tail:" >&2
  tail -n 200 "$wm_log" >&2 || true
  exit 1
fi

kill "$HXM_PID"
wait "$HXM_PID" || true

echo "Real X11 integration test complete."
