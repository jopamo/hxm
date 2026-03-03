#!/usr/bin/env python3
"""
Guard cookie handler ownership rules.

Rules:
- Any function that matches the cookie handler signature
  (..., void* reply, xcb_generic_error_t* err) must not call free(reply/err).
- Canonical ownership contract must exist in include/cookie_jar.h.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


@dataclass
class Violation:
    path: pathlib.Path
    line: int
    function_name: str
    message: str


FUNC_RE = re.compile(
    r"(?P<name>[A-Za-z_]\w*)\s*"
    r"\([^;{}]*?\bvoid\s*\*\s*reply\s*,\s*xcb_generic_error_t\s*\*\s*err[^;{}]*\)\s*\{",
    re.MULTILINE | re.DOTALL,
)

FREE_REPLY_RE = re.compile(r"\bfree\s*\(\s*reply\s*\)")
FREE_ERR_RE = re.compile(r"\bfree\s*\(\s*err\s*\)")


def find_matching_brace(text: str, open_brace_idx: int) -> int:
    depth = 0
    i = open_brace_idx
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    escaped = False

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 1
        elif in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == "'":
                in_char = False
        else:
            if ch == "/" and nxt == "/":
                in_line_comment = True
                i += 1
            elif ch == "/" and nxt == "*":
                in_block_comment = True
                i += 1
            elif ch == '"':
                in_string = True
            elif ch == "'":
                in_char = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1

    return -1


def sanitize_for_pattern_scan(text: str) -> str:
    out: list[str] = []
    i = 0
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    escaped = False

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
                out.append("\n")
            else:
                out.append(" ")
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                out.append(" ")
                out.append(" ")
                in_block_comment = False
                i += 1
            else:
                out.append("\n" if ch == "\n" else " ")
        elif in_string:
            if escaped:
                escaped = False
                out.append(" ")
            elif ch == "\\":
                escaped = True
                out.append(" ")
            elif ch == '"':
                in_string = False
                out.append(" ")
            else:
                out.append("\n" if ch == "\n" else " ")
        elif in_char:
            if escaped:
                escaped = False
                out.append(" ")
            elif ch == "\\":
                escaped = True
                out.append(" ")
            elif ch == "'":
                in_char = False
                out.append(" ")
            else:
                out.append("\n" if ch == "\n" else " ")
        else:
            if ch == "/" and nxt == "/":
                out.append(" ")
                out.append(" ")
                in_line_comment = True
                i += 1
            elif ch == "/" and nxt == "*":
                out.append(" ")
                out.append(" ")
                in_block_comment = True
                i += 1
            elif ch == '"':
                in_string = True
                out.append(" ")
            elif ch == "'":
                in_char = True
                out.append(" ")
            else:
                out.append(ch)
        i += 1

    return "".join(out)


def line_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def collect_default_paths(repo_root: pathlib.Path) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for rel in ("src", "tests"):
        base = repo_root / rel
        if not base.is_dir():
            continue
        out.extend(sorted(base.rglob("*.c")))
    return out


def check_path(path: pathlib.Path) -> list[Violation]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    sanitized = sanitize_for_pattern_scan(text)
    violations: list[Violation] = []

    for match in FUNC_RE.finditer(text):
        fn_name = match.group("name")
        open_idx = text.find("{", match.start(), match.end())
        if open_idx < 0:
            continue
        close_idx = find_matching_brace(text, open_idx)
        if close_idx < 0:
            continue

        body = sanitized[open_idx + 1 : close_idx]
        body_start = open_idx + 1

        for m in FREE_REPLY_RE.finditer(body):
            off = body_start + m.start()
            violations.append(
                Violation(
                    path=path,
                    line=line_for_offset(text, off),
                    function_name=fn_name,
                    message="cookie handler must not free(reply); reply is borrowed",
                )
            )

        for m in FREE_ERR_RE.finditer(body):
            off = body_start + m.start()
            violations.append(
                Violation(
                    path=path,
                    line=line_for_offset(text, off),
                    function_name=fn_name,
                    message="cookie handler must not free(err); err is borrowed",
                )
            )

    return violations


def check_canonical_contract(repo_root: pathlib.Path) -> list[str]:
    header = repo_root / "include" / "cookie_jar.h"
    if not header.is_file():
        return [f"canonical contract header missing: {header}"]

    text = header.read_text(encoding="utf-8", errors="ignore")
    required = [
        "COOKIE_JAR_HANDLER_OWNERSHIP_CONTRACT",
        "reply/err are borrowed",
        "Handlers must not free() or retain reply/err",
    ]
    missing = [snippet for snippet in required if snippet not in text]
    if missing:
        return [f"canonical ownership contract drift in {header}: missing {snippet!r}" for snippet in missing]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description="Enforce cookie handler ownership contract.")
    parser.add_argument("--root", required=True, help="Repository root path")
    parser.add_argument(
        "--extra-path",
        action="append",
        default=[],
        help="Additional C source path to scan (can be repeated)",
    )
    parser.add_argument(
        "--skip-canonical-check",
        action="store_true",
        help="Skip canonical include/cookie_jar.h contract check",
    )
    args = parser.parse_args()

    repo_root = pathlib.Path(args.root).resolve()
    if not repo_root.is_dir():
        print(f"error: repo root not found: {repo_root}", file=sys.stderr)
        return 2

    paths = collect_default_paths(repo_root)
    for p in args.extra_path:
        extra = pathlib.Path(p).resolve()
        if not extra.is_file():
            print(f"error: --extra-path not found: {extra}", file=sys.stderr)
            return 2
        paths.append(extra)

    errors: list[str] = []
    if not args.skip_canonical_check:
        errors.extend(check_canonical_contract(repo_root))

    violations: list[Violation] = []
    for path in paths:
        if not path.is_file():
            continue
        violations.extend(check_path(path))

    if errors or violations:
        for err in errors:
            print(f"error: {err}", file=sys.stderr)
        for v in violations:
            print(
                f"error: {v.path}:{v.line}: {v.function_name}: {v.message}",
                file=sys.stderr,
            )
        return 1

    print("cookie handler ownership contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
