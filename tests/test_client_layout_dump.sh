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
require_line '^OFFSET client_hot_t.hints [0-9]+$'
require_line '^OFFSET client_hot_t.stacking_index [0-9]+$'
require_line '^OFFSET client_hot_t.dirty [0-9]+$'
require_line '^OFFSET client_hot_t.state [0-9]+$'
require_line '^OFFSET client_hot_t.flags [0-9]+$'
require_line '^OFFSET client_hot_t.damage_region [0-9]+$'
require_line '^OFFSET client_hot_t.sync_value [0-9]+$'
require_line '^OFFSET client_hot_t.fullscreen_monitors [0-9]+$'

require_line '^OFFSET client_cold_t.title [0-9]+$'
require_line '^OFFSET client_cold_t.visual_id [0-9]+$'
require_line '^OFFSET client_cold_t.depth [0-9]+$'
require_line '^OFFSET client_cold_t.colormap [0-9]+$'
require_line '^OFFSET client_cold_t.frame_colormap [0-9]+$'
require_line '^OFFSET client_cold_t.render_ctx [0-9]+$'
require_line '^OFFSET client_cold_t.icon_surface [0-9]+$'
require_line '^OFFSET client_cold_t.protocols [0-9]+$'
require_line '^OFFSET client_cold_t.strut [0-9]+$'
require_line '^OFFSET client_cold_t.pid [0-9]+$'

if grep -Eq '^OFFSET client_hot_t.visual_id [0-9]+$' <<<"$output"; then
  echo "unexpected hot visual_id offset found in layout dump output" >&2
  exit 1
fi

echo "test_client_layout_dump passed"
