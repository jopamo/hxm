#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "SKIP: Xvfb not found" >&2
  exit 77
fi

if ! command -v conky >/dev/null 2>&1; then
  echo "SKIP: conky not found" >&2
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

probe_bin=${CONKY_PROBE_BIN:-}
if [ -z "$probe_bin" ]; then
  if [ -x "$repo_root/build/conky_probe" ]; then
    probe_bin="$repo_root/build/conky_probe"
  else
    echo "conky_probe binary not found; set CONKY_PROBE_BIN" >&2
    exit 1
  fi
fi

normal_bin=${CONKY_NORMAL_BIN:-}
if [ -z "$normal_bin" ]; then
  if [ -x "$repo_root/build/conky_normal_client" ]; then
    normal_bin="$repo_root/build/conky_normal_client"
  else
    echo "conky_normal_client binary not found; set CONKY_NORMAL_BIN" >&2
    exit 1
  fi
fi

conky_conf="$script_dir/conky_test.conf"
if [ ! -f "$conky_conf" ]; then
  echo "conky config not found: $conky_conf" >&2
  exit 1
fi

tmp_home=$(mktemp -d)
wm_log="$tmp_home/hxm.log"

cleanup() {
  if [ -n "${conky_pid:-}" ]; then
    kill "$conky_pid" >/dev/null 2>&1 || true
    wait "$conky_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${normal_pid:-}" ]; then
    kill "$normal_pid" >/dev/null 2>&1 || true
    wait "$normal_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${wm_pid:-}" ]; then
    kill "$wm_pid" >/dev/null 2>&1 || true
    wait "$wm_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${xvfb_pid:-}" ]; then
    kill "$xvfb_pid" >/dev/null 2>&1 || true
    wait "$xvfb_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp_home"
}
trap cleanup EXIT

export HOME="$tmp_home"
export XDG_CONFIG_HOME="$tmp_home/.config"
mkdir -p "$XDG_CONFIG_HOME/hxm"
cat >"$XDG_CONFIG_HOME/hxm/hxm.conf" <<'CONF'
desktop_count = 1
CONF

export DISPLAY=:99
Xvfb :99 -screen 0 1920x1080x24 +extension RANDR >/dev/null 2>&1 &
xvfb_pid=$!
sleep 0.2

"$hxm_bin" >"$wm_log" 2>&1 &
wm_pid=$!

conky -c "$conky_conf" >/dev/null 2>&1 &
conky_pid=$!

"$normal_bin" >/dev/null 2>&1 &
normal_pid=$!

set +e
CONKY_CLASS_MATCH="Conky" \
CONKY_NAME_MATCH="hxm-conky-test" \
NORMAL_CLASS_MATCH="HxmNormal" \
NORMAL_NAME_MATCH="hxm-normal" \
EXPECT_TYPE="DOCK" \
EXPECT_BELOW=1 \
EXPECT_STICKY=1 \
EXPECT_SKIP_TASKBAR=1 \
EXPECT_SKIP_PAGER=1 \
CHECK_STACKING=1 \
REQUIRE_HINTS=0 \
REQUIRE_NORMAL_HINTS=0 \
MIN_W=50 \
MIN_H=20 \
"$probe_bin"
probe_rc=$?
set -e

if [ $probe_rc -ne 0 ]; then
  echo "conky_probe failed (rc=$probe_rc)" >&2
  echo "WM log tail:" >&2
  tail -n 200 "$wm_log" >&2 || true
  exit $probe_rc
fi

echo "conky_probe passed"
