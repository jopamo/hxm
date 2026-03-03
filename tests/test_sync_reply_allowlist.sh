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

# Negative check: missing SYNC_REPLY_EXEMPT annotation must fail.
annot_root="$tmpdir/annot_root"
mkdir -p "$annot_root/src"
cat >"$annot_root/src/sync_missing_annotation.c" <<'EOF'
void synthetic_missing_annotation(void* conn, int cookie) {
  (void)xcb_get_property_reply(conn, cookie, 0);
}
EOF
annot_allowlist="$tmpdir/annot_allowlist.txt"
cat >"$annot_allowlist" <<'EOF'
src/sync_missing_annotation.c|xcb_get_property_reply|1|synthetic test reply exemption bound<=1 per helper call
EOF
if python3 "$checker" --root "$annot_root" --allowlist "$annot_allowlist" --tracked-file src/sync_missing_annotation.c >/dev/null 2>&1; then
  echo "expected allowlist checker to fail for missing SYNC_REPLY_EXEMPT annotation" >&2
  exit 1
fi

# Positive check: callsite-level SYNC_REPLY_EXEMPT annotation passes.
cat >"$annot_root/src/sync_with_annotation.c" <<'EOF'
void synthetic_with_annotation(void* conn, int cookie) {
  // SYNC_REPLY_EXEMPT: synthetic startup probe bound<=1 for test coverage.
  (void)xcb_get_property_reply(conn, cookie, 0);
}
EOF
annot_allowlist_ok="$tmpdir/annot_allowlist_ok.txt"
cat >"$annot_allowlist_ok" <<'EOF'
src/sync_with_annotation.c|xcb_get_property_reply|1|synthetic test reply exemption bound<=1 per helper call
EOF
python3 "$checker" --root "$annot_root" --allowlist "$annot_allowlist_ok" --tracked-file src/sync_with_annotation.c >/dev/null

echo "test_sync_reply_allowlist passed"
