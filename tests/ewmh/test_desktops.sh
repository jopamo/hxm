#!/usr/bin/env bash
set -euo pipefail

client=${1:?}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

get_first_value() {
  echo "$1" | sed 's/[^0-9]/ /g' | awk '{print $1}'
}

wait_for_root_value() {
  local atom=$1
  local expected=$2
  for _ in $(seq 1 200); do
    local json
    json=$("$client" get-root-cardinals "$atom")
    local got
    got=$(get_first_value "$json")
    if [ "$got" = "$expected" ]; then
      return 0
    fi
    sleep 0.02
  done
  fail "timeout waiting for $atom to be $expected"
}

wait_for_window_value() {
  local win=$1
  local atom=$2
  local expected=$3
  for _ in $(seq 1 200); do
    local json
    json=$("$client" get-window-cardinals "$win" "$atom")
    local got
    got=$(get_first_value "$json")
    if [ "$got" = "$expected" ]; then
      return 0
    fi
    sleep 0.02
  done
  fail "timeout waiting for $atom on $win to be $expected"
}

json=$("$client" get-root-cardinals _NET_NUMBER_OF_DESKTOPS)
count=$(get_first_value "$json")
if [ "$count" != "3" ]; then
  fail "expected 3 desktops, got $count"
fi

"$client" send-client-message 0 _NET_CURRENT_DESKTOP 1 0 0 0 0
wait_for_root_value _NET_CURRENT_DESKTOP 1

win=$("$client" create-window)
"$client" map-window "$win"

"$client" send-client-message "$win" _NET_WM_DESKTOP 2 1 0 0 0
wait_for_window_value "$win" _NET_WM_DESKTOP 2

echo "PASS: desktops"
