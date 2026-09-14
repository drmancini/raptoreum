#!/usr/bin/env python3
"""Measure how far apart the nodes' mempools drift.

A block that commits to transaction identifiers can only be reconstructed by a
peer that already holds those bodies. This measures whether the network actually
converges on a shared pending set, by taking the symmetric difference between
every node's mempool and the union of all of them.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

from test_framework.authproxy import AuthServiceProxy  # noqa: E402


def connect(datadir, port, chain="regtest"):
    cookie = open(os.path.join(datadir, chain, ".cookie")).read().strip()
    return AuthServiceProxy("http://%s@127.0.0.1:%d" % (cookie, port), timeout=120)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--node", action="append", required=True,
                    help="datadir:rpcport, repeat per node")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--interval", type=float, default=20.0)
    a = ap.parse_args()

    nodes = []
    for spec in a.node:
        d, p = spec.rsplit(":", 1)
        nodes.append((d, int(p), connect(d, int(p))))

    end = time.time() + a.seconds
    with open(a.out, "w") as f:
        f.write("ts," + ",".join("n%d_size" % i for i in range(len(nodes)))
                + ",union,missing_max,missing_total,worst_node\n")
        while time.time() < end:
            t = time.time()
            pools = []
            for _d, _p, rpc in nodes:
                try:
                    pools.append(set(rpc.getrawmempool()))
                except Exception as e:
                    print("rpc error:", e, flush=True)
                    pools.append(set())
            union = set().union(*pools) if pools else set()
            missing = [len(union - p) for p in pools]
            worst = missing.index(max(missing)) if missing else -1
            f.write("%.3f,%s,%d,%d,%d,%d\n"
                    % (t, ",".join(str(len(p)) for p in pools), len(union),
                       max(missing) if missing else 0, sum(missing), worst))
            f.flush()
            print("union %8d | sizes %s | missing %s"
                  % (len(union), [len(p) for p in pools], missing), flush=True)
            time.sleep(max(0.0, a.interval - (time.time() - t)))


if __name__ == "__main__":
    main()
