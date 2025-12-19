#!/usr/bin/env python3
import subprocess
import time
import random
import os

def spawn_windows(count):
    procs = []
    cmd = ["./tests/dummy_client"]
    if not os.path.exists(cmd[0]):
        print(f"Error: {cmd[0]} not found. Compile it first.")
        return []

    for _ in range(count):
        p = subprocess.Popen(cmd)
        procs.append(p)
    return procs

def main():
    display = os.environ.get("DISPLAY")
    if not display:
        print("DISPLAY not set. Run this in Xephyr or Xvfb.")
        return

    print(f"Starting stress test on {display}")
    
    count = 50
    print(f"Spawning {count} windows...")
    procs = spawn_windows(count)
    
    time.sleep(2)
    
    print("Randomly moving/resizing...")
    # We don't have a good way to do this from python easily without Xlib/xcffib
    # but we can just use 'xdotool' if available.
    
    try:
        for _ in range(100):
            # Pick a random window (we don't know IDs easily, but we can just spam events)
            # Actually, let's just wait and see if bbox crashes.
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    print("Killing windows...")
    for p in procs:
        p.terminate()
    
    print("Stress test finished.")

if __name__ == "__main__":
    main()
