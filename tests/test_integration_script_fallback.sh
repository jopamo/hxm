#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

fake_bin="$tmpdir/bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/Xvfb" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_XVFB_MARKER:-}" ]; then
  : >"$FAKE_XVFB_MARKER"
fi
trap 'exit 0' TERM INT
while true; do
  sleep 1
done
EOF

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

chmod +x "$fake_bin/Xvfb" "$fake_bin/hxm" "$fake_bin/integration_client"

stdout_log="$tmpdir/stdout.log"
stderr_log="$tmpdir/stderr.log"

PATH="$fake_bin:/usr/bin:/bin" \
  HXM_FORCE_DIRECT_XVFB=1 \
  HXM_BIN="$fake_bin/hxm" \
  INTEGRATION_CLIENT="$fake_bin/integration_client" \
  XVFB_DISPLAY=":123" \
  FAKE_XVFB_MARKER="$tmpdir/xvfb.started" \
  FAKE_HXM_MARKER="$tmpdir/hxm.started" \
  FAKE_CLIENT_MARKER="$tmpdir/client.started" \
  bash "$repo_root/scripts/test-integration.sh" >"$stdout_log" 2>"$stderr_log"

if [ ! -f "$tmpdir/xvfb.started" ]; then
  echo "expected direct Xvfb fallback to start Xvfb" >&2
  exit 1
fi

if [ ! -f "$tmpdir/hxm.started" ]; then
  echo "expected integration script to start hxm" >&2
  exit 1
fi

if [ ! -f "$tmpdir/client.started" ]; then
  echo "expected integration script to run integration_client" >&2
  exit 1
fi

if ! grep -q "direct Xvfb" "$stdout_log"; then
  echo "expected direct Xvfb fallback log line" >&2
  cat "$stdout_log" >&2
  exit 1
fi

echo "test_integration_script_fallback passed"
