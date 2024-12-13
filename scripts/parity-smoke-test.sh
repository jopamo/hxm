#!/bin/bash
set -e

# Setup config
mkdir -p ~/.config/hxm
cat > ~/.config/hxm/hxm.conf <<EOF
rule = class:Special -> desktop:3
EOF

DISPLAY_NUM=":99"
export DISPLAY=$DISPLAY_NUM

# Start Xvfb
echo "Starting Xvfb on $DISPLAY..."
Xvfb $DISPLAY -screen 0 1280x720x24 &
XVFB_PID=$!

# Ensure cleanup on exit
trap "kill $XVFB_PID 2>/dev/null || true" EXIT

sleep 1

# Start hxm
echo "Starting hxm..."
./build-meson-asan/hxm &
HXM_PID=$!
trap "kill $HXM_PID $XVFB_PID 2>/dev/null || true" EXIT

sleep 1

# Run parity client
echo "Running parity client..."
./build-meson-asan/parity_client

echo "Parity smoke test complete."
