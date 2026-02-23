#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
checker="$repo_root/scripts/ci/check_sync_reply_allowlist.py"
allowlist="$repo_root/scripts/ci/sync_reply_allowlist.txt"

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

python3 "$checker" --root "$repo_root" --allowlist "$allowlist" >/dev/null

# Negative check: missing an allowlisted entry must fail.
missing="$tmpdir/missing_allowlist.txt"
grep -v '^src/wm_desktop.c|xcb_get_property_reply|1|' "$allowlist" >"$missing"
if python3 "$checker" --root "$repo_root" --allowlist "$missing" >/dev/null 2>&1; then
  echo "expected allowlist checker to fail for missing entry" >&2
  exit 1
fi

# Negative check: stale extra entry must fail.
stale="$tmpdir/stale_allowlist.txt"
cp "$allowlist" "$stale"
echo 'src/wm_input_keys.c|xcb_get_property_reply|1|synthetic stale entry bound<=0 for negative test' >>"$stale"
if python3 "$checker" --root "$repo_root" --allowlist "$stale" >/dev/null 2>&1; then
  echo "expected allowlist checker to fail for stale extra entry" >&2
  exit 1
fi

echo "test_sync_reply_allowlist passed"
