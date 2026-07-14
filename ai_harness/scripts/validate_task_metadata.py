#!/usr/bin/env python3

import argparse
import sys
from pathlib import Path


REQUIRED_HEADINGS = [
    "Goal",
    "Read first",
    "Accepted local contracts",
    "In scope",
    "Out of scope",
    "Allowed edit paths",
    "Allowed task exception paths",
    "Required behavior",
    "Required tests",
    "Validation",
    "Done means",
]

FORBIDDEN_TERMS = [
    "O-DU",
    "flexible_o_du",
    "DU",
    "MAC",
    "HARQ",
    "TA scheduler",
    "PRACH",
    "PHY",
    "RU",
    "RF",
    "ZMQ",
    "GIS",
]


def section_body(text, heading):
    marker = f"## {heading}"
    start = text.find(marker)
    if start == -1:
        return ""
    start = text.find("\n", start)
    if start == -1:
        return ""
    next_heading = text.find("\n## ", start + 1)
    if next_heading == -1:
        return text[start + 1 :]
    return text[start + 1 : next_heading]


def validate(path):
    p = Path(path)
    errors = []
    if not p.exists():
        return [f"{path}: file does not exist"]

    text = p.read_text(encoding="utf-8")
    lower = text.lower()

    for heading in REQUIRED_HEADINGS:
        if f"## {heading}".lower() not in lower:
            errors.append(f"{path}: missing section '## {heading}'")

    if "cu-cp only" not in lower:
        errors.append(f"{path}: missing explicit 'CU-CP only' contract")

    out_of_scope = section_body(text, "Out of scope")
    for term in FORBIDDEN_TERMS:
        if term.lower() not in out_of_scope.lower():
            errors.append(f"{path}: out-of-scope section does not mention '{term}'")

    validation = section_body(text, "Validation")
    if "guard" not in validation.lower() and "run_task_validation" not in validation.lower():
        errors.append(f"{path}: validation section does not reference required validation")

    exceptions = section_body(text, "Allowed task exception paths")
    if not exceptions.strip():
        errors.append(f"{path}: allowed exceptions section is empty; use '- None' when no exception is needed")

    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("task_files", nargs="+", help="Task card markdown files to validate")
    args = parser.parse_args()

    all_errors = []
    for path in args.task_files:
        all_errors.extend(validate(path))

    if all_errors:
        print("Task metadata validation failed.\n")
        for error in all_errors:
            print(f"  - {error}")
        return 1

    print("Task metadata validation passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
