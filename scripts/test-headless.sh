#!/bin/bash
set -e

DISPLAY_NUM=":99"
export DISPLAY=$DISPLAY_NUM

# Start Xvfb
echo "Starting Xvfb on $DISPLAY..."
Xvfb $DISPLAY -screen 0 1280x720x24 &
XVFB_PID=$!

sleep 2

# Start hxm
echo "Starting hxm..."
./build/hxm &
HXM_PID=$!

sleep 2

# Run stress test
echo "Running stress test..."
./scripts/stress-test.py

echo "Stress test complete."

# Cleanup
kill $HXM_PID
kill $XVFB_PID
