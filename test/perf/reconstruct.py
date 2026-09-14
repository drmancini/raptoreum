#!/usr/bin/env python3
"""Measure how much of a block a peer can reconstruct from its own mempool.

A compact block is decoupling in miniature: the sender transmits short
identifiers, the receiver fills the block from its mempool and fetches only what
it lacks. The fraction it must fetch is the decoupling premise, measured.
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
from test_framework.authproxy import AuthServiceProxy  # noqa: E402


def connect(datadir, port, chain="regtest"):
    cookie = open(os.path.join(datadir, chain, ".cookie")).read().strip()
    return AuthServiceProxy("http://%s@127.0.0.1:%d" % (cookie, port), timeout=180)


def msg_bytes(rpc_conn):
    """Per-message byte counters summed across peers."""
    sent, recv = {}, {}
    for p in rpc_conn.getpeerinfo():
        for k, v in (p.get("bytessent_per_msg") or {}).items():
            sent[k] = sent.get(k, 0) + int(v)
        for k, v in (p.get("bytesrecv_per_msg") or {}).items():
            recv[k] = recv.get(k, 0) + int(v)
    return sent, recv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hub", required=True, help="datadir of the mining node")
    ap.add_argument("--hub-wallet", default="perf")
    ap.add_argument("--peer", required=True, help="datadir of the receiving node")
    ap.add_argument("--peer-port", type=int, default=19911)
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    hub = rpc(a.hub, wallet=a.hub_wallet)
    peer = connect(a.peer, a.peer_port)

    peer_pool = set(peer.getrawmempool())
    hub_pool = set(hub.getrawmempool())
    sent0, recv0 = msg_bytes(peer)

    addr = hub.getnewaddress()
    t0 = time.time()
    blockhash = hub.generatetoaddress(1, addr)[0]
    mine_s = time.time() - t0

    # let the peer receive and reconstruct
    for _ in range(120):
        if peer.getblockcount() == hub.getblockcount():
            break
        time.sleep(0.5)
    time.sleep(2)

    block = hub.getblock(blockhash, 1)
    btx = set(block["tx"][1:])          # skip the coinbase
    sent1, recv1 = msg_bytes(peer)

    had = len(btx & peer_pool)
    missing = len(btx - peer_pool)
    delta = lambda d1, d0, k: d1.get(k, 0) - d0.get(k, 0)

    out = dict(
        label=a.label,
        block_txs=len(btx),
        block_size=block["size"],
        peer_mempool_before=len(peer_pool),
        hub_mempool_before=len(hub_pool),
        could_fill=had,
        must_fetch=missing,
        fill_pct=round(100.0 * had / max(1, len(btx)), 1),
        mine_seconds=round(mine_s, 2),
        peer_synced=peer.getblockcount() == hub.getblockcount(),
        bytes_getblocktxn_sent=delta(sent1, sent0, "getblocktxn"),
        bytes_blocktxn_recv=delta(recv1, recv0, "blocktxn"),
        bytes_cmpctblock_recv=delta(recv1, recv0, "cmpctblock"),
        bytes_block_recv=delta(recv1, recv0, "block"),
    )
    print(json.dumps(out))


if __name__ == "__main__":
    main()
