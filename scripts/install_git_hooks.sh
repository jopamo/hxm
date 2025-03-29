#!/bin/bash
set -e

REPO_ROOT="$(dirname "$(dirname "$(readlink -f "$0")")")"
HOOK_PATH="$REPO_ROOT/.git/hooks/pre-commit"

echo "Installing pre-commit hook to $HOOK_PATH..."

cat > "$HOOK_PATH" << 'EOF'
#!/bin/bash
# Auto-generated pre-commit hook to update LLM report

REPO_ROOT="$(git rev-parse --show-toplevel)"
echo "Updating REPORT.md..."

# Run the generation script
"$REPO_ROOT/scripts/generate_full_report.sh"

# Add the generated report to the commit
git add "$REPO_ROOT/REPORT.md"
git add "$REPO_ROOT/TODO.md"
git add "$REPO_ROOT/llm-report/report.json"

echo "Report updated and staged."
EOF

chmod +x "$HOOK_PATH"
echo "Hook installed successfully."
