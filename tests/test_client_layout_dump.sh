#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
bin="${CLIENT_LAYOUT_DUMP_BIN:-$repo_root/build/client_layout_dump}"

if [[ ! -x "$bin" ]]; then
  echo "layout dump binary missing: $bin" >&2
  exit 1
fi

output=$($bin)

require_line() {
  local pattern=$1
  if ! grep -Eq "$pattern" <<<"$output"; then
    echo "missing expected output line matching: $pattern" >&2
    exit 1
  fi
}

require_line '^TYPE client_hot_t SIZE [0-9]+$'
require_line '^TYPE client_cold_t SIZE [0-9]+$'
require_line '^TYPE rect_t SIZE [0-9]+$'
require_line '^TYPE size_hints_t SIZE [0-9]+$'
require_line '^TYPE dirty_region_t SIZE [0-9]+$'
require_line '^TYPE pending_state_msg_t SIZE [0-9]+$'
require_line '^TYPE strut_t SIZE [0-9]+$'

require_line '^OFFSET client_hot_t.xid [0-9]+$'
require_line '^OFFSET client_hot_t.server [0-9]+$'
require_line '^OFFSET client_hot_t.desired [0-9]+$'
require_line '^OFFSET client_hot_t.pending [0-9]+$'
require_line '^OFFSET client_hot_t.stacking_index [0-9]+$'
require_line '^OFFSET client_hot_t.dirty [0-9]+$'
require_line '^OFFSET client_hot_t.state [0-9]+$'
require_line '^OFFSET client_hot_t.flags [0-9]+$'

require_line '^OFFSET client_cold_t.title [0-9]+$'
require_line '^OFFSET client_cold_t.visual_id [0-9]+$'
require_line '^OFFSET client_cold_t.depth [0-9]+$'
require_line '^OFFSET client_cold_t.colormap [0-9]+$'
require_line '^OFFSET client_cold_t.frame_colormap [0-9]+$'
require_line '^OFFSET client_cold_t.render_ctx [0-9]+$'
require_line '^OFFSET client_cold_t.icon_surface [0-9]+$'
require_line '^OFFSET client_cold_t.hints [0-9]+$'
require_line '^OFFSET client_cold_t.hints_flags [0-9]+$'
require_line '^OFFSET client_cold_t.damage [0-9]+$'
require_line '^OFFSET client_cold_t.damage_region [0-9]+$'
require_line '^OFFSET client_cold_t.frame_damage [0-9]+$'
require_line '^OFFSET client_cold_t.protocols [0-9]+$'
require_line '^OFFSET client_cold_t.sync_value [0-9]+$'
require_line '^OFFSET client_cold_t.manage_phase [0-9]+$'
require_line '^OFFSET client_cold_t.pending_state_count [0-9]+$'
require_line '^OFFSET client_cold_t.pending_state_msgs [0-9]+$'
require_line '^OFFSET client_cold_t.icon_geometry [0-9]+$'
require_line '^OFFSET client_cold_t.window_opacity [0-9]+$'
require_line '^OFFSET client_cold_t.bypass_compositor [0-9]+$'
require_line '^OFFSET client_cold_t.fullscreen_monitors [0-9]+$'
require_line '^OFFSET client_cold_t.gtk_frame_extents_set [0-9]+$'
require_line '^OFFSET client_cold_t.gtk_extents [0-9]+$'
require_line '^OFFSET client_cold_t.user_time [0-9]+$'
require_line '^OFFSET client_cold_t.user_time_window [0-9]+$'
require_line '^OFFSET client_cold_t.strut [0-9]+$'
require_line '^OFFSET client_cold_t.pid [0-9]+$'

if grep -Eq '^OFFSET client_hot_t.visual_id [0-9]+$' <<<"$output"; then
  echo "unexpected hot visual_id offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.hints [0-9]+$' <<<"$output"; then
  echo "unexpected hot hints offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.hints_flags [0-9]+$' <<<"$output"; then
  echo "unexpected hot hints_flags offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.sync_value [0-9]+$' <<<"$output"; then
  echo "unexpected hot sync_value offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.manage_phase [0-9]+$' <<<"$output"; then
  echo "unexpected hot manage_phase offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.pending_state_count [0-9]+$' <<<"$output"; then
  echo "unexpected hot pending_state_count offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.pending_state_msgs [0-9]+$' <<<"$output"; then
  echo "unexpected hot pending_state_msgs offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.icon_geometry [0-9]+$' <<<"$output"; then
  echo "unexpected hot icon_geometry offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.window_opacity [0-9]+$' <<<"$output"; then
  echo "unexpected hot window_opacity offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.bypass_compositor [0-9]+$' <<<"$output"; then
  echo "unexpected hot bypass_compositor offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.fullscreen_monitors [0-9]+$' <<<"$output"; then
  echo "unexpected hot fullscreen_monitors offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.gtk_frame_extents_set [0-9]+$' <<<"$output"; then
  echo "unexpected hot gtk_frame_extents_set offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.gtk_extents [0-9]+$' <<<"$output"; then
  echo "unexpected hot gtk_extents offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.user_time [0-9]+$' <<<"$output"; then
  echo "unexpected hot user_time offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.user_time_window [0-9]+$' <<<"$output"; then
  echo "unexpected hot user_time_window offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.damage [0-9]+$' <<<"$output"; then
  echo "unexpected hot damage offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.damage_region [0-9]+$' <<<"$output"; then
  echo "unexpected hot damage_region offset found in layout dump output" >&2
  exit 1
fi

if grep -Eq '^OFFSET client_hot_t.frame_damage [0-9]+$' <<<"$output"; then
  echo "unexpected hot frame_damage offset found in layout dump output" >&2
  exit 1
fi

echo "test_client_layout_dump passed"
