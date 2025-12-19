#!/bin/bash
set -e

DISPLAY_NUM=":98"
export DISPLAY=$DISPLAY_NUM

# Start Xvfb
echo "Starting Xvfb on $DISPLAY..."
Xvfb $DISPLAY -screen 0 1280x720x24 &
XVFB_PID=$!

sleep 2

# Start bbox
echo "Starting bbox..."
./build/bbox &
BBOX_PID=$!

sleep 2

# Run integration test
echo "Running integration client..."
./tests/integration_client

echo "Integration test complete."

# Cleanup
kill $BBOX_PID
kill $XVFB_PID
