#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
checker="$repo_root/scripts/ci/fail_on_skips.py"

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

cat >"$tmpdir/no_skips.json" <<'EOF'
{"name":"hxm:unit_a","result":"OK"}
{"name":"hxm:unit_b","result":"FAIL"}
EOF

python3 "$checker" "$tmpdir/no_skips.json" >/dev/null

cat >"$tmpdir/has_skip_lines.json" <<'EOF'
{"name":"hxm:ewmh_xvfb","result":"SKIP"}
{"name":"hxm:unit_a","result":"OK"}
EOF

if python3 "$checker" "$tmpdir/has_skip_lines.json" >/dev/null 2>&1; then
  echo "expected checker to fail for line-delimited SKIP entries" >&2
  exit 1
fi

cat >"$tmpdir/has_skip_array.json" <<'EOF'
[
  {"name":"hxm:conky_xvfb","result":"SKIP"},
  {"name":"hxm:unit_b","result":"OK"}
]
EOF

if python3 "$checker" "$tmpdir/has_skip_array.json" >/dev/null 2>&1; then
  echo "expected checker to fail for JSON-array SKIP entries" >&2
  exit 1
fi

echo "test_fail_on_skips passed"
