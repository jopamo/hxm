#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

tmp_home=$(mktemp -d)
export HOME="$tmp_home"
export XDG_CONFIG_HOME="$tmp_home/.config"
mkdir -p "$XDG_CONFIG_HOME/hxm"

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "SKIP: Xvfb not found" >&2
  exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found" >&2
  exit 77
fi

start_xvfb() {
  local start=98
  local end=140
  if [ -n "${XVFB_DISPLAY:-}" ]; then
    start=${XVFB_DISPLAY#:}
    end=$start
  fi

  for i in $(seq "$start" "$end"); do
    local disp=":$i"
    Xvfb "$disp" -screen 0 1280x720x24 +extension RANDR >/dev/null 2>&1 &
    XVFB_PID=$!

    local alive=1
    for _ in $(seq 1 40); do
      if ! kill -0 "$XVFB_PID" 2>/dev/null; then
        alive=0
        break
      fi
      sleep 0.05
    done

    if [ "$alive" -eq 1 ]; then
      export DISPLAY="$disp"
      return 0
    fi

    wait "$XVFB_PID" 2>/dev/null || true
    unset XVFB_PID

    if [ -n "${XVFB_DISPLAY:-}" ]; then
      break
    fi
  done

  return 1
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

dummy_client=${DUMMY_CLIENT:-}
if [ -z "$dummy_client" ]; then
  if [ -x "$repo_root/build/dummy_client" ]; then
    dummy_client="$repo_root/build/dummy_client"
  elif [ -x "$repo_root/tests/dummy_client" ]; then
    dummy_client="$repo_root/tests/dummy_client"
  else
    echo "dummy_client not found; set DUMMY_CLIENT" >&2
    exit 1
  fi
fi

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

if ! start_xvfb; then
  echo "SKIP: unable to start Xvfb (check xkeyboard-config setup)" >&2
  exit 77
fi
echo "Starting Xvfb on $DISPLAY..."

echo "Starting hxm..."
"$hxm_bin" >/dev/null 2>&1 &
HXM_PID=$!

sleep 2

echo "Running stress test..."
DUMMY_CLIENT="$dummy_client" python3 "$repo_root/scripts/stress-test.py"

echo "Stress test complete."
