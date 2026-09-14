#!/usr/bin/env python3
"""Do peers hold the same subset, or complementary ones?

The trickle selects what to announce with a deterministic global ordering
(CompareDepthAndScore), identical on every peer link. If that makes every peer
receive the same prefix, then redundancy buys no coverage: a node missing a
transaction cannot fetch it from a different peer, because every peer is missing
the same one.
"""
import argparse
import itertools
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

from test_framework.authproxy import AuthServiceProxy  # noqa: E402


def connect(datadir, port, chain="regtest"):
    cookie = open(os.path.join(datadir, chain, ".cookie")).read().strip()
    return AuthServiceProxy("http://%s@127.0.0.1:%d" % (cookie, port), timeout=180)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hub", required=True)
    ap.add_argument("--hub-port", type=int, default=19898)
    ap.add_argument("--peer", action="append", required=True, help="datadir:port")
    a = ap.parse_args()

    hub = set(connect(a.hub, a.hub_port).getrawmempool())
    peers = []
    for spec in a.peer:
        d, p = spec.rsplit(":", 1)
        peers.append(set(connect(d, int(p)).getrawmempool()))

    print("hub mempool: %d" % len(hub))
    for i, p in enumerate(peers):
        print("  peer %d: %d  (%.1f%% of hub)" % (i, len(p), 100.0 * len(p) / max(1, len(hub))))

    print()
    print("pairwise overlap between peers:")
    for i, j in itertools.combinations(range(len(peers)), 2):
        a_, b_ = peers[i], peers[j]
        inter = len(a_ & b_)
        union = len(a_ | b_)
        jac = 100.0 * inter / max(1, union)
        print("  peer %d vs peer %d: shared %d, union %d, Jaccard %.1f%%" % (i, j, inter, union, jac))

    allp = set().union(*peers) if peers else set()
    best = max((len(p) for p in peers), default=0)
    print()
    print("union of ALL peers:   %d" % len(allp))
    print("best single peer:     %d" % best)
    print("coverage gained by having %d peers instead of the best one: %+d (%.1f%%)"
          % (len(peers), len(allp) - best, 100.0 * (len(allp) - best) / max(1, best)))
    print("hub transactions no peer has: %d (%.1f%%)"
          % (len(hub - allp), 100.0 * len(hub - allp) / max(1, len(hub))))


if __name__ == "__main__":
    main()
