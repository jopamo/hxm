#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

client_src="$script_dir/x_test_client.c"
client_bin="$script_dir/x_test_client"
panel_src="$script_dir/panel_simulator.c"
panel_bin="$script_dir/panel_simulator"

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "SKIP: Xvfb not found" >&2
  exit 77
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "SKIP: pkg-config not found" >&2
  exit 77
fi

cc=${CC:-cc}
if ! $cc -std=c11 -O2 -Wall -Wextra -o "$client_bin" "$client_src" $(pkg-config --cflags --libs xcb); then
  echo "failed to build x_test_client" >&2
  exit 1
fi
if ! $cc -std=c11 -O2 -Wall -Wextra -o "$panel_bin" "$panel_src" $(pkg-config --cflags --libs xcb); then
  echo "failed to build panel_simulator" >&2
  exit 1
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

wait_for_x() {
  for _ in $(seq 1 50); do
    if "$client_bin" get-root-cardinals _NET_SUPPORTED >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

wait_for_substructure_redirect() {
  for _ in $(seq 1 100); do
    if "$client_bin" assert-substructure-redirect >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

wait_for_supporting_wm_check() {
  for _ in $(seq 1 100); do
    local json
    json=$("$client_bin" get-root-cardinals _NET_SUPPORTING_WM_CHECK 2>/dev/null || true)
    local win
    win=$(echo "$json" | sed 's/[^0-9]/ /g' | awk '{print $1}')
    if [ -n "$win" ] && [ "$win" != "0" ]; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

pick_display() {
  if [ -n "${XVFB_DISPLAY:-}" ]; then
    echo "$XVFB_DISPLAY"
    return 0
  fi
  for i in $(seq 99 120); do
    if [ ! -e "/tmp/.X${i}-lock" ]; then
      echo ":$i"
      return 0
    fi
  done
  echo ":99"
}

start_xvfb() {
  Xvfb "$DISPLAY" -screen 0 1024x768x24 "$@" >/dev/null 2>&1 &
  xvfb_pid=$!
}

tmp_home=$(mktemp -d)
cleanup() {
  if [ -n "${hxm_pid:-}" ] && kill -0 "$hxm_pid" 2>/dev/null; then
    kill "$hxm_pid"
    wait "$hxm_pid" || true
  fi
  if [ -n "${xvfb_pid:-}" ] && kill -0 "$xvfb_pid" 2>/dev/null; then
    kill "$xvfb_pid"
    wait "$xvfb_pid" || true
  fi
  rm -rf "$tmp_home"
}
trap cleanup EXIT

export HOME="$tmp_home"
export XDG_CONFIG_HOME="$tmp_home/.config"
mkdir -p "$XDG_CONFIG_HOME/hxm"
cat >"$XDG_CONFIG_HOME/hxm/hxm.conf" <<'EOF'
desktop_count = 3
desktop_names = one,two,three
EOF

export DISPLAY=$(pick_display)
start_xvfb +extension RANDR
if ! wait_for_x; then
  if [ -n "${xvfb_pid:-}" ] && kill -0 "$xvfb_pid" 2>/dev/null; then
    kill "$xvfb_pid"
    wait "$xvfb_pid" || true
  fi
  start_xvfb
  if ! wait_for_x; then
    echo "failed to start Xvfb" >&2
    exit 1
  fi
fi

if [ "$("$client_bin" has-extension RANDR)" != "yes" ]; then
  echo "warning: RANDR extension not available on Xvfb" >&2
fi

"$hxm_bin" >/dev/null 2>&1 &
hxm_pid=$!

if ! wait_for_supporting_wm_check; then
  echo "WM did not publish _NET_SUPPORTING_WM_CHECK" >&2
  exit 1
fi

if ! wait_for_substructure_redirect; then
  echo "WM did not claim SubstructureRedirect on root" >&2
  exit 1
fi

"$script_dir/test_desktops.sh" "$client_bin"
"$script_dir/test_strut_removal.sh" "$client_bin"
"$script_dir/test_state_remove.sh" "$client_bin"
# Export hxm_pid for test_restart.sh
export hxm_pid
"$script_dir/test_restart.sh" "$client_bin"
