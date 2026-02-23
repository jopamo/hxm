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
  if [ -n "${XVFB_PID:-}" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
    kill "$XVFB_PID"
    wait "$XVFB_PID" || true
  fi
  rm -rf "$tmp_home"
}
trap cleanup EXIT

force_direct_xvfb=${HXM_FORCE_DIRECT_XVFB:-0}
has_xvfb_run=0
if [ "$force_direct_xvfb" = "1" ]; then
  if ! command -v Xvfb >/dev/null 2>&1; then
    echo "SKIP: Xvfb not found (required when HXM_FORCE_DIRECT_XVFB=1)" >&2
    exit 77
  fi
elif command -v xvfb-run >/dev/null 2>&1; then
  has_xvfb_run=1
elif ! command -v Xvfb >/dev/null 2>&1; then
  echo "SKIP: neither xvfb-run nor Xvfb found" >&2
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

# Pass a longer timeout to the C client when running under sanitizers.
timeout_ms=5000
if [[ -n "${TSAN_OPTIONS:-}" ]]; then
  timeout_ms=30000
elif [[ -n "${ASAN_OPTIONS:-}" || -n "${UBSAN_OPTIONS:-}" ]]; then
  timeout_ms=15000
fi
export HXM_TEST_TIMEOUT_MS="$timeout_ms"

wm_log="${repo_root}/build/hxm.log"
mkdir -p "$(dirname "$wm_log")"

if [ "$has_xvfb_run" -eq 1 ]; then
  echo "Running integration client with xvfb-run..."

  xvfb-run -a -s "-screen 0 1600x1200x24 +extension RANDR" bash -c "
    set -e
    \"$hxm_bin\" >\"$wm_log\" 2>&1 &
    HXM_PID=\$!
    sleep 2

    if ! \"$integration_client\"; then
      echo \"Integration test failed!\" >&2
      echo \"WM log tail:\" >&2
      tail -n 200 \"$wm_log\" >&2 || true
      kill \$HXM_PID
      exit 1
    fi

    kill \$HXM_PID
    wait \$HXM_PID || true
  "
else
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
    echo ":99"
  }

  display_num=$(pick_display)
  export DISPLAY="$display_num"

  echo "Running integration client with direct Xvfb on $DISPLAY..."
  Xvfb "$DISPLAY" -screen 0 1600x1200x24 +extension RANDR >/dev/null 2>&1 &
  XVFB_PID=$!

  sleep 2
  if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    echo "Xvfb failed to start on $DISPLAY" >&2
    exit 1
  fi

  "$hxm_bin" >"$wm_log" 2>&1 &
  HXM_PID=$!
  sleep 2

  if ! "$integration_client"; then
    echo "Integration test failed!" >&2
    echo "WM log tail:" >&2
    tail -n 200 "$wm_log" >&2 || true
    exit 1
  fi
fi

echo "Integration test complete."
