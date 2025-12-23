#!/usr/bin/env bash
set -euo pipefail

client=${1:?}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

get_values() {
  echo "$1" | sed 's/[^0-9]/ /g'
}

get_nth() {
  local json=$1
  local n=$2
  echo "$json" | sed 's/[^0-9]/ /g' | awk -v n="$n" '{print $n}'
}

read_window_cardinals() {
  local win=$1
  local atom=$2
  local json
  if ! json=$("$client" get-window-cardinals "$win" "$atom"); then
    fail "failed to read $atom from window $win"
  fi
  echo "$json"
}

assert_window_top() {
  local win=$1
  local atom=$2
  local expected=$3
  local json
  local top
  json=$(read_window_cardinals "$win" "$atom")
  top=$(get_nth "$json" 3)
  if [ -z "$top" ]; then
    top=0
  fi
  if [ "$top" != "$expected" ]; then
    echo "TEST INFRA FAILURE: $atom top expected=$expected got=$top json=$json" >&2
    exit 1
  fi
}

wait_for_workarea_top() {
  local expected=$1
  for _ in $(seq 1 200); do
    local json
    json=$("$client" get-root-cardinals _NET_WORKAREA)
    local top
    top=$(get_nth "$json" 2)
    if [ "$top" = "$expected" ]; then
      return 0
    fi
    sleep 0.02
  done
  fail "timeout waiting for workarea top=$expected"
}

out=$(mktemp)
"$client" create-window-and-sleep 10 >"$out" &
client_pid=$!
trap 'kill "$client_pid" 2>/dev/null || true; rm -f "$out"' EXIT

sleep 0.1
win=$(cat "$out")
if [ -z "$win" ]; then
  fail "failed to create window"
fi

"$client" set-window-atoms "$win" _NET_WM_WINDOW_TYPE _NET_WM_WINDOW_TYPE_DOCK
"$client" set-window-cardinals "$win" _NET_WM_STRUT_PARTIAL \
  0 0 30 0 \
  0 0 0 0 \
  0 1024 0 0
assert_window_top "$win" _NET_WM_STRUT_PARTIAL 30

wait_for_workarea_top 30

"$client" set-window-cardinals "$win" _NET_WM_STRUT 0 0 20 0
assert_window_top "$win" _NET_WM_STRUT 20
"$client" delete-window-prop "$win" _NET_WM_STRUT_PARTIAL
assert_window_top "$win" _NET_WM_STRUT_PARTIAL 0
wait_for_workarea_top 20

"$client" delete-window-prop "$win" _NET_WM_STRUT
assert_window_top "$win" _NET_WM_STRUT 0
wait_for_workarea_top 0

"$client" set-window-cardinals "$win" _NET_WM_STRUT_PARTIAL \
  0 0 25 0 \
  0 0 0 0 \
  0 1024 0 0
assert_window_top "$win" _NET_WM_STRUT_PARTIAL 25
wait_for_workarea_top 25

"$client" set-window-cardinals "$win" _NET_WM_STRUT_PARTIAL
assert_window_top "$win" _NET_WM_STRUT_PARTIAL 0
wait_for_workarea_top 0

echo "PASS: strut removal"
