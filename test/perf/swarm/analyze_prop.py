#!/usr/bin/env python3
"""Per-transaction propagation latency across the swarm.

Origin time comes from the submitting node's submit.py log (RPC submission does
not go through net_processing, so it emits no AcceptToMemoryPool line locally).
Arrival at every other node comes from that node's AcceptToMemoryPool line under
-debug=mempool, which carries a microsecond timestamp.

Reports the distribution of "time until a transaction is held by all N nodes" --
the quantity a commitment block's reconstruction rate depends on, and which is
independent of whatever block interval is chosen.
"""
import argparse, os, re, statistics, sys
from datetime import datetime, timezone

TS = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)Z")
ACC = re.compile(r"AcceptToMemoryPool: peer=\d+: accepted ([0-9a-f]{64})")

def ts(line):
    m = TS.match(line)
    if not m: return None
    s = m.group(1)[:26].ljust(26, "0")
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%f").replace(tzinfo=timezone.utc).timestamp()

ap = argparse.ArgumentParser()
ap.add_argument("--run", required=True)
a = ap.parse_args()

off = {}
p = os.path.join(a.run, "offsets-before.txt")
if os.path.exists(p):
    for line in open(p):
        f = line.split()
        if len(f) >= 3:
            try: off[f[0]] = float(f[2]) / 1000.0
            except ValueError: off[f[0]] = 0.0

# origin: txid -> (alias, wall seconds)
origin = {}
for fn in sorted(os.listdir(a.run)):
    if not fn.startswith("sent-") or not fn.endswith(".log"): continue
    al = fn[5:-4]
    for line in open(os.path.join(a.run, fn), errors="replace"):
        f = line.split()
        if len(f) >= 4 and f[3] == "ok" and len(f[2]) == 64:
            origin[f[2]] = (al, int(f[1]) / 1e9 + off.get(al, 0.0))

# arrivals: txid -> {alias: corrected wall seconds}
arr = {}
nodes = set()
for fn in sorted(os.listdir(a.run)):
    if not fn.startswith("debug-") or not fn.endswith(".log"): continue
    al = fn[6:-4]; nodes.add(al)
    o = off.get(al, 0.0)
    for line in open(os.path.join(a.run, fn), errors="replace"):
        m = ACC.search(line)
        if not m: continue
        t = ts(line)
        if t is None: continue
        arr.setdefault(m.group(1), {})[al] = t + o

N = len(nodes)
print("nodes with logs: %d | transactions submitted: %d | with arrivals: %d"
      % (N, len(origin), len(arr)))
if not origin or not arr:
    print("no data"); sys.exit(0)

full, partial, per_hop = [], 0, []
for txid, (oal, ot) in origin.items():
    seen = arr.get(txid, {})
    others = {k: v for k, v in seen.items() if k != oal}
    for al, t in others.items():
        per_hop.append((t - ot) * 1000)
    if len(others) >= N - 1:
        full.append((max(others.values()) - ot) * 1000)
    else:
        partial += 1

print()
print("per-hop propagation (origin -> one node), ms")
ph = sorted(x for x in per_hop if x > -1000)
if ph:
    for q in (50, 90, 99):
        print("  p%-3d %9.0f" % (q, ph[int(len(ph) * q / 100) - 1]))
    print("  max  %9.0f    n=%d" % (ph[-1], len(ph)))

print()
print("time until a transaction is held by ALL %d nodes, ms" % N)
f = sorted(full)
if f:
    for q in (50, 90, 99):
        print("  p%-3d %9.0f" % (q, f[int(len(f) * q / 100) - 1]))
    print("  max  %9.0f" % f[-1])
print("  reached all nodes: %d of %d (%.1f%%)   never did: %d"
      % (len(full), len(origin), 100.0 * len(full) / len(origin), partial))

print()
print("fraction network-wide within a given budget (what a commitment block sees)")
for budget in (1, 2, 5, 10, 30, 60, 120):
    n = sum(1 for x in full if x <= budget * 1000)
    print("  within %3ds: %6.2f%% of all submitted" % (budget, 100.0 * n / max(len(origin), 1)))
