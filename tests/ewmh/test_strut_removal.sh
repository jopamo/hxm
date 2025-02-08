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

wait_for_workarea_top 30

"$client" set-window-cardinals "$win" _NET_WM_STRUT 0 0 20 0
"$client" delete-window-prop "$win" _NET_WM_STRUT_PARTIAL
wait_for_workarea_top 20

"$client" delete-window-prop "$win" _NET_WM_STRUT
wait_for_workarea_top 0

"$client" set-window-cardinals "$win" _NET_WM_STRUT_PARTIAL \
  0 0 25 0 \
  0 0 0 0 \
  0 1024 0 0
wait_for_workarea_top 25

"$client" set-window-cardinals "$win" _NET_WM_STRUT_PARTIAL
wait_for_workarea_top 0

echo "PASS: strut removal"
