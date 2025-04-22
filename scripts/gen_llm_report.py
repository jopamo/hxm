#!/usr/bin/env python3
import argparse
import json
import re
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path


def parse_coverage(path: Path):
    if not path.exists():
        return 0, []
    lines = path.read_text().splitlines()
    files = []
    total = 0
    for line in lines:
        if not line or line.startswith("-") or line.startswith("File"):
            continue
        if line.startswith("TOTAL"):
            m = re.match(r"^TOTAL\s+(\d+)\s+(\d+)\s+(\d+)%", line)
            if m:
                total = int(m.group(3))
            continue
        m = re.match(r"^(\S+)\s+(\d+)\s+(\d+)\s+(\d+)%\s*(.*)$", line)
        if not m:
            continue
        path_str, total_lines, exec_lines, cover, missing = m.groups()
        files.append(
            {
                "path": path_str,
                "cover_percent": int(cover),
                "missing": missing.strip(),
            }
        )
    return total, files


def parse_ctags_xref(path: Path):
    if not path.exists():
        return [], Counter()
    symbols = []
    kinds = Counter()
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        parts = line.split(None, 4)
        if len(parts) < 4:
            continue
        name, kind, line_no, file_path = parts[:4]
        signature = parts[4].strip() if len(parts) > 4 else ""
        sym = {
            "name": name,
            "kind": kind,
            "line": int(line_no),
            "file": file_path,
        }
        if signature:
            sym["signature"] = signature
        symbols.append(sym)
        kinds[kind] += 1
    return symbols, kinds


def parse_clang_tidy_yaml(path: Path):
    if not path.exists():
        return 0, {}, []
    diag_counts = Counter()
    items = []

    current_diag = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if "DiagnosticName:" in line:
            name = line.split(":", 1)[1].strip()
            diag_counts[name] += 1
            current_diag = {"name": name}
            items.append(current_diag)
        elif current_diag is not None:
            if line.startswith("Message:") and "message" not in current_diag:
                m = re.search(r"Message:\s*(.*)", line)
                if m:
                    msg = m.group(1).strip()
                    if (msg.startswith("'" ) and msg.endswith("'")) or (
                        msg.startswith('"') and msg.endswith('"')
                    ):
                        msg = msg[1:-1]
                    current_diag["message"] = msg.replace("''", "'")
            elif line.startswith("FilePath:") and "file" not in current_diag:
                m = re.search(r"FilePath:\s*(.*)", line)
                if m:
                    current_diag["file"] = m.group(1).strip().strip("'" ).strip('"')
            elif line.startswith("FileOffset:") and "offset" not in current_diag:
                m = re.search(r"FileOffset:\s*(\d+)", line)
                if m:
                    current_diag["offset"] = int(m.group(1))

    return len(items), dict(diag_counts), items


def process_compile_commands(src_path: Path, dst_path: Path):
    if not src_path.exists():
        return []

    try:
        cmds = json.loads(src_path.read_text())
    except json.JSONDecodeError:
        return []

    seen = set()
    unique = []
    for entry in cmds:
        file = entry.get("file")
        if not file or file in seen:
            continue
        seen.add(file)
        unique.append(entry)

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(json.dumps(unique, indent=2))
    return unique


