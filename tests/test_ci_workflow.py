#!/usr/bin/env python3
"""
Regression guard for the dedicated full-suite Xvfb CI lane.
"""

from __future__ import annotations

import pathlib
import re
import sys


def fail(message: str) -> int:
    print(f"error: {message}", file=sys.stderr)
    return 1


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    if not workflow.is_file():
        return fail(f"workflow file not found: {workflow}")

    text = workflow.read_text(encoding="utf-8")
    if "pull_request:" not in text:
        return fail("ci workflow must run on pull_request events")

    full_suite_job_match = re.search(
        r"(?ms)^  full_suite_xvfb:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text
    )
    if full_suite_job_match is None:
        return fail("missing full_suite_xvfb job in .github/workflows/ci.yml")

    job = full_suite_job_match.group(1)
    expected_snippets = [
        "name: full-suite-xvfb",
        "xargs -r dnf -y install < scripts/deps-fedora.txt",
        'meson setup "build-full-suite"',
        'meson compile -C "build-full-suite"',
        'meson test -C "build-full-suite" --print-errorlogs',
        'python3 scripts/ci/fail_on_skips.py "build-full-suite/meson-logs/testlog.json"',
    ]

    for snippet in expected_snippets:
        if snippet not in job:
            return fail(f"full_suite_xvfb job is missing required step: {snippet}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
