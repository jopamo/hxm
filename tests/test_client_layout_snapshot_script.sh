#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
snapshot_script="$repo_root/scripts/snapshot-client-layout.sh"

if [[ ! -x "$snapshot_script" ]]; then
  echo "snapshot script missing or not executable: $snapshot_script" >&2
  exit 1
fi

output=$($snapshot_script)

require_output_line() {
  local pattern=$1
  if ! grep -Eq "$pattern" <<<"$output"; then
    echo "missing expected output line matching: $pattern" >&2
    exit 1
  fi
}

require_output_line '^SNAPSHOT_COMMIT [0-9a-f]{40}$'
require_output_line '^SNAPSHOT_TIME_UTC [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$'
require_output_line '^TYPE client_hot_t SIZE [0-9]+$'
require_output_line '^TYPE client_cold_t SIZE [0-9]+$'
require_output_line '^OFFSET client_cold_t.visual_id [0-9]+$'
require_output_line '^OFFSET client_cold_t.render_ctx [0-9]+$'
require_output_line '^OFFSET client_cold_t.sync_value [0-9]+$'
require_output_line '^OFFSET client_cold_t.manage_phase [0-9]+$'
require_output_line '^OFFSET client_cold_t.pending_state_count [0-9]+$'
require_output_line '^OFFSET client_cold_t.pending_state_msgs [0-9]+$'
require_output_line '^OFFSET client_cold_t.icon_geometry [0-9]+$'
require_output_line '^OFFSET client_cold_t.window_opacity [0-9]+$'
require_output_line '^OFFSET client_cold_t.bypass_compositor [0-9]+$'
require_output_line '^OFFSET client_cold_t.fullscreen_monitors [0-9]+$'

expected_commit=$(git -C "$repo_root" rev-parse HEAD)
actual_commit=$(grep '^SNAPSHOT_COMMIT ' <<<"$output" | awk '{print $2}')
if [[ "$actual_commit" != "$expected_commit" ]]; then
  echo "snapshot commit mismatch: expected $expected_commit got $actual_commit" >&2
  exit 1
fi

tmpfile=$(mktemp)
trap 'rm -f "$tmpfile"' EXIT
$snapshot_script "$tmpfile"

if [[ ! -s "$tmpfile" ]]; then
  echo "snapshot output file was not created: $tmpfile" >&2
  exit 1
fi

file_commit=$(grep '^SNAPSHOT_COMMIT ' "$tmpfile" | awk '{print $2}')
if [[ "$file_commit" != "$expected_commit" ]]; then
  echo "snapshot file commit mismatch: expected $expected_commit got $file_commit" >&2
  exit 1
fi

echo "test_client_layout_snapshot_script passed"
