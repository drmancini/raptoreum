#!/usr/bin/env python3
"""Phase 2: turn the fan-out set into pre-framed transactions ready to send.

Each transaction spends two outputs and pays two, back to the same two keys, so
every new output carries the secret that can spend it and no key bookkeeping is
needed. Generations are emitted in order, which is what keeps the file free of
any transaction appearing before its ancestors.
"""
import argparse
import json
import os
import struct
import sys
import time
from multiprocessing import get_context

import hashlib

import coincurve

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

import wire  # noqa: E402


def sign_one(spec):
    """spec: ((txid_int, vout, value, pubkey, secret) x width, out_value)"""
    group, out_value = spec
    inputs = [(u[0], u[1], u[2], u[3]) for u in group]
    outputs = [(u[3], out_value) for u in group]
    tx = wire.build_tx(inputs, outputs)
    keys = [coincurve.PrivateKey(u[4]) for u in group]
    wire.sign_tx(tx, inputs, lambda i, msg32: keys[i].sign(msg32, hasher=None))
    return tx.hash, wire.tx_message(tx)


def lineage_fee(seed, lineage, lo, hi):
    """Deterministic per-lineage fee, so shards stay self-consistent.

    Skewed low: most transactions pay near the floor and a tail pays much more,
    which is the shape a real mempool has and the shape fee-ordered relay and
    block selection are designed for.
    """
    h = hashlib.sha256(b"%s:%d" % (seed, lineage)).digest()
    u = int.from_bytes(h[:8], "big") / float(1 << 64)
    return int(lo + (hi - lo) * (u ** 3))


def generation(pool, utxos, fee, chunk, width=2, fee_range=None, fee_seed=b""):
    """Group the utxos width-at-a-time, sign, and return (results, next_utxos)."""
    specs = []
    for i in range(0, len(utxos) - width + 1, width):
        group = utxos[i:i + width]
        total_in = sum(u[2] for u in group)
        f = fee if fee_range is None else lineage_fee(fee_seed, i // width, *fee_range)
        # keep every output comfortably above dust however large the fee is
        f = min(f, max(0, total_in - 2000 * width))
        out_value = (total_in - f) // width
        if out_value <= 0:
            break
        specs.append((group, out_value))
    if pool is None:
        results = [sign_one(s) for s in specs]
    else:
        results = pool.map(sign_one, specs, chunksize=chunk)

    nxt = []
    for (txid_hex, _msg), (group, out_value) in zip(results, specs):
        txid = int(txid_hex, 16)
        for vout, u in enumerate(group):
            nxt.append((txid, vout, out_value, u[3], u[4]))
    return results, nxt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--count", type=int, default=250000)
    ap.add_argument("--width", type=int, default=2, help="inputs (and outputs) per transaction")
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("--fee", type=int, default=10000)
    ap.add_argument("--fee-min", type=int, default=0, help="if set, draw a per-lineage fee in [fee-min, fee-max]")
    ap.add_argument("--fee-max", type=int, default=0)
    ap.add_argument("--fee-seed", default="rtm-perf-fees-v1")
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--chunk", type=int, default=256)
    a = ap.parse_args()

    raw = json.load(open(os.path.join(a.corpus, "utxos.json")))
    utxos = [(int(u["txid"], 16), u["vout"], u["value"],
              bytes.fromhex(u["pubkey"]), bytes.fromhex(u["secret"])) for u in raw]
    print("fan-out set: %d outputs" % len(utxos))

    ctx = get_context("spawn")
    pool = ctx.Pool(a.workers) if a.workers > 1 else None

    written = 0
    sizes = []
    t0 = time.time()
    with open(os.path.join(a.corpus, "txs.bin"), "wb") as binf, \
         open(os.path.join(a.corpus, "txids.txt"), "w") as idf:
        for gen in range(a.depth):
            if written >= a.count:
                break
            fee_range = (a.fee_min, a.fee_max) if a.fee_max > a.fee_min else None
            results, utxos = generation(pool, utxos, a.fee, a.chunk, a.width,
                                        fee_range, a.fee_seed.encode())
            if not results:
                print("generation %d produced nothing; values exhausted" % gen)
                break
            for txid_hex, msg in results:
                if written >= a.count:
                    break
                binf.write(struct.pack("<I", len(msg)))
                binf.write(msg)
                idf.write(txid_hex + "\n")
                sizes.append(len(msg) - 24)
                written += 1
            print("  generation %d (ancestor depth %d): %d transactions, %d total, %.1fs"
                  % (gen, gen + 1, len(results), written, time.time() - t0))

    if pool is not None:
        pool.close()
        pool.join()

    elapsed = time.time() - t0
    manifest = dict(count=written, width=a.width, depth=a.depth, fee=a.fee,
                    fee_min=a.fee_min, fee_max=a.fee_max, fee_seed=a.fee_seed,
                    payload_min=min(sizes), payload_max=max(sizes),
                    payload_mean=round(sum(sizes) / len(sizes), 1),
                    seconds=round(elapsed, 1), workers=a.workers)
    with open(os.path.join(a.corpus, "corpus-manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("wrote %d transactions in %.1fs (%.0f/s), payload %d-%d bytes, mean %.1f"
          % (written, elapsed, written / elapsed, manifest["payload_min"],
             manifest["payload_max"], manifest["payload_mean"]))


if __name__ == "__main__":
    main()
