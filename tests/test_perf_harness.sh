#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
runner="$repo_root/scripts/run-perf-harness.sh"

if [[ ! -x "$runner" ]]; then
  echo "perf harness runner missing or not executable: $runner" >&2
  exit 1
fi

output=$($runner --no-perf --iters 1000)

require_output_line() {
  local pattern=$1
  if ! grep -Eq "$pattern" <<<"$output"; then
    echo "missing expected output line matching: $pattern" >&2
    exit 1
  fi
}

require_output_line '^SCENARIO focus_cycle OPS [0-9]+$'
require_output_line '^SCENARIO stacking_ops OPS [0-9]+$'
require_output_line '^SCENARIO move_resize OPS [0-9]+$'
require_output_line '^SCENARIO flush_loops OPS [0-9]+$'

echo "test_perf_harness passed"
