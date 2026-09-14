#!/usr/bin/env python3
"""Time block template assembly as the mempool grows.

getblocktemplate selects from every candidate in the pool, and pools call it
constantly. If assembly is superlinear in pool size, a miner on a busy chain
stalls on every template — and that is true at the shipped block size, with no
patches.

Note on caching: the node reuses a template unless the tip changed or more than
five seconds have passed AND the mempool was updated. During an active fill the
mempool is always updated, so an interval above five seconds forces a rebuild
every time.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

from fanout import rpc  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--wallet", default="perf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--interval", type=float, default=10.0)
    a = ap.parse_args()

    node = rpc(a.datadir, wallet=a.wallet)
    end = time.time() + a.seconds
    with open(a.out, "w") as f:
        f.write("ts,mempool_txs,gbt_ms,template_txs,template_bytes,getmempoolinfo_ms\n")
        while time.time() < end:
            t0 = time.time()
            mi = node.getmempoolinfo()
            t_mi = (time.time() - t0) * 1000

            t1 = time.time()
            tmpl = node.getblocktemplate()
            gbt = (time.time() - t1) * 1000

            txs = tmpl.get("transactions", [])
            nbytes = sum(len(t["data"]) // 2 for t in txs)
            f.write("%.3f,%d,%.1f,%d,%d,%.1f\n"
                    % (time.time(), int(mi["size"]), gbt, len(txs), nbytes, t_mi))
            f.flush()
            print("mempool %8d   gbt %8.1f ms   template %5d txs / %d bytes"
                  % (int(mi["size"]), gbt, len(txs), nbytes), flush=True)
            time.sleep(max(0.0, a.interval - (time.time() - t0)))


if __name__ == "__main__":
    main()
