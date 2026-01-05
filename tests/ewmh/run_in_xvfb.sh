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

if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "SKIP: xvfb-run not found" >&2
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

tmp_home=$(mktemp -d)
cleanup() {
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

echo "Running EWMH tests with xvfb-run..."

export HXM_BIN="$hxm_bin"
export HXM_LOG_FILE="${HXM_LOG_FILE:-/dev/null}"
export EWMH_CLIENT_BIN="$client_bin"
export EWMH_SCRIPT_DIR="$script_dir"

xvfb-run -a -s "-screen 0 1024x768x24 +extension RANDR" bash <<'EOF'
set -e

if [ "$("$EWMH_CLIENT_BIN" has-extension RANDR)" != "yes" ]; then
  echo "warning: RANDR extension not available on Xvfb" >&2
fi

hxm_log_file="${HXM_LOG_FILE:-/dev/null}"
"$HXM_BIN" >"$hxm_log_file" 2>&1 &
hxm_pid=$!
export hxm_pid

# Wait for WM to be ready
for _ in $(seq 1 100); do
  json=$($EWMH_CLIENT_BIN get-root-cardinals _NET_SUPPORTING_WM_CHECK 2>/dev/null || true)
  win=$(echo "$json" | sed 's/[^0-9]/ /g' | awk '{print $1}')
  if [ -n "$win" ] && [ "$win" != "0" ]; then
    break
  fi
  sleep 0.05
done

if [ -z "$win" ] || [ "$win" = "0" ]; then
   echo "WM did not publish _NET_SUPPORTING_WM_CHECK" >&2
   kill $hxm_pid || true
   exit 1
fi

for _ in $(seq 1 100); do
  if "$EWMH_CLIENT_BIN" assert-substructure-redirect >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

# Run tests
if ! "$EWMH_SCRIPT_DIR/test_desktops.sh" "$EWMH_CLIENT_BIN"; then
   kill $hxm_pid || true
   exit 1
fi
if ! "$EWMH_SCRIPT_DIR/test_strut_removal.sh" "$EWMH_CLIENT_BIN"; then
   kill $hxm_pid || true
   exit 1
fi
if ! "$EWMH_SCRIPT_DIR/test_state_remove.sh" "$EWMH_CLIENT_BIN"; then
   kill $hxm_pid || true
   exit 1
fi
if ! "$EWMH_SCRIPT_DIR/test_restart.sh" "$EWMH_CLIENT_BIN"; then
   kill $hxm_pid || true
   exit 1
fi

kill $hxm_pid
wait $hxm_pid || true
EOF
