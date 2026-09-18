#!/usr/bin/env python3
"""Verify source citations that carry their own symbol, and count those that don't.

A line number is not an address. Any commit touching a cited file moves every
anchor below it — the acceptance probe alone moved everything past
validation.cpp:111 by 5 lines and past 1082 by 19 — so a citation written as
`validation.cpp:419` cannot be checked by a machine and rots silently.

A citation written as `validation.cpp:MAX_STANDARD_TX_SIZE` can be. This script
resolves those against the tree and reports the ones that no longer exist or
have become ambiguous. Unkeyed `file:line` citations are counted as
unverifiable, which is the migration backlog.

**Auto-rewriting unkeyed citations was tried and abandoned.** Inferring the
symbol from surrounding prose picks *a* nearby symbol rather than *the* one the
citation refers to: it proposed moving `validation.cpp:3026` to `:5485` and
`:1541` to `:122`, and for `scriptExecutionCache` it chose the declaration over
the insert site the sentence was about. A heuristic that repoints a citation
with confidence is worse than a stale number, because staleness is at least
visible. So: keyed citations are verified, unkeyed ones are counted, and nothing
is rewritten.

    ./check-anchors.py             # verify keyed, count unkeyed
    ./check-anchors.py --list      # also list the unkeyed backlog
    ./check-anchors.py --quiet     # summary only, for CI

Citation forms accepted:
    `validation.cpp:MAX_STANDARD_TX_SIZE`         a symbol
    `validation.cpp:scriptExecutionCache.insert`  a specific use, for a symbol
                                                  that appears in several places
    `llmq/quorums_chainlocks.cpp:TrySignChainTip` a path-qualified file
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

DOCS = Path(__file__).resolve().parent
SRC = DOCS.parent / "src"
TARGETS = ["findings.md", "transaction-decoupling.md", "build-plan.md", "perf-constants.md"]

FILE = r"(?:[\w\-]+/)*[\w\-]+\.(?:cpp|h|hpp)"
KEYED = re.compile(r"`(" + FILE + r"):([A-Za-z_][\w:.]*)`")
UNKEYED = re.compile(r"`(" + FILE + r"):(\d+)(?:[-–]\d+)?`")


def find_file(name: str):
    p = SRC / name
    if p.exists():
        return p
    hits = list(SRC.rglob(Path(name).name))
    return hits[0] if len(hits) == 1 else None


def sites(path: Path, key: str):
    """Lines where `key` appears, ignoring comment-only lines."""
    out = []
    for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if key not in line:
            continue
        if line.strip().startswith(("//", "*", "/*")):
            continue
        out.append(i)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="list the unkeyed backlog")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    tally = Counter()
    problems, unkeyed = [], []

    for doc in TARGETS:
        dp = DOCS / doc
        if not dp.exists():
            continue
        text = dp.read_text(encoding="utf-8")
        for fname, num in UNKEYED.findall(text):
            tally["unkeyed (unverifiable)"] += 1
            unkeyed.append(f"{doc}: {fname}:{num}")
        for fname, key in KEYED.findall(text):
            path = find_file(fname)
            if path is None:
                tally["file missing"] += 1
                problems.append(f"{doc}: {fname} — no such file under src/")
                continue
            found = sites(path, key)
            if not found:
                tally["symbol gone"] += 1
                problems.append(f"{doc}: {fname}:{key} — not found; renamed or removed")
            elif len(found) > 4:
                tally["ambiguous"] += 1
                problems.append(f"{doc}: {fname}:{key} — {len(found)} occurrences; "
                                f"key it more precisely, e.g. {key}.insert")
            else:
                tally["verified"] += 1
                if not a.quiet:
                    print(f"  ok  {doc}: {fname}:{key} -> line {', '.join(map(str, found))}")

    for p in problems:
        print(f"  !!  {p}")
    if a.list:
        print("\nunkeyed backlog (each needs its symbol, not a new number):")
        for u in unkeyed:
            print(f"      {u}")

    print("\n" + "  ".join(f"{k}: {v}" for k, v in sorted(tally.items())))
    return 1 if (tally["symbol gone"] or tally["file missing"]) else 0


if __name__ == "__main__":
    sys.exit(main())