def generate_markdown_report(report, out_path: Path):
    lines = [
        "# LLM Context Report",
        "",
        f"Generated at: {report['metadata']['generated_at']}",
        "",
        "## 1. Project Overview",
        "",
        f"- **Root**: `{report['metadata']['repo_root']}`",
        f"- **Total Symbols**: {report['symbols']['total']}",
        f"- **Total Coverage**: {report['coverage']['total_percent']}%",
        f"- **Clang-Tidy Issues**: {report['clang_tidy']['diagnostic_count']}",
        "",
        "## 2. Code Coverage",
        "",
        "| File | Coverage | Missing Lines |",
        "|---|---|---|",
    ]

    sorted_files = sorted(report["coverage"]["files"], key=lambda x: x["cover_percent"])
    fully_covered_count = 0
    for f in sorted_files:
        if f["cover_percent"] == 100:
            fully_covered_count += 1
            continue

        path = f["path"]
        cover = f["cover_percent"]
        missing = f["missing"] if f["missing"] else "-"
        if len(missing) > 50:
            missing = missing[:47] + "..."
        lines.append(f"| `{path}` | {cover}% | {missing} |")

    if fully_covered_count > 0:
        lines.append(f"\n*{fully_covered_count} files with 100% coverage are hidden*")

    lines.extend([
        "",
        "## 3. Clang-Tidy Diagnostics",
        "",
        "### Summary by Type",
        "",
    ])

    for name, count in report["clang_tidy"]["diagnostics_by_name"].items():
        lines.append(f"- **{name}**: {count}")

    if report["clang_tidy"]["items"]:
        lines.extend(["", "### Top Issues", ""])
        for item in report["clang_tidy"]["items"][:20]:
            file = item.get("file", "unknown")
            msg = item.get("message", "No message")
            offset = item.get("offset", 0)
            lines.append(f"- **{file}**:{offset}: {msg}")

        if len(report["clang_tidy"]["items"]) > 20:
            lines.append(f"\n*(...and {len(report['clang_tidy']['items']) - 20} more)*")

    lines.extend(["", "## 4. Symbol Statistics", ""])

    for kind, count in report["symbols"]["by_kind"].items():
        lines.append(f"- **{kind}**: {count}")

    out_path.write_text("\n".join(lines) + "\n")


def build_report(repo_root: Path):
    report_dir = repo_root / "llm-report"
    coverage_path = repo_root / "build-coverage" / "meson-logs" / "coverage.txt"
    ctags_path = report_dir / "ctags.txt"
    clang_tidy_path = report_dir / "clang-tidy.yaml"

    compile_db_src = repo_root / "build-coverage" / "compile_commands.json"
    compile_db_dst = report_dir / "clang-tidy-db" / "compile_commands.json"

    total_cover, files = parse_coverage(coverage_path)
    symbols, kind_counts = parse_ctags_xref(ctags_path)
    tidy_total, tidy_by_name, tidy_items = parse_clang_tidy_yaml(clang_tidy_path)
    unique_cmds = process_compile_commands(compile_db_src, compile_db_dst)

    report = {
        "metadata": {
            "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "repo_root": str(repo_root),
            "coverage_path": str(coverage_path) if coverage_path.exists() else None,
            "ctags_path": str(ctags_path) if ctags_path.exists() else None,
            "clang_tidy_path": str(clang_tidy_path) if clang_tidy_path.exists() else None,
            "compile_commands_path": str(compile_db_dst) if compile_db_dst.exists() else None,
        },
        "coverage": {"total_percent": total_cover, "files": files},
        "symbols": {"total": len(symbols), "by_kind": dict(kind_counts), "items": symbols},
        "clang_tidy": {"diagnostic_count": tidy_total, "diagnostics_by_name": tidy_by_name, "items": tidy_items},
        "compilation": {"unique_files_count": len(unique_cmds), "commands": unique_cmds},
    }

    out_json = report_dir / "report.json"
    report_dir.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2))
    generate_markdown_report(report, repo_root / "REPORT.md")
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--prepare-compile-db",
        action="store_true",
        help="Only dedupe and copy compile_commands.json into llm-report/clang-tidy-db",
    )
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    report_dir = repo_root / "llm-report"

    if args.prepare_compile_db:
        compile_db_src = repo_root / "build-coverage" / "compile_commands.json"
        compile_db_dst = report_dir / "clang-tidy-db" / "compile_commands.json"
        process_compile_commands(compile_db_src, compile_db_dst)
        return

    build_report(repo_root)


if __name__ == "__main__":
    main()