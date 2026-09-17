#!/usr/bin/env python3
"""Split a corpus txs.bin into per-node shards.

The v3 corpus is depth-1: every transaction spends fanout UTXOs directly, so
there are no parent/child ordering constraints and any partition is valid.
Round-robin keeps each shard a uniform sample of the corpus.

Format in and out is identical: repeated <uint32 little-endian length><record>.
"""
import argparse, os, struct, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--shards", type=int, required=True)
    ap.add_argument("--limit", type=int, default=0, help="0 = whole corpus")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    outs = [open(os.path.join(a.out, "shard-%02d.bin" % i), "wb") for i in range(a.shards)]
    counts = [0] * a.shards
    nbytes = [0] * a.shards

    with open(os.path.join(a.corpus, "txs.bin"), "rb") as f:
        i = 0
        while True:
            head = f.read(4)
            if len(head) < 4:
                break
            n = struct.unpack("<I", head)[0]
            rec = f.read(n)
            if len(rec) < n:
                break
            k = i % a.shards
            outs[k].write(head); outs[k].write(rec)
            counts[k] += 1; nbytes[k] += n
            i += 1
            if a.limit and i >= a.limit:
                break

    for o in outs:
        o.close()
    print("split %d transactions into %d shards" % (sum(counts), a.shards))
    for i, (c, b) in enumerate(zip(counts, nbytes)):
        print("  shard-%02d  %7d tx  %8.1f MB  mean %.0f B" % (i, c, b / 1e6, b / max(c, 1)))

main()
