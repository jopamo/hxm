#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

build_dir=${BUILD_DIR:-$repo_root/build}
harness_bin=${PERF_HARNESS_BIN:-$build_dir/perf_harness}
perf_bin=${PERF_BIN:-perf}

iters=100000
clients=1024
scenario=all
use_perf=1

die_usage() {
  cat >&2 <<USAGE
usage: $0 [--no-perf] [--iters N] [--clients N] [--scenario all|focus_cycle|stacking_ops|move_resize|flush_loops]
USAGE
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-perf)
      use_perf=0
      shift
      ;;
    --iters)
      [[ $# -ge 2 ]] || die_usage
      iters=$2
      shift 2
      ;;
    --clients)
      [[ $# -ge 2 ]] || die_usage
      clients=$2
      shift 2
      ;;
    --scenario)
      [[ $# -ge 2 ]] || die_usage
      scenario=$2
      shift 2
      ;;
    *)
      die_usage
      ;;
  esac
done

if [[ ! -x "$harness_bin" ]]; then
  if ! meson compile -C "$build_dir" perf_harness >/dev/null 2>&1; then
    meson compile -C "$build_dir" >/dev/null
  fi
fi

if [[ ! -x "$harness_bin" ]]; then
  echo "perf harness binary missing after build attempt: $harness_bin" >&2
  exit 1
fi

if [[ "$use_perf" -eq 0 ]]; then
  exec "$harness_bin" --scenario "$scenario" --iters "$iters" --clients "$clients"
fi

if ! command -v "$perf_bin" >/dev/null 2>&1; then
  echo "perf binary not found: $perf_bin (use --no-perf to run harness without counters)" >&2
  exit 1
fi

events='cycles,instructions,cache-misses,LLC-load-misses,branches,branch-misses'
scenarios=(focus_cycle stacking_ops move_resize flush_loops)
if [[ "$scenario" != "all" ]]; then
  scenarios=("$scenario")
fi

for name in "${scenarios[@]}"; do
  scenario_out=$(mktemp)
  perf_out=$(mktemp)
  cleanup() {
    rm -f "$scenario_out" "$perf_out"
  }
  trap cleanup EXIT

  if ! "$perf_bin" stat -x, -e "$events" -- "$harness_bin" --scenario "$name" --iters "$iters" --clients "$clients" >"$scenario_out" 2>"$perf_out"; then
    cat "$perf_out" >&2
    echo "perf collection failed for scenario $name (use --no-perf if hardware counters are unavailable)." >&2
    exit 1
  fi

  cat "$scenario_out"
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" == \#* ]] && continue
    printf 'PERF %s %s\n' "$name" "$line"
  done <"$perf_out"

  cleanup
  trap - EXIT
done
