#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
script="$repo_root/scripts/test-integration-real-x11.sh"

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

fake_bin="$tmpdir/bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/hxm" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_HXM_MARKER:-}" ]; then
  : >"$FAKE_HXM_MARKER"
fi
trap 'exit 0' TERM INT
while true; do
  sleep 1
done
EOF

cat >"$fake_bin/integration_client" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_CLIENT_MARKER:-}" ]; then
  : >"$FAKE_CLIENT_MARKER"
fi
exit 0
EOF

chmod +x "$fake_bin/hxm" "$fake_bin/integration_client"

missing_display_stderr="$tmpdir/missing_display.stderr"
set +e
PATH="$fake_bin:/usr/bin:/bin" \
  HXM_BIN="$fake_bin/hxm" \
  INTEGRATION_CLIENT="$fake_bin/integration_client" \
  bash "$script" >/dev/null 2>"$missing_display_stderr"
missing_display_status=$?
set -e

if [ "$missing_display_status" -ne 77 ]; then
  echo "expected missing DISPLAY to return 77, got $missing_display_status" >&2
  cat "$missing_display_stderr" >&2
  exit 1
fi

if ! grep -q "DISPLAY is not set" "$missing_display_stderr"; then
  echo "expected missing DISPLAY error message" >&2
  cat "$missing_display_stderr" >&2
  exit 1
fi

stdout_log="$tmpdir/stdout.log"
stderr_log="$tmpdir/stderr.log"

PATH="$fake_bin:/usr/bin:/bin" \
  DISPLAY=":123" \
  HXM_BIN="$fake_bin/hxm" \
  INTEGRATION_CLIENT="$fake_bin/integration_client" \
  FAKE_HXM_MARKER="$tmpdir/hxm.started" \
  FAKE_CLIENT_MARKER="$tmpdir/client.started" \
  bash "$script" >"$stdout_log" 2>"$stderr_log"

if [ ! -f "$tmpdir/hxm.started" ]; then
  echo "expected script to start hxm" >&2
  exit 1
fi

if [ ! -f "$tmpdir/client.started" ]; then
  echo "expected script to run integration_client" >&2
  exit 1
fi

if ! grep -q "real X11" "$stdout_log"; then
  echo "expected real X11 status line" >&2
  cat "$stdout_log" >&2
  exit 1
fi

echo "test_integration_real_x11_script passed"
