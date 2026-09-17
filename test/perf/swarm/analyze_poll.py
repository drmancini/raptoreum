#!/usr/bin/env python3
"""Relay delivery rate from mempool growth.

The miner offers no load, so every transaction in its mempool arrived over
relay: the slope of its mempool size against time is the delivery rate S.
Submitting nodes grow at (own offered rate + relay delivery), which is reported
too as a cross-check.
"""
import argparse, sys
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument("--poll", required=True)
ap.add_argument("--miner", default="cor")
ap.add_argument("--offered", type=float, default=1500.0)
a = ap.parse_args()

series = defaultdict(list)
for line in open(a.poll):
    f = line.split()
    if len(f) == 3:
        try: series[f[1]].append((float(f[0]), int(f[2])))
        except ValueError: pass
if not series:
    print("no samples"); sys.exit(0)

def slope(pts):
    # fit only the rising section: drop samples after the max, which are post-run
    if len(pts) < 4: return None, 0
    pts = sorted(pts)
    peak = max(range(len(pts)), key=lambda i: pts[i][1])
    pts = pts[:peak + 1]
    if len(pts) < 4: return None, 0
    t0 = pts[0][0]
    xs = [p[0] - t0 for p in pts]; ys = [p[1] for p in pts]
    n = len(xs); sx = sum(xs); sy = sum(ys)
    sxy = sum(x * y for x, y in zip(xs, ys)); sxx = sum(x * x for x in xs)
    d = n * sxx - sx * sx
    if d == 0: return None, 0
    return (n * sxy - sx * sy) / d, n

print("  %-5s %12s %10s %8s  %s" % ("node", "growth tx/s", "peak", "n", "note"))
S = None
for al in sorted(series):
    m, n = slope(series[al])
    if m is None: continue
    peak = max(p[1] for p in series[al])
    note = "MINER: pure relay delivery" if al == a.miner else ""
    if al == a.miner: S = m
    print("  %-5s %12.0f %10d %8d  %s" % (al, m, peak, n, note))

if S:
    print()
    print("  relay delivery S = %.0f tx/s against %.0f offered (%.0f%%)"
          % (S, a.offered, 100.0 * S / a.offered))
    print("  deficit accumulating at %.0f tx/s" % (a.offered - S))
