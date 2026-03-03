#!/usr/bin/env python3
"""
Regression guard for the sanitizer CI workflow shape.
"""

from __future__ import annotations

import pathlib
import re
import sys
from typing import Iterable


def fail(message: str) -> int:
    print(f"error: {message}", file=sys.stderr)
    return 1


SOURCE_PATTERNS = [
    re.compile(r"\bpthread_(create|join|mutex|rwlock|cond|key)_\b"),
    re.compile(r"\bstd::thread\b"),
    re.compile(r"#\s*include\s*<thread>"),
    re.compile(r"\bthrd_(create|join|sleep|yield)\b"),
    re.compile(r"\bg_thread_(new|join|init)\b"),
    re.compile(r"\bGThread\b"),
    re.compile(r"\buv_thread_(create|join)\b"),
    re.compile(r"\bdispatch_(async|queue)\b"),
    re.compile(r"\bomp_(parallel|set_num_threads)\b"),
]

MESON_PATTERNS = [
    re.compile(r"dependency\('threads'\)"),
    re.compile(r"find_library\('pthread'\)"),
    re.compile(r"\b-pthread\b"),
]

SOURCE_SUFFIXES = {
    ".c",
    ".h",
    ".cc",
    ".cpp",
    ".cxx",
    ".hh",
    ".hpp",
    ".hxx",
}


def iter_source_files(repo_root: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(repo_root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(repo_root)
        if any(part == ".git" for part in rel.parts):
            continue
        if any(part.startswith("build") for part in rel.parts):
            continue
        if path.suffix in SOURCE_SUFFIXES:
            yield path


def iter_meson_files(repo_root: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(repo_root.rglob("meson.build")):
        rel = path.relative_to(repo_root)
        if any(part == ".git" for part in rel.parts):
            continue
        if any(part.startswith("build") for part in rel.parts):
            continue
        yield path


def first_pattern_match(paths: Iterable[pathlib.Path], patterns: list[re.Pattern[str]]) -> str | None:
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for lineno, line in enumerate(text.splitlines(), start=1):
            for pattern in patterns:
                if pattern.search(line):
                    return f"{path}:{lineno} {line.strip()}"
    return None


def thread_usage_evidence(repo_root: pathlib.Path) -> tuple[bool, str]:
    source_hit = first_pattern_match(iter_source_files(repo_root), SOURCE_PATTERNS)
    if source_hit is not None:
        return True, source_hit

    meson_hit = first_pattern_match(iter_meson_files(repo_root), MESON_PATTERNS)
    if meson_hit is not None:
        return True, meson_hit

    return False, "no matches in source or meson build definitions"


def ensure_snippets(text: str, snippets: list[str]) -> str | None:
    for snippet in snippets:
        if snippet not in text:
            return snippet
    return None


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    if not workflow.is_file():
        return fail(f"workflow file not found: {workflow}")

    text = workflow.read_text(encoding="utf-8")
    threads_used, evidence = thread_usage_evidence(repo_root)

    if re.search(r"(?m)^  full_suite_xvfb:\s*$", text):
        return fail("legacy full_suite_xvfb job must not be present in ci.yml")
    if "continue-on-error: true" in text:
        return fail("ci.yml must not contain continue-on-error: true")

    required_snippets = [
        "name: ci",
        "on:\n  push:\n  pull_request:",
        "jobs:\n  san:",
        "runs-on: ubuntu-latest",
        "container: fedora:rawhide",
        "name: san-${{ matrix.name }}",
        "fail-fast: false",
        "- name: asan",
        'meson_sanitize: "address,undefined"',
        'extra_cflags: "-fsanitize-address-use-after-scope"',
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:fast_unwind_on_malloc=0:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:detect_odr_violation=1",
        "- name: ubsan",
        'meson_sanitize: "undefined"',
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
        "CC: clang",
        "CXX: clang++",
        "LLVM_SYMBOLIZER_PATH: /usr/bin/llvm-symbolizer",
        "ASAN_SYMBOLIZER_PATH: /usr/bin/llvm-symbolizer",
        "CFLAGS: -O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-sanitize-merge",
        "CXXFLAGS: -O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-sanitize-merge",
        "LDFLAGS: -fuse-ld=lld",
        "- uses: actions/checkout@v4",
        "if [ \"${{ matrix.name }}\" = \"tsan\" ]; then",
        "meson test -C \"build-${{ matrix.name }}\" --print-errorlogs --num-processes 1",
        "meson test -C \"build-${{ matrix.name }}\" --print-errorlogs",
    ]
    missing = ensure_snippets(text, required_snippets)
    if missing is not None:
        return fail(f"san job missing required workflow snippet: {missing}")

    tsan_present = re.search(r"(?m)^\s*- name: tsan\s*$", text) is not None
    if threads_used and not tsan_present:
        return fail(f"threads are used but tsan matrix entry is missing ({evidence})")
    if not threads_used and tsan_present:
        return fail("threads are not used but tsan matrix entry is present")
    if tsan_present:
        tsan_required_snippets = [
            'meson_sanitize: "thread"',
            "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1",
        ]
        tsan_missing = ensure_snippets(text, tsan_required_snippets)
        if tsan_missing is not None:
            return fail(f"tsan matrix entry missing required snippet: {tsan_missing}")

    job_names = re.findall(r"(?m)^  ([A-Za-z0-9_-]+):\n", text.split("jobs:\n", 1)[1])
    if job_names != ["san"]:
        return fail(f"ci.yml must define exactly one job named san; found {job_names}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
