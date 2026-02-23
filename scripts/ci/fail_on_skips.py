#!/usr/bin/env python3

import argparse
import json
import pathlib
import sys


def parse_testlog(path: pathlib.Path) -> list[dict]:
    raw = path.read_text(encoding="utf-8").strip()
    if not raw:
        return []

    if raw[0] == "[":
        payload = json.loads(raw)
        if not isinstance(payload, list):
            raise ValueError("Expected top-level JSON array")
        return [entry for entry in payload if isinstance(entry, dict)]

    entries: list[dict] = []
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        payload = json.loads(line)
        if isinstance(payload, dict):
            entries.append(payload)
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail if Meson testlog.json contains skipped tests."
    )
    parser.add_argument("testlog", help="Path to meson-logs/testlog.json")
    args = parser.parse_args()

    testlog_path = pathlib.Path(args.testlog)
    if not testlog_path.is_file():
        print(f"error: test log not found: {testlog_path}", file=sys.stderr)
        return 2

    try:
        entries = parse_testlog(testlog_path)
    except Exception as exc:  # pragma: no cover - exercised by shell harness
        print(f"error: failed to parse {testlog_path}: {exc}", file=sys.stderr)
        return 2

    skipped = [
        entry.get("name", "<unnamed>")
        for entry in entries
        if entry.get("result") == "SKIP"
    ]

    if skipped:
        print("error: skipped tests are not allowed in this lane:", file=sys.stderr)
        for test_name in skipped:
            print(f"  - {test_name}", file=sys.stderr)
        return 1

    print("No skipped tests detected.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
