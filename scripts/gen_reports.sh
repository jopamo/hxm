#!/bin/bash
set -euo pipefail

REPO_ROOT="$(dirname "$(dirname "$(readlink -f "$0")")")"

BUILD_DIR="$REPO_ROOT/build-coverage"
REPORT_DIR="$REPO_ROOT/llm-report"

COVERAGE_OUT_DIR="$REPO_ROOT/coverage"
MARKDOWN_REPORT_PATH="$REPO_ROOT/REPORT.md"

MESON_LOG_DIR="$BUILD_DIR/meson-logs"
COVERAGE_TXT_PATH="$MESON_LOG_DIR/coverage.txt"
COVERAGE_HTML_DIR="$MESON_LOG_DIR/coveragereport"

echo "=== Setup directories ==="
mkdir -p "$REPORT_DIR"
mkdir -p "$REPORT_DIR/clang-tidy-db"
mkdir -p "$MESON_LOG_DIR"

echo "=== Configure coverage build directory ==="
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  meson setup "$BUILD_DIR" \
    -Db_coverage=true \
    -Dbuildtype=debug \
    -Doptimization=0
fi

echo "=== Build and test (with coverage) ==="
meson compile -C "$BUILD_DIR"
meson test -C "$BUILD_DIR"

echo "=== Generate coverage reports ==="
if command -v gcovr >/dev/null 2>&1; then
  rm -rf "$COVERAGE_HTML_DIR"
  mkdir -p "$COVERAGE_HTML_DIR"

  # HTML Report
  gcovr -r "$REPO_ROOT" \
    "$BUILD_DIR" \
    -e "$REPO_ROOT/subprojects" \
    --html --html-nested \
    --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
    -o "$COVERAGE_HTML_DIR/index.html"

  # Text Report
  gcovr -r "$REPO_ROOT" \
    "$BUILD_DIR" \
    -e "$REPO_ROOT/subprojects" \
    --txt \
    --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
    -o "$COVERAGE_TXT_PATH"

  echo "--- Copying HTML report to $COVERAGE_OUT_DIR ---"
  rm -rf "$COVERAGE_OUT_DIR"
  mkdir -p "$COVERAGE_OUT_DIR"
  cp -r "$COVERAGE_HTML_DIR/"* "$COVERAGE_OUT_DIR/"
else
  echo "Warning: gcovr not found, skipping HTML/text coverage generation"
  : > "$COVERAGE_TXT_PATH"
fi

echo "=== Generating ctags ==="
if command -v ctags >/dev/null 2>&1; then
  rm -f "$REPORT_DIR/ctags.txt"
  ctags -R -x -o "$REPORT_DIR/ctags.txt" src include tests
else
  echo "Warning: ctags not found, skipping"
  : > "$REPORT_DIR/ctags.txt"
fi

echo "=== Preparing compilation database for clang-tidy ==="
# Produce llm-report/clang-tidy-db/compile_commands.json (dedup + copy)
python3 "$REPO_ROOT/scripts/gen_llm_report.py" --prepare-compile-db

echo "=== Running clang-tidy ==="
if command -v clang-tidy >/dev/null 2>&1 && command -v rg >/dev/null 2>&1; then
  rg --files -g '*.c' src > "$REPORT_DIR/clang_tidy_files.txt"

  # Keep going even if clang-tidy finds issues
  clang-tidy \
    -p "$REPORT_DIR/clang-tidy-db" \
    -checks='-*,clang-analyzer-*' \
    --export-fixes="$REPORT_DIR/clang-tidy.yaml" \
    --quiet \
    @"$REPORT_DIR/clang_tidy_files.txt" || true
else
  echo "Warning: clang-tidy or rg not found, skipping"
  : > "$REPORT_DIR/clang-tidy.yaml"
fi

echo "=== Generating final JSON + Markdown report ==="
python3 "$REPO_ROOT/scripts/gen_llm_report.py"

echo ""
echo "✅ JSON report: $REPORT_DIR/report.json"
echo "✅ Markdown report: $MARKDOWN_REPORT_PATH"
if [ -f "$COVERAGE_OUT_DIR/index.html" ]; then
  echo "✅ HTML coverage: $COVERAGE_OUT_DIR/index.html"
  echo "   file://$COVERAGE_OUT_DIR/index.html"
fi
