#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "SKIP: xvfb-run not found" >&2
  exit 77
fi

hxm_bin=${HXM_BIN:-}
if [ -z "$hxm_bin" ]; then
  if [ -x "$repo_root/build/hxm" ]; then
    hxm_bin="$repo_root/build/hxm"
  elif [ -x "$repo_root/hxm" ]; then
    hxm_bin="$repo_root/hxm"
  else
    echo "hxm binary not found; set HXM_BIN" >&2
    exit 1
  fi
fi

integration_client=${INTEGRATION_CLIENT:-}
if [ -z "$integration_client" ]; then
  if [ -x "$repo_root/build/integration_client" ]; then
    integration_client="$repo_root/build/integration_client"
  elif [ -x "$repo_root/tests/integration_client" ]; then
    integration_client="$repo_root/tests/integration_client"
  else
    echo "integration_client not found; set INTEGRATION_CLIENT" >&2
    exit 1
  fi
fi

# Pass a longer timeout to the C client if TSAN is active.
timeout_ms=5000
if [[ -n "${TSAN_OPTIONS:-}" ]]; then
  timeout_ms=30000
fi
export HXM_TEST_TIMEOUT_MS="$timeout_ms"

wm_log="${repo_root}/build/hxm.log"
mkdir -p "$(dirname "$wm_log")"

echo "Running integration client with xvfb-run..."

xvfb-run -a -s "-screen 0 1280x720x24 +extension RANDR" bash -c "
  set -e
  \"$hxm_bin\" >\"$wm_log\" 2>&1 &
  HXM_PID=\$!
  sleep 2

  if ! \"$integration_client\"; then
    echo \"Integration test failed!\" >&2
    echo \"WM log tail:\" >&2
    tail -n 200 \"$wm_log\" >&2 || true
    kill \$HXM_PID
    exit 1
  fi

  kill \$HXM_PID
  wait \$HXM_PID || true
"

echo "Integration test complete."
