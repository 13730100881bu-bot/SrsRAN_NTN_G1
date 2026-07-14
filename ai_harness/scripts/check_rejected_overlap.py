#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path


def run_git(args):
    return subprocess.check_output(["git"] + args, text=True)


def normalize(path):
    return path.strip().replace("\\", "/")


def changed_files(base):
    changed = {normalize(line) for line in run_git(["diff", "--name-only", base, "--"]).splitlines() if line.strip()}
    status = run_git(["status", "--porcelain", "--untracked-files=all"])
    for line in status.splitlines():
        if len(line) < 4:
            continue
        path = line[3:].strip()
        if " -> " in path:
            path = path.split(" -> ", 1)[1].strip()
        if path:
            changed.add(normalize(path))
    return sorted(changed)


def rejected_paths(path):
    p = Path(path)
    if not p.exists():
        print(f"Rejected/quarantined path file not found: {path}", file=sys.stderr)
        sys.exit(2)

    paths = []
    for line in p.read_text(encoding="utf-8").splitlines():
        value = normalize(line)
        if value and not value.startswith("#"):
            paths.append(value)
    return paths


def parse_markdown_paths(path, section_heading):
    p = Path(path)
    if not p.exists():
        return []

    paths = []
    in_section = False
    for line in p.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if value.startswith("## "):
            in_section = value[3:].strip().lower() == section_heading.lower()
            continue
        if not in_section:
            continue
        if value.startswith("# "):
            break
        if value.startswith("- `") and value.endswith("`"):
            path_value = normalize(value[3:-1])
            if path_value and path_value.lower() != "none":
                paths.append(path_value)
    return paths


def is_rejected(path, rejected):
    for item in rejected:
        normalized = item.rstrip("/")
        if path == normalized:
            return True
        if item.endswith("/") and path.startswith(item):
            return True
    return False


def is_task_exception(path, exceptions):
    for item in exceptions:
        normalized = item.rstrip("/")
        if path == normalized:
            return True
        if item.endswith("/") and path.startswith(item):
            return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="ai/cucp-harness-base", help="Base commit or ref")
    parser.add_argument(
        "--rejected-file",
        default="ai_harness/audit/rejected_or_quarantined_paths.txt",
        help="File containing rejected/quarantined repository paths",
    )
    parser.add_argument(
        "--task-file",
        default=None,
        help="Optional task card. Paths under 'Allowed task exception paths' are ignored for this overlap check.",
    )
    args = parser.parse_args()

    rejected = rejected_paths(args.rejected_file)
    if not rejected:
        print(f"No rejected/quarantined paths found in {args.rejected_file}", file=sys.stderr)
        return 2

    task_exceptions = parse_markdown_paths(args.task_file, "Allowed task exception paths") if args.task_file else []

    violations = [
        path
        for path in changed_files(args.base)
        if is_rejected(path, rejected) and not is_task_exception(path, task_exceptions)
    ]
    if violations:
        print("Rejected/quarantined overlap check failed.\n")
        print("Changed files that overlap rejected/quarantined paths:")
        for path in violations:
            print(f"  - {path}")
        print("\nAction required:")
        print("  1. Revert these files, or")
        print("  2. Move the path from rejected/quarantined to accepted after human review.")
        return 1

    print("Rejected/quarantined overlap check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
