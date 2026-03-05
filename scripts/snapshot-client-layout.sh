#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
build_dir=${BUILD_DIR:-$repo_root/build}
layout_bin=${CLIENT_LAYOUT_DUMP_BIN:-$build_dir/client_layout_dump}

usage() {
  echo "usage: $0 [output-file]" >&2
}

if [[ $# -gt 1 ]]; then
  usage
  exit 2
fi

if [[ ! -x "$layout_bin" ]]; then
  meson compile -C "$build_dir" client_layout_dump >/dev/null
fi

if [[ ! -x "$layout_bin" ]]; then
  echo "layout dump binary missing after build attempt: $layout_bin" >&2
  exit 1
fi

snapshot_commit=$(git -C "$repo_root" rev-parse HEAD)
snapshot_time_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)

emit_snapshot() {
  printf 'SNAPSHOT_COMMIT %s\n' "$snapshot_commit"
  printf 'SNAPSHOT_TIME_UTC %s\n' "$snapshot_time_utc"
  "$layout_bin"
}

if [[ $# -eq 1 ]]; then
  output_file=$1
  output_dir=$(dirname "$output_file")
  mkdir -p "$output_dir"

  tmpfile=$(mktemp "$output_file.tmp.XXXXXX")
  trap 'rm -f "$tmpfile"' EXIT
  emit_snapshot >"$tmpfile"
  mv "$tmpfile" "$output_file"
  trap - EXIT
else
  emit_snapshot
fi
