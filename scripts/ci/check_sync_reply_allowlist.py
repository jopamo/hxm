#!/usr/bin/env python3
"""
Guardrail for synchronous xcb *_reply usage in runtime interaction paths.

Policy:
- scan a fixed set of runtime files
- count synchronous xcb *_reply callsites by (file, symbol)
- require every count to be explicitly allowlisted with rationale + bound
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import Counter


DEFAULT_TRACKED_FILES = (
    "src/menu.c",
    "src/wm.c",
    "src/wm_desktop.c",
    "src/wm_input_keys.c",
)

REPLY_RE = re.compile(r"\b(xcb_[A-Za-z0-9_]+_reply)\s*\(")
IGNORED_REPLY_SYMBOLS = {"xcb_poll_for_reply"}
EXEMPTION_ANNOTATION_TOKEN = "SYNC_REPLY_EXEMPT"


def fail(message: str) -> int:
    print(f"error: {message}", file=sys.stderr)
    return 1


def has_exemption_annotation(lines: list[str], zero_based_line: int) -> bool:
    start = max(0, zero_based_line - 4)
    end = min(len(lines) - 1, zero_based_line)
    for idx in range(start, end + 1):
        if EXEMPTION_ANNOTATION_TOKEN in lines[idx]:
            return True
    return False


def parse_allowlist(
    allowlist_path: pathlib.Path, tracked_files: tuple[str, ...]
) -> tuple[Counter[tuple[str, str]], int]:
    if not allowlist_path.is_file():
        raise ValueError(f"allowlist file not found: {allowlist_path}")

    expected: Counter[tuple[str, str]] = Counter()
    tracked_set = set(tracked_files)

    for lineno, raw in enumerate(
        allowlist_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue

        parts = [part.strip() for part in line.split("|", 3)]
        if len(parts) != 4:
            raise ValueError(
                f"{allowlist_path}:{lineno}: expected 4 fields "
                f"(path|symbol|count|rationale), got: {raw!r}"
            )

        rel_path, symbol, count_text, rationale = parts
        if rel_path not in tracked_set:
            raise ValueError(
                f"{allowlist_path}:{lineno}: path {rel_path!r} is not in tracked files"
            )
        if not symbol.startswith("xcb_") or not symbol.endswith("_reply"):
            raise ValueError(
                f"{allowlist_path}:{lineno}: invalid reply symbol {symbol!r}"
            )
        if symbol.endswith("_from_reply"):
            raise ValueError(
                f"{allowlist_path}:{lineno}: parser helpers ending in _from_reply "
                "must not appear in this allowlist"
            )

        try:
            count = int(count_text)
        except ValueError as exc:
            raise ValueError(
                f"{allowlist_path}:{lineno}: invalid count {count_text!r}"
            ) from exc

        if count <= 0:
            raise ValueError(
                f"{allowlist_path}:{lineno}: count must be positive, got {count}"
            )
        if "bound<=" not in rationale:
            raise ValueError(
                f"{allowlist_path}:{lineno}: rationale must include measurable bound "
                "(missing 'bound<=')"
            )

        expected[(rel_path, symbol)] += count

    return expected, sum(expected.values())


def collect_actual(
    repo_root: pathlib.Path, tracked_files: tuple[str, ...]
) -> tuple[Counter[tuple[str, str]], int, list[str]]:
    actual: Counter[tuple[str, str]] = Counter()
    total = 0
    missing_annotations: list[str] = []

    for rel_path in tracked_files:
        path = repo_root / rel_path
        if not path.is_file():
            raise ValueError(f"tracked file not found: {path}")

        lines = path.read_text(encoding="utf-8").splitlines()
        for lineno, raw in enumerate(lines, start=1):
            line = raw.strip()
            if line.startswith("//"):
                continue

            for match in REPLY_RE.finditer(raw):
                symbol = match.group(1)
                if symbol in IGNORED_REPLY_SYMBOLS:
                    continue
                if symbol.endswith("_from_reply"):
                    continue
                actual[(rel_path, symbol)] += 1
                total += 1
                if not has_exemption_annotation(lines, lineno - 1):
                    missing_annotations.append(f"{rel_path}:{lineno}: missing {EXEMPTION_ANNOTATION_TOKEN} for {symbol}")

    return actual, total, missing_annotations


def describe(counter: Counter[tuple[str, str]]) -> list[str]:
    out: list[str] = []
    for (path, symbol), count in sorted(counter.items()):
        out.append(f"{path}|{symbol}|{count}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Enforce synchronous xcb *_reply allowlist for runtime interaction files."
        )
    )
    parser.add_argument(
        "--root",
        default=None,
        help="Repository root (default: auto-detected from script location).",
    )
    parser.add_argument(
        "--allowlist",
        default=None,
        help="Allowlist file path (default: scripts/ci/sync_reply_allowlist.txt).",
    )
    parser.add_argument(
        "--tracked-file",
        action="append",
        default=None,
        help=(
            "Relative runtime file path to scan. May be repeated. "
            "Defaults to built-in runtime file set."
        ),
    )
    args = parser.parse_args()

    if args.root is None:
        repo_root = pathlib.Path(__file__).resolve().parents[2]
    else:
        repo_root = pathlib.Path(args.root).resolve()

    allowlist = (
        pathlib.Path(args.allowlist).resolve()
        if args.allowlist is not None
        else (repo_root / "scripts" / "ci" / "sync_reply_allowlist.txt")
    )
    tracked_files = (
        tuple(dict.fromkeys(args.tracked_file))
        if args.tracked_file
        else DEFAULT_TRACKED_FILES
    )

    try:
        expected, expected_total = parse_allowlist(allowlist, tracked_files)
        actual, actual_total, missing_annotations = collect_actual(
            repo_root, tracked_files
        )
    except ValueError as exc:
        return fail(str(exc))

    if missing_annotations:
        print(
            "error: sync reply exemption annotation missing\n"
            f"required token: {EXEMPTION_ANNOTATION_TOKEN}\n"
            "callsites:\n"
            + "\n".join(missing_annotations),
            file=sys.stderr,
        )
        return 1

    if expected != actual:
        expected_lines = "\n".join(describe(expected))
        actual_lines = "\n".join(describe(actual))
        print(
            "error: sync reply allowlist mismatch\n"
            f"tracked files: {', '.join(tracked_files)}\n"
            f"allowlist: {allowlist}\n"
            "expected (allowlist):\n"
            f"{expected_lines if expected_lines else '<none>'}\n"
            "actual (source scan):\n"
            f"{actual_lines if actual_lines else '<none>'}",
            file=sys.stderr,
        )
        return 1

    print(
        f"sync reply allowlist OK ({actual_total} callsites across "
        f"{len(tracked_files)} files)"
    )
    if expected_total != actual_total:
        return fail(
            "internal error: equal counters but different totals "
            f"(expected={expected_total} actual={actual_total})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
