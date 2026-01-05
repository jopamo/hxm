#!/bin/bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "SKIP: Xvfb not found" >&2
  exit 77
fi

pick_display() {
  if [ -n "${XVFB_DISPLAY:-}" ]; then
    echo "$XVFB_DISPLAY"
    return 0
  fi
  for i in $(seq 98 110); do
    if [ ! -e "/tmp/.X${i}-lock" ]; then
      echo ":$i"
      return 0
    fi
  done
  echo ":98"
}

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

DISPLAY_NUM=$(pick_display)
export DISPLAY=$DISPLAY_NUM

cleanup() {
  if [ -n "${HXM_PID:-}" ] && kill -0 "$HXM_PID" 2>/dev/null; then
    kill "$HXM_PID"
    wait "$HXM_PID" || true
  fi
  if [ -n "${XVFB_PID:-}" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
    kill "$XVFB_PID"
    wait "$XVFB_PID" || true
  fi
}
trap cleanup EXIT

# Start Xvfb
echo "Starting Xvfb on $DISPLAY..."
Xvfb "$DISPLAY" -screen 0 1280x720x24 +extension RANDR >/dev/null 2>&1 &
XVFB_PID=$!

sleep 2
if ! kill -0 "$XVFB_PID" 2>/dev/null; then
  echo "Xvfb failed to start on $DISPLAY" >&2
  exit 1
fi

# Start hxm
echo "Starting hxm..."
wm_log="${repo_root}/build/hxm.log"
mkdir -p "$(dirname "$wm_log")"
"$hxm_bin" >"$wm_log" 2>&1 &
HXM_PID=$!

sleep 2

# Run integration test
echo "Running integration client..."
# Pass a longer timeout to the C client if TSAN is active
timeout_ms=5000
if [[ -n "${TSAN_OPTIONS:-}" ]]; then
  timeout_ms=30000
fi
export HXM_TEST_TIMEOUT_MS="$timeout_ms"

if ! "$integration_client"; then
  echo "Integration test failed!" >&2
  echo "WM log tail:" >&2
  tail -n 200 "$wm_log" >&2 || true
  exit 1
fi

echo "Integration test complete."
