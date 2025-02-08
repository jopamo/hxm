#!/usr/bin/env bash
set -euo pipefail

client=${1:?}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

get_atom() {
  "$client" get-atom "$1"
}

get_values() {
  echo "$1" | sed 's/[^0-9]/ /g'
}

has_value() {
  local json=$1
  local want=$2
  echo "$json" | sed 's/[^0-9]/ /g' | awk -v w="$want" '{for (i=1;i<=NF;i++) if ($i==w) {print "yes"; exit}}'
}

wait_for_state() {
  local win=$1
  local want_atom=$2
  local present=$3
  for _ in $(seq 1 200); do
    local json
    json=$("$client" get-window-cardinals "$win" _NET_WM_STATE)
    local found
    found=$(has_value "$json" "$want_atom" || true)
    if [ "$present" = "yes" ] && [ "$found" = "yes" ]; then
      return 0
    fi
    if [ "$present" = "no" ] && [ -z "$found" ]; then
      return 0
    fi
    sleep 0.02
  done
  fail "timeout waiting for state atom $want_atom present=$present"
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

"$client" map-window "$win"

atom_above=$(get_atom _NET_WM_STATE_ABOVE)
atom_below=$(get_atom _NET_WM_STATE_BELOW)
atom_maxv=$(get_atom _NET_WM_STATE_MAXIMIZED_VERT)

"$client" send-client-message "$win" _NET_WM_STATE 1 "$atom_above" "$atom_maxv" 0 0
wait_for_state "$win" "$atom_above" yes
wait_for_state "$win" "$atom_maxv" yes

"$client" send-client-message "$win" _NET_WM_STATE 0 "$atom_above" 0 0 0
wait_for_state "$win" "$atom_above" no
wait_for_state "$win" "$atom_maxv" yes

"$client" send-client-message "$win" _NET_WM_STATE 1 "$atom_below" 0 0 0
wait_for_state "$win" "$atom_below" yes

"$client" send-client-message "$win" _NET_WM_STATE 0 "$atom_above" 0 0 0
wait_for_state "$win" "$atom_below" yes

echo "PASS: state removal"
