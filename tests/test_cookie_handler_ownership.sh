#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
checker="$repo_root/scripts/ci/check_cookie_handler_ownership.py"

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

python3 "$checker" --root "$repo_root" >/dev/null

cat >"$tmpdir/violating_handler.c" <<'EOF'
#include <stdlib.h>
#include <xcb/xcb.h>

struct server;
struct cookie_slot;

static void violating_handler(struct server* s, const struct cookie_slot* slot, void* reply, xcb_generic_error_t* err) {
  (void)s;
  (void)slot;
  free(reply);
  free(err);
}
EOF

if python3 "$checker" --root "$repo_root" --extra-path "$tmpdir/violating_handler.c" >/dev/null 2>&1; then
  echo "expected ownership checker to fail on a handler that frees borrowed reply/err" >&2
  exit 1
fi

echo "test_cookie_handler_ownership passed"
