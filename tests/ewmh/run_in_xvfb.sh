#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

client_src="$script_dir/x_test_client.c"
client_bin="$script_dir/x_test_client"

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "pkg-config not found" >&2
  exit 1
fi

cc=${CC:-cc}
if ! $cc -std=c11 -O2 -Wall -Wextra -o "$client_bin" "$client_src" $(pkg-config --cflags --libs xcb); then
  echo "failed to build x_test_client" >&2
  exit 1
fi

bbox_bin=${BBOX_BIN:-}
if [ -z "$bbox_bin" ]; then
  if [ -x "$repo_root/build/bbox" ]; then
    bbox_bin="$repo_root/build/bbox"
  elif [ -x "$repo_root/bbox" ]; then
    bbox_bin="$repo_root/bbox"
  else
    echo "bbox binary not found; set BBOX_BIN" >&2
    exit 1
  fi
fi

tmp_home=$(mktemp -d)
cleanup() {
  if [ -n "${bbox_pid:-}" ] && kill -0 "$bbox_pid" 2>/dev/null; then
    kill "$bbox_pid"
    wait "$bbox_pid" || true
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
mkdir -p "$XDG_CONFIG_HOME/bbox"
cat >"$XDG_CONFIG_HOME/bbox/bbox.conf" <<'EOF'
desktop_count = 3
desktop_names = one,two,three
EOF

export DISPLAY=:99
Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
xvfb_pid=$!

for _ in $(seq 1 50); do
  if "$client_bin" get-root-cardinals _NET_SUPPORTED >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

"$bbox_bin" >/dev/null 2>&1 &
bbox_pid=$!

for _ in $(seq 1 50); do
  if "$client_bin" get-root-cardinals _NET_SUPPORTED >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

"$script_dir/test_desktops.sh" "$client_bin"
"$script_dir/test_strut_removal.sh" "$client_bin"
"$script_dir/test_state_remove.sh" "$client_bin"
