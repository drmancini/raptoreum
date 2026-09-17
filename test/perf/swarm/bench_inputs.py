#!/usr/bin/env python3
"""Validation cost against input count, to test the quadratic-sighash claim.

RTM has no SegWit (SigVersion::BASE only), so SignatureHash reserialises the
whole transaction for every input: an N-input transaction should cost O(N^2).
MAX_STANDARD_TX_SIZE (100kB) exists to bound that, and its own comment says so.

Each N is timed twice: once with a valid transaction, and once with a
same-shape transaction whose first prevout does not exist. The second is
rejected before any signature work but after the same parsing and transport, so
the difference isolates validation from the cost of moving a large hex blob.
"""
import argparse, base64, http.client, json, os, sys, time, statistics

sys.path.insert(0, "/data/rtm-perf/rig")
sys.path.insert(0, "/data/forge/projects/raptoreum/test/functional")
import coincurve, wire

ap = argparse.ArgumentParser()
ap.add_argument("--utxos", required=True)
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--cookie", required=True)
ap.add_argument("--counts", default="1,2,5,10,25,50,100,200,400,800")
ap.add_argument("--reps", type=int, default=3)
a = ap.parse_args()

auth = open(a.cookie).read().strip()
hdr = {"Content-Type": "application/json",
       "Authorization": "Basic " + base64.b64encode(auth.encode()).decode()}
def rpc(method, params):
    c = http.client.HTTPConnection("127.0.0.1", a.port, timeout=600)
    c.request("POST", "/", json.dumps({"jsonrpc": "1.0", "id": "b",
                                       "method": method, "params": params}), hdr)
    d = json.loads(c.getresponse().read()); c.close()
    return d.get("result"), d.get("error")

print("loading utxos...", flush=True)
U = json.load(open(a.utxos))
print("  %d available" % len(U), flush=True)
cur = 0

def take(n):
    global cur
    s = U[cur:cur + n]; cur += n
    return [(int(u["txid"], 16), u["vout"], u["value"],
             bytes.fromhex(u["pubkey"]), bytes.fromhex(u["secret"])) for u in s]

def build(ins, bogus=False):
    if bogus:
        ins = [(1, 0, ins[0][2]) + ins[0][3:]] + list(ins[1:])
    inputs = [(i[0], i[1], i[2], i[3]) for i in ins]
    total = sum(i[2] for i in ins)
    # The minimum relay fee scales with size, so a flat fee is rejected before
    # any validation happens once the transaction is large -- which silently
    # turns the measurement into a parse benchmark. A P2PKH input is ~148 B.
    fee = max(10000, (len(ins) * 150 + 200) * 3)
    out = (total - fee) // 2
    outputs = [(ins[0][3], out), (ins[-1][3], out)]
    tx = wire.build_tx(inputs, outputs)
    keys = [coincurve.PrivateKey(i[4]) for i in ins]
    wire.sign_tx(tx, inputs, lambda i, m: keys[i].sign(m, hasher=None))
    return tx

print("%6s %9s %11s %11s %11s %9s" % ("inputs","bytes","valid_ms","bogus_ms","delta_ms","us/input"))
for n in [int(x) for x in a.counts.split(",")]:
    vs, bs, size = [], [], 0
    for _ in range(a.reps):
        ins = take(n)
        t0 = time.time(); tx = build(ins); tbuild = time.time() - t0
        raw = tx.serialize().hex(); size = len(raw) // 2
        t0 = time.time(); r, e = rpc("sendrawtransaction", [raw]); vs.append((time.time() - t0) * 1000)
        if e and "already" not in str(e).lower() and "missing" not in str(e).lower():
            print("  n=%d rejected: %s" % (n, str(e)[:90])); break
        ins2 = take(n)
        txb = build(ins2, bogus=True)
        t0 = time.time(); rpc("sendrawtransaction", [txb.serialize().hex()]); bs.append((time.time() - t0) * 1000)
    if not vs: continue
    v = statistics.median(vs); b = statistics.median(bs) if bs else 0.0
    print("%6d %9d %11.1f %11.1f %11.1f %9.1f"
          % (n, size, v, b, v - b, (v - b) * 1000.0 / n), flush=True)
