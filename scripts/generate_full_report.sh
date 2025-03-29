#!/bin/bash
set -e

REPO_ROOT="$(dirname "$(dirname "$(readlink -f "$0")")")"
REPORT_DIR="$REPO_ROOT/llm-report"
BUILD_DIR="$REPO_ROOT/build-coverage"

echo "=== Setup directories ==="
mkdir -p "$REPORT_DIR"
mkdir -p "$REPORT_DIR/clang-tidy-db"

echo "=== Building and testing (with coverage) ==="
if [ ! -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" -Db_coverage=true
fi
meson compile -C "$BUILD_DIR"
meson test -C "$BUILD_DIR"

echo "=== Generating ctags ==="
if command -v ctags &> /dev/null; then
    rm -f "$REPORT_DIR/ctags.txt"
    ctags -R -x -o "$REPORT_DIR/ctags.txt" src include tests
else
    echo "Warning: ctags not found, skipping."
    touch "$REPORT_DIR/ctags.txt"
fi

echo "=== Generating clang-tidy report ==="
if command -v clang-tidy &> /dev/null && command -v rg &> /dev/null; then
    # Generate file list
    rg --files -g '*.c' src > "$REPORT_DIR/clang_tidy_files.txt"
    
    # Run the python script once to prepare the compilation database
    echo "Preparing compilation database..."
    python3 "$REPO_ROOT/scripts/gen_llm_report.py"
    
    # Run clang-tidy
    # Note: We use || true because clang-tidy might return non-zero on warnings/errors but we want to proceed
    echo "Running clang-tidy..."
    clang-tidy -p "$REPORT_DIR/clang-tidy-db" -checks='-*,clang-analyzer-*' --export-fixes="$REPORT_DIR/clang-tidy.yaml" --quiet @"$REPORT_DIR/clang_tidy_files.txt" || true
else
    echo "Warning: clang-tidy or rg not found, skipping."
    touch "$REPORT_DIR/clang-tidy.yaml"
fi

echo "=== Generating final JSON report ==="
python3 "$REPO_ROOT/scripts/gen_llm_report.py"

echo "Done. Report at $REPORT_DIR/report.json"
