#!/usr/bin/env python3
"""Phase 1: build the fan-out UTXO set the load corpus spends.

Drives an already-running regtest node over RPC. Keys are generated here and
never enter the node's wallet, so phase 2 can sign offline without a single
dumpprivkey call.
"""
import argparse
import json
import os
import sys
import time

import coincurve

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "functional"))
from test_framework.address import key_to_p2pkh          # noqa: E402
from test_framework.authproxy import AuthServiceProxy     # noqa: E402


def rpc(datadir, chain="regtest"):
    cookie = os.path.join(datadir, chain, ".cookie")
    with open(cookie) as f:
        auth = f.read().strip()
    port = {"regtest": 19898, "testnet3": 10229, "main": 10226}[chain]
    return AuthServiceProxy("http://%s@127.0.0.1:%d" % (auth, port))


def make_keys(n, seed):
    keys = []
    for i in range(n):
        secret = coincurve.utils.sha256(b"%s:%d" % (seed, i))
        k = coincurve.PrivateKey(secret)
        pub = k.public_key.format(compressed=True)
        keys.append((secret, pub, key_to_p2pkh(pub, main=False)))
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--utxos", type=int, default=200000)
    ap.add_argument("--per-tx", type=int, default=1000)
    ap.add_argument("--value", type=float, default=0.0005)
    ap.add_argument("--seed", default="rtm-perf-fanout-v1")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    node = rpc(a.datadir)
    print("height at start:", node.getblockcount())

    mine_to = node.getnewaddress()
    need = a.utxos * a.value * 1.2
    while float(node.getbalance()) < need:
        node.generatetoaddress(100, mine_to)
        print("  balance %.2f / %.2f" % (float(node.getbalance()), need))

    print("generating %d keys" % a.utxos)
    t0 = time.time()
    keys = make_keys(a.utxos, a.seed.encode())
    print("  %.1fs" % (time.time() - t0))

    batches = [keys[i:i + a.per_tx] for i in range(0, len(keys), a.per_tx)]
    txids = []
    for n, batch in enumerate(batches):
        txid = node.sendmany("", {addr: a.value for _s, _p, addr in batch})
        txids.append(txid)
        if n % 10 == 9:
            node.generatetoaddress(1, mine_to)
            print("  batch %d/%d" % (n + 1, len(batches)))
    node.generatetoaddress(6, mine_to)

    print("indexing outputs from %d transactions" % len(txids))
    by_addr = {}
    for txid in txids:
        raw = node.getrawtransaction(txid, True)
        for out in raw["vout"]:
            spk = out["scriptPubKey"]
            for addr in spk.get("addresses", []):
                by_addr[addr] = (txid, out["n"], int(round(out["value"] * 1e8)))

    utxos = []
    for secret, pub, addr in keys:
        if addr in by_addr:
            txid, vout, value = by_addr[addr]
            utxos.append(dict(txid=txid, vout=vout, value=value,
                              secret=secret.hex(), pubkey=pub.hex(), addr=addr))

    manifest = dict(utxos=len(utxos), requested=a.utxos, value_sat=int(a.value * 1e8),
                    seed=a.seed, tip=node.getbestblockhash(), height=node.getblockcount())
    with open(os.path.join(a.out, "utxos.json"), "w") as f:
        json.dump(utxos, f)
    with open(os.path.join(a.out, "fanout-manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("wrote %d utxos, tip %s at height %d"
          % (len(utxos), manifest["tip"], manifest["height"]))
    if len(utxos) != a.utxos:
        print("WARNING: %d of %d requested outputs were not found"
              % (a.utxos - len(utxos), a.utxos))


if __name__ == "__main__":
    main()
