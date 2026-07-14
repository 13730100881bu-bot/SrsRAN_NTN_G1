#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path


FORBIDDEN_MARKERS = [
    "/flexible_o_du/",
    "/o_du/",
    "/du/",
    "/du_high/",
    "/du_low/",
    "/mac/",
    "/scheduler/",
    "/phy/",
    "/lower_phy/",
    "/radio/",
    "/ru/",
    "/rf/",
    "/prach/",
    "/harq/",
    "/gis/",
]


def run_git(args):
    return subprocess.check_output(["git"] + args, text=True)


def changed_files(base):
    changed = {line.strip() for line in run_git(["diff", "--name-only", base, "--"]).splitlines() if line.strip()}
    status = run_git(["status", "--porcelain", "--untracked-files=all"])
    for line in status.splitlines():
        if len(line) < 4:
            continue
        path = line[3:].strip()
        if " -> " in path:
            path = path.split(" -> ", 1)[1].strip()
        if path:
            changed.add(path.replace("\\", "/"))
    return sorted(changed)


def parse_markdown_paths(path, section_heading=None):
    paths = []
    p = Path(path)
    if not p.exists():
        return paths

    in_section = section_heading is None
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if section_heading is not None:
            if line.startswith("## "):
                heading = line[3:].strip().lower()
                in_section = heading == section_heading.lower()
                continue
            if line.startswith("# ") and in_section:
                break
            if not in_section:
                continue
        if line.startswith("- `") and line.endswith("`"):
            value = line[3:-1].strip()
            if value and value.lower() != "none":
                paths.append(value)
        elif line.startswith("- "):
            value = line[2:].strip()
            if value.lower() != "none":
                continue
    return paths


def is_allowed(path, allowed_prefixes):
    for prefix in allowed_prefixes:
        normalized = prefix.rstrip("/")
        if path == normalized:
            return True
        if prefix.endswith("/") and path.startswith(prefix):
            return True
        if path.startswith(prefix):
            return True
    return False


def has_forbidden_marker(path):
    normalized = "/" + path
    return any(marker in normalized for marker in FORBIDDEN_MARKERS)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="ai/cucp-harness-base", help="Base commit or ref")
    parser.add_argument(
        "--allowed-file",
        default="ai_harness/context/allowed_paths.md",
        help="Markdown file containing - `path/` allowed prefixes",
    )
    parser.add_argument(
        "--task-file",
        default=None,
        help="Optional task card. Paths under 'Allowed task exception paths' are added to the allowed list.",
    )
    args = parser.parse_args()

    allowed = parse_markdown_paths(args.allowed_file)
    if args.task_file:
        allowed += parse_markdown_paths(args.task_file, "Allowed task exception paths")

    if not allowed:
        print(f"No allowed paths found in {args.allowed_file}", file=sys.stderr)
        return 2

    changed = changed_files(args.base)

    violations = []
    warnings = []

    for path in changed:
        if is_allowed(path, allowed):
            continue

        if has_forbidden_marker(path):
            violations.append(path)
        else:
            warnings.append(path)

    if violations or warnings:
        print("CU-CP path guard failed.\n")

        if violations:
            print("Likely forbidden non-CU-CP changes:")
            for p in violations:
                print(f"  - {p}")

        if warnings:
            print("\nChanged files outside allowed prefixes:")
            for p in warnings:
                print(f"  - {p}")

        print("\nAction required:")
        print("  1. Revert these files, or")
        print("  2. Add an explicit task-level exception after human review.")
        return 1

    print("CU-CP path guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
