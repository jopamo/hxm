#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
header="$repo_root/include/client.h"

require_line() {
  local pattern=$1
  if ! grep -Eq "$pattern" "$header"; then
    echo "missing expected header line matching: $pattern" >&2
    exit 1
  fi
}

require_line '^ \* Hot storage contract for client_hot_t:$'
require_line '^#define CLIENT_HOT_SIZE_GUARD_BYTES 584u$'
require_line '^HXM_STATIC_ASSERT\(sizeof\(client_hot_t\) <= CLIENT_HOT_SIZE_GUARD_BYTES,$'

echo "test_client_hot_guard passed"
