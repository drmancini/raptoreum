#!/usr/bin/env python3
"""Find numbers restated in living documents instead of cited from findings.md.

One number, one home: a measured value lives in perf-results.md and is named in
findings.md; a constant is named in findings.md. Living docs cite the ID. A value
copied into prose is how two documents come to disagree, which this folder has
already done four times.

This is a lint, not a gate on correctness. It reports lines in living documents
that carry a number with a unit and no findings ID on the same line. Some of
those are legitimate — a worked example, an arithmetic derivation shown on
purpose — so the output is a budget to shrink, not a list of bugs. Run it with
--max to fail once a baseline is agreed.

    ./check-numbers.py                 # report
    ./check-numbers.py --max 120       # exit 1 if over budget
    ./check-numbers.py --show 20       # print the worst offenders
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Living documents only. findings.md and perf-constants.md are where values are
# allowed to live; the log is append-only history; archive/ is frozen by
# definition; README is navigation.
LIVING = [
    "transaction-decoupling.md",
    "build-plan.md",
    "upstream-ledger.md",
    "platform/architecture-decisions.md",
]

# A document that owns a subject must be allowed to state it. build-plan owns
# the schedule, so effort in days, weeks and months is its content, not a
# restated finding — counting those was the lint marking a document down for
# doing its job.
OWNED = {
    "build-plan.md": re.compile(r"\b\d[\d,]*(?:\.\d+)?\s*(?:d|w|weeks?|days?|months?|mo)\b"),
}

UNIT = (r"(?:tx/s|locks/s|recoveries/s|msg/s|ms|µs|us|ns|s|[kKMGT]i?B|B/s|B\b|sigops|%|"
        r"weeks?|days?|months?|hours?|years?|tx|blocks?|entries|inputs?|outputs?|nodes?|×)")
NUMBER = re.compile(r"\b\d[\d,]*(?:\.\d+)?\s*" + UNIT + r"\b")
# Hyphenated so they cannot collide with the security review's attack labels (A1, B1,
# C1...), the accumulator example's D0, or "R = N * D^-1".
FINDING_ID = re.compile(r"\b[KFXDR]-\d+[a-z]?\b")
# A file:line or file:symbol anchor is a citation, not a restated value.
ANCHOR = re.compile(r"`[\w/.\-]+\.(?:cpp|h|py|sh|md):[\w\d]+")
SECTION = re.compile(r"§[\d.]+[A-Za-z]?")
# Spelled-out quantities hide from a digit-based regex, and the plain-language
# companion is written almost entirely in them.
SPELLED = re.compile(r"\b(?:thousand|million|billion|terabytes?|gigabytes?|megabytes?|hundreds?|dozens?|twice|tenfold)\b", re.I)


def scan(path: Path, name: str = ""):
    """Yield (lineno, line) for lines that restate a number without a citation."""
    in_fence = False
    in_comment = False
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        # The lifecycle header is metadata, not prose.
        if line.startswith("<!--"):
            in_comment = True
        if in_comment:
            if "-->" in line:
                in_comment = False
            continue
        if not NUMBER.search(line) and not SPELLED.search(line):
            continue
        if FINDING_ID.search(line):
            continue
        owned = OWNED.get(name)
        if owned is not None:
            nums = [m.group(0) for m in NUMBER.finditer(line)]
            if nums and all(owned.match(n) for n in nums) and not SPELLED.search(line):
                continue
        # A bare section or code anchor beside a number is usually a pointer to
        # where the number is established, which is the behaviour we want.
        if ANCHOR.search(line) and not NUMBER.search(ANCHOR.sub("", line)):
            continue
        if SECTION.search(line) and not NUMBER.search(SECTION.sub("", line)):
            continue
        yield n, line


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=None, help="fail above this total")
    ap.add_argument("--show", type=int, default=0, help="print this many offenders per file")
    a = ap.parse_args()

    total = 0
    print(f"{'document':<40} {'restated':>8}")
    print("-" * 50)
    for name in LIVING:
        p = HERE / name
        if not p.exists():
            print(f"{name:<40} {'MISSING':>8}")
            continue
        hits = list(scan(p, name))
        total += len(hits)
        print(f"{name:<40} {len(hits):>8}")
        for n, line in hits[: a.show]:
            print(f"    {n:>5}: {line[:110]}")
    print("-" * 50)
    print(f"{'total':<40} {total:>8}")

    if a.max is not None and total > a.max:
        print(f"\nover budget: {total} > {a.max}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
