# LLM Report

This directory contains a combined index intended for LLM consumption:

- `REPORT.md`: A human and AI-friendly Markdown summary of the project status.
- `ctags.txt`: symbol index from Universal Ctags (xref format).
- `clang-tidy.yaml`: LLVM clang-tidy diagnostics (clang-analyzer checks).
- `clang-tidy-db/compile_commands.json`: de-duplicated compilation database.
- `report.json`: merged JSON with coverage, symbols, and clang-tidy summary.

## Regenerate

1) Symbols (ctags):

```sh
ctags -R -x -o llm-report/ctags.txt src include tests
```

2) clang-tidy file list:

```sh
rg --files -g '*.c' src > llm-report/clang_tidy_files.txt
```

3) De-duplicate compile commands (uses `build-coverage/compile_commands.json`):

```sh
python3 - <<'PY'
import json
from pathlib import Path
src = Path('build-coverage/compile_commands.json')
cmds = json.loads(src.read_text())
seen = set()
unique = []
for entry in cmds:
    file = entry.get('file')
    if not file or file in seen:
        continue
    seen.add(file)
    unique.append(entry)
Path('llm-report/clang-tidy-db').mkdir(parents=True, exist_ok=True)
Path('llm-report/clang-tidy-db/compile_commands.json').write_text(json.dumps(unique, indent=2))
PY
```

4) clang-tidy (LLVM):

```sh
clang-tidy -p llm-report/clang-tidy-db -checks='-*,clang-analyzer-*' --export-fixes=llm-report/clang-tidy.yaml --quiet @llm-report/clang_tidy_files.txt
```

5) Generate merged JSON:

```sh
python3 scripts/gen_llm_report.py
```
