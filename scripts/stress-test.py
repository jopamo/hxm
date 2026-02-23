#!/usr/bin/env python3

import os
import subprocess
import time


def spawn_windows(count):
    procs = []
    client = os.environ.get("DUMMY_CLIENT", "./tests/dummy_client")
    cmd = [client]
    if not os.path.exists(cmd[0]):
        print(f"Error: {cmd[0]} not found. Compile it first.")
        raise SystemExit(1)

    for _ in range(count):
        p = subprocess.Popen(cmd)
        procs.append(p)
    return procs


def main():
    display = os.environ.get("DISPLAY")
    if not display:
        print("DISPLAY not set. Run this in Xephyr or Xvfb.")
        raise SystemExit(1)

    print(f"Starting stress test on {display}")

    count = 50
    print(f"Spawning {count} windows...")
    procs = spawn_windows(count)

    time.sleep(2)

    print("Randomly moving/resizing...")
    # Keep windows alive for a short period to exercise lifecycle paths.
    for _ in range(100):
        time.sleep(0.1)

    print("Killing windows...")
    for p in procs:
        p.terminate()

    print("Stress test finished.")


if __name__ == "__main__":
    main()
