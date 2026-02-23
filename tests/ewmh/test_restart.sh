#!/usr/bin/env bash
set -euo pipefail

client="$1"
hxm_pid="${2:?missing hxm pid}"
out=$(mktemp)
# Keep alive long enough for test
"$client" create-window-and-sleep 20 >"$out" &
client_pid=$!

sleep 0.5
win=$(cat "$out")
if [ -z "$win" ]; then
  echo "Failed to get window ID"
  exit 1
fi
echo "Created window $win"

# Window is already mapped by create-window-and-sleep
sleep 1.0

# Check if hxm manages it
json=$("$client" get-root-cardinals _NET_CLIENT_LIST)
echo "Client list: $json"

# Move to desktop 1
# _NET_WM_DESKTOP message: data[0]=desktop, data[1]=source
"$client" send-client-message "$win" _NET_WM_DESKTOP 1 2 0 0 0
sleep 1.0

# Switch current desktop to 1
"$client" send-client-message 0 _NET_CURRENT_DESKTOP 1 0 0 0 0
sleep 0.5
json=$("$client" get-root-cardinals _NET_CURRENT_DESKTOP)
cur=$(echo "$json" | grep -o '[0-9]*' | head -n 1)
if [ "$cur" != "1" ]; then
  echo "Failed to switch to desktop 1. Got: $cur"
  kill "$client_pid" || true
  exit 1
fi

# Verify desktop is 1
json=$("$client" get-window-cardinals "$win" _NET_WM_DESKTOP)
val=$(echo "$json" | grep -o '[0-9]*' | head -n 1)
if [ "$val" != "1" ]; then
  echo "Failed to move window to desktop 1. Got: $val"
  # Clean up client
  kill "$client_pid" || true
  exit 1
fi

echo "Window on desktop 1. Current desktop 1. Restarting hxm..."

kill -USR2 "$hxm_pid"
sleep 2.0

# Verify current desktop is still 1
json=$("$client" get-root-cardinals _NET_CURRENT_DESKTOP)
cur=$(echo "$json" | grep -o '[0-9]*' | head -n 1)
if [ "$cur" != "1" ]; then
  echo "Failed to preserve current desktop 1 after restart. Got: $cur"
  kill "$client_pid" || true
  exit 1
fi

# Verify desktop is still 1
json=$("$client" get-window-cardinals "$win" _NET_WM_DESKTOP)
val=$(echo "$json" | grep -o '[0-9]*' | head -n 1)

if [ "$val" != "1" ]; then
  echo "Failed to preserve desktop 1 after restart. Got: $val"
  kill "$client_pid" || true
  exit 1
fi

# Verify focus restoration
json=$("$client" get-root-cardinals _NET_ACTIVE_WINDOW)
val=$(echo "$json" | grep -o '[0-9]*' | head -n 1)
if [ "$val" != "$win" ]; then
  echo "Failed to restore focus to $win. Got: $val"
  kill "$client_pid" || true
  exit 1
fi

echo "Restart persistence test passed!"
