#!/usr/bin/env python3
"""Measure how fast transactions cross from one node to another.

Acceptance says what a node will take. This says what it will pass on, which is
the number that decides whether the network converges on a shared pending set.
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
    ap.add_argument("--datadir-a", required=True)
    ap.add_argument("--datadir-b", required=True)
    ap.add_argument("--wallet-a", default="perf")
    ap.add_argument("--port-b", type=int, default=19900)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=180)
    a = ap.parse_args()

    A = rpc(a.datadir_a, wallet=a.wallet_a)
    cookie = open(os.path.join(a.datadir_b, "regtest", ".cookie")).read().strip()
    from test_framework.authproxy import AuthServiceProxy
    B = AuthServiceProxy("http://%s@127.0.0.1:%d" % (cookie, a.port_b))

    prev_a = prev_b = None
    rows = []
    end = time.time() + a.seconds
    with open(a.out, "w") as f:
        f.write("ts,a_txs,b_txs,a_rate,b_rate,lag_txs,b_bytesrecv\n")
        while time.time() < end:
            t = time.time()
            ma, mb = int(A.getmempoolinfo()["size"]), int(B.getmempoolinfo()["size"])
            nb = int(B.getnettotals()["totalbytesrecv"])
            ra = "" if prev_a is None else ma - prev_a
            rb = "" if prev_b is None else mb - prev_b
            f.write("%.3f,%d,%d,%s,%s,%d,%d\n" % (t, ma, mb, ra, rb, ma - mb, nb))
            f.flush()
            if isinstance(rb, int):
                rows.append((ra, rb))
            prev_a, prev_b = ma, mb
            time.sleep(max(0.0, 1.0 - (time.time() - t)))

    live = [r for r in rows if r[0] > 0]
    if live:
        print("while the sender was accepting:")
        print("  A accepted/s: mean %.0f" % (sum(r[0] for r in live)/len(live)))
        print("  B received/s: mean %.1f max %d" % (sum(r[1] for r in live)/len(live),
                                                    max(r[1] for r in live)))
    tail = [r[1] for r in rows[-20:]]
    if tail:
        print("  B received/s over the last 20 samples: mean %.1f" % (sum(tail)/len(tail)))
    print("final: A=%d B=%d lag=%d" % (prev_a, prev_b, prev_a - prev_b))


if __name__ == "__main__":
    main()
