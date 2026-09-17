#!/usr/bin/env python3
"""Turn a Phase 1 run's collected logs into propagation / reconstruction / connect numbers."""
import argparse, os, re, statistics, sys
from datetime import datetime

TS   = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)Z")
TIP  = re.compile(r"UpdateTip: new best=([0-9a-f]{64}) height=(\d+)")
RECON= re.compile(r"Successfully reconstructed block ([0-9a-f]{64}) with (\d+) txn prefilled, "
                  r"(\d+) txn from mempool \(incl at least (\d+) from extra pool\) and (\d+) txn requested")
CONN = re.compile(r"- Connect block: ([\d.]+)ms")

def ts(line):
    m = TS.match(line)
    if not m: return None
    s = m.group(1)
    d = datetime.strptime(s[:26].ljust(26, "0"), "%Y-%m-%dT%H:%M:%S.%f")
    return d.timestamp()

def parse(path):
    tips, recons, conns = {}, {}, []
    last_conn = None
    for line in open(path, errors="replace"):
        t = ts(line)
        if t is None: continue
        m = RECON.search(line)
        if m:
            recons[m.group(1)] = dict(prefilled=int(m.group(2)), mempool=int(m.group(3)),
                                      extra=int(m.group(4)), requested=int(m.group(5)))
        m = CONN.search(line)
        if m: last_conn = float(m.group(1))
        m = TIP.search(line)
        if m:
            tips[m.group(1)] = dict(t=t, height=int(m.group(2)), connect_ms=last_conn)
    return tips, recons

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--miner", default="eur")
    a = ap.parse_args()

    off = {}
    p = os.path.join(a.run, "offsets-before.txt")
    if os.path.exists(p):
        for line in open(p):
            f = line.split()
            if len(f) >= 3:
                try: off[f[0]] = float(f[2]) / 1000.0
                except ValueError: off[f[0]] = 0.0

    nodes = {}
    for fn in sorted(os.listdir(a.run)):
        if fn.startswith("debug-") and fn.endswith(".log"):
            alias = fn[6:-4]
            nodes[alias] = parse(os.path.join(a.run, fn))

    if a.miner not in nodes:
        print("miner %s not in run" % a.miner); sys.exit(1)

    miner_tips = nodes[a.miner][0]
    blocks = sorted(miner_tips, key=lambda h: miner_tips[h]["height"])
    if not blocks:
        print("no blocks in this run"); return

    print("blocks mined: %d (height %d..%d), nodes: %d"
          % (len(blocks), miner_tips[blocks[0]]["height"], miner_tips[blocks[-1]]["height"], len(nodes)))
    print()

    # --- propagation -------------------------------------------------------
    per_node, all_last = {}, []
    for h in blocks:
        base = miner_tips[h]["t"] + off.get(a.miner, 0.0)
        deltas = []
        for al, (tips, _) in nodes.items():
            if al == a.miner or h not in tips: continue
            d = (tips[h]["t"] + off.get(al, 0.0) - base) * 1000
            per_node.setdefault(al, []).append(d)
            deltas.append(d)
        if deltas: all_last.append(max(deltas))

    print("propagation from %s, clock-corrected (ms)" % a.miner)
    print("  %-5s %8s %8s %8s %8s %6s" % ("node", "median", "p90", "min", "max", "n"))
    for al in sorted(per_node, key=lambda x: statistics.median(per_node[x])):
        v = sorted(per_node[al])
        print("  %-5s %8.1f %8.1f %8.1f %8.1f %6d"
              % (al, statistics.median(v), v[int(len(v)*0.9)-1] if len(v) > 1 else v[0], v[0], v[-1], len(v)))
    if all_last:
        print("  full-mesh convergence: median %.0f ms, p90 %.0f ms, worst %.0f ms"
              % (statistics.median(all_last), sorted(all_last)[int(len(all_last)*0.9)-1], max(all_last)))
    print()

    # --- reconstruction ----------------------------------------------------
    tot = req = 0
    worst = []
    for al, (_, recons) in nodes.items():
        for h, r in recons.items():
            tot += 1
            if r["requested"] > 0:
                req += 1
                worst.append((r["requested"], al, h[:12], r["mempool"], r["prefilled"]))
    print("compact-block reconstruction: %d attempts, %d needed a round trip (%.1f%%)"
          % (tot, req, 100.0 * req / tot if tot else 0.0))
    for n, al, h, mp, pf in sorted(worst, reverse=True)[:6]:
        print("  %-5s block %s: %d txn requested (mempool %d, prefilled %d)" % (al, h, n, mp, pf))
    print()

    # --- connect cost ------------------------------------------------------
    print("ConnectBlock cost (ms)")
    print("  %-5s %8s %8s %8s %6s" % ("node", "median", "p90", "max", "n"))
    for al in sorted(nodes):
        v = sorted(x["connect_ms"] for x in nodes[al][0].values() if x["connect_ms"] is not None)
        if not v: continue
        print("  %-5s %8.2f %8.2f %8.2f %6d"
              % (al, statistics.median(v), v[int(len(v)*0.9)-1] if len(v) > 1 else v[0], v[-1], len(v)))

main()
