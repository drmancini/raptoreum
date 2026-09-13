#!/usr/bin/env python3
"""Sample the node once a second for the duration of a run.

With no mining and no eviction the mempool's growth per second is the accepted
rate, so the headline number of stage A needs no instrumented build.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

from fanout import rpc  # noqa: E402

CLK = os.sysconf("SC_CLK_TCK")


def proc_cpu(pid):
    try:
        f = open("/proc/%d/stat" % pid).read().split()
        return (int(f[13]) + int(f[14])) / CLK, int(open("/proc/%d/statm" % pid).read().split()[1]) * 4096
    except OSError:
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--wallet", default="perf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--interval", type=float, default=1.0)
    a = ap.parse_args()

    node = rpc(a.datadir, wallet=a.wallet)
    pid = int(open(os.path.join(a.datadir, "regtest", "raptoreumd.pid")).read().strip())

    prev_size = None
    prev_cpu = None
    rows = []
    t_end = time.time() + a.seconds
    with open(a.out, "w") as f:
        f.write("ts,mempool_txs,mempool_bytes,mempool_usage,accepted_per_s,peers,"
                "bytes_recv,bytes_sent,node_cpu_pct,node_rss\n")
        while time.time() < t_end:
            t = time.time()
            mi = node.getmempoolinfo()
            ni = node.getnettotals()
            peers = len(node.getpeerinfo())
            cpu, rss = proc_cpu(pid)
            size = int(mi["size"])
            accepted = "" if prev_size is None else size - prev_size
            cpu_pct = "" if prev_cpu is None else round((cpu - prev_cpu) * 100 / a.interval, 1)
            f.write("%.3f,%d,%d,%d,%s,%d,%d,%d,%s,%s\n"
                    % (t, size, int(mi["bytes"]), int(mi["usage"]), accepted, peers,
                       int(ni["totalbytesrecv"]), int(ni["totalbytessent"]), cpu_pct, rss))
            f.flush()
            rows.append(accepted)
            prev_size, prev_cpu = size, cpu
            time.sleep(max(0.0, a.interval - (time.time() - t)))

    vals = [r for r in rows if isinstance(r, int)]
    if vals:
        print(json.dumps(dict(samples=len(vals), accepted_total=sum(vals),
                              accepted_mean=round(sum(vals) / len(vals), 1),
                              accepted_max=max(vals), final_mempool=prev_size), indent=2))


if __name__ == "__main__":
    main()
