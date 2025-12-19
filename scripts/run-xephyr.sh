#!/bin/bash
# Launch bbox in Xephyr for testing

XEPHYR_DISPLAY=":1"
SCREEN_SIZE="1280x720"

# Check if Xephyr is installed
if ! command -v Xephyr &> /dev/null; then
    echo "Xephyr could not be found. Please install it."
    exit 1
fi

# Kill old Xephyr if running
pkill -f "Xephyr $XEPHYR_DISPLAY"

# Start Xephyr
Xephyr $XEPHYR_DISPLAY -screen $SCREEN_SIZE -ac -br -noreset &
XEPHYR_PID=$!

sleep 1

# Start bbox
DISPLAY=$XEPHYR_DISPLAY ./build/bbox &
BBOX_PID=$!

echo "bbox started in Xephyr on $XEPHYR_DISPLAY (PID $BBOX_PID)"

# Launch a test client if available
# Dummy client launch disabled
echo "Run 'DISPLAY=$XEPHYR_DISPLAY your-app' to test."
trap "kill $BBOX_PID $XEPHYR_PID" SIGINT SIGTERM

echo "Press Ctrl+C to stop"

# Wait for Ctrl+C
wait
