#!/usr/bin/env python3
"""Control for F-91/F-93: does validation cost scale with input count even with
ZERO sigops at all (every input a bare OP_TRUE spend, no CHECKSIG anywhere)?

RESULT (2026-09-19): yes, and it matches bench_packed.py's --worst and --best
curves almost exactly at every N -- see docs/findings.md F-93. The cost is not
about sigop accounting at all; it is a general many-input cost in mempool ATMP
(confirmed absent at ConnectBlock, see bench_packed.py's docstring and F-94).
"""
import argparse, base64, hashlib, http.client, json, os, statistics, sys, time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.join(_HERE, ".."))
import coincurve, wire  # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut  # noqa: E402
from test_framework.script import CScript, OP_TRUE  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--cookie", required=True)
ap.add_argument("--counts", default="200,500,1000,1500,2000,2400")
ap.add_argument("--reps", type=int, default=4)
a = ap.parse_args()

auth = open(a.cookie).read().strip()
HDR = {"Content-Type": "application/json",
       "Authorization": "Basic " + base64.b64encode(auth.encode()).decode()}


def rpc_raw(method, params):
    c = http.client.HTTPConnection("127.0.0.1", a.port, timeout=600)
    c.request("POST", "/", json.dumps({"jsonrpc": "1.0", "id": "t",
                                       "method": method, "params": params}), HDR)
    d = json.loads(c.getresponse().read())
    c.close()
    return d.get("result"), d.get("error")


def rpc(method, params):
    r, e = rpc_raw(method, params)
    if e:
        sys.exit("rpc %s failed: %s" % (method, e))
    return r


FUND_KEY = coincurve.PrivateKey(hashlib.sha256(b"rtm-f91-puretrue-v1").digest())
FUND_PUB = FUND_KEY.public_key.format(compressed=True)
TRIVIAL_SPK = CScript([OP_TRUE])
PAD_VALUE = 600


def confirm_mempool():
    addr = rpc("getnewaddress", [])
    for _ in range(60):
        if rpc("getmempoolinfo", []).get("size", 1) == 0:
            return
        rpc("generatetoaddress", [1, addr])
    sys.exit("mempool would not confirm")


from test_framework.address import key_to_p2pkh  # noqa: E402
our_addr = key_to_p2pkh(FUND_PUB)
seed_txid = rpc("sendtoaddress", [our_addr, 39.0])
confirm_mempool()
raw = rpc("getrawtransaction", [seed_txid, True])
seed_vout = next(o["n"] for o in raw["vout"]
                 if o["scriptPubKey"].get("hex", "") == wire.p2pkh_script(FUND_PUB).hex())
seed_value = int(round(next(o["value"] for o in raw["vout"] if o["n"] == seed_vout) * 100000000))

COUNTS = [int(x) for x in a.counts.split(",")]
NEED = sum(COUNTS) * a.reps * 2   # valid + a same-count fresh set per rep (not reused)
print("funding %d OP_TRUE outputs..." % NEED, flush=True)

pad_outpoints = []
cur_txid, cur_vout, cur_value = int(seed_txid, 16), seed_vout, seed_value
BATCH = 3000
remaining = NEED
while remaining > 0:
    n = min(BATCH, remaining)
    outs = [(TRIVIAL_SPK, PAD_VALUE) for _ in range(n)]
    fee = 50000
    change = cur_value - sum(v for _s, v in outs) - fee
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    tx.vin.append(CTxIn(COutPoint(cur_txid, cur_vout), b"", 0xffffffff))
    tx.vout = [CTxOut(v, s) for s, v in outs]
    if change > 1000:
        tx.vout.append(CTxOut(change, wire.p2pkh_script(FUND_PUB)))
        change_idx = len(tx.vout) - 1
    else:
        change_idx = None
    from test_framework.script import SIGHASH_ALL, SignatureHash  # noqa: E402
    sighash, err = SignatureHash(wire.p2pkh_script(FUND_PUB), tx, 0, SIGHASH_ALL)
    assert err is None, err
    sig = FUND_KEY.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
    tx.vin[0].scriptSig = CScript([sig, FUND_PUB])
    tx.rehash()
    txh = rpc("sendrawtransaction", [tx.serialize().hex()])
    confirm_mempool()
    new_txid = int(txh, 16)
    for i in range(n):
        pad_outpoints.append((new_txid, i))
    remaining -= n
    if change_idx is None:
        break
    cur_txid, cur_vout, cur_value = new_txid, change_idx, change
    print("  funded %d/%d" % (len(pad_outpoints), NEED), flush=True)

print("fanout ready: %d outputs" % len(pad_outpoints), flush=True)

print("%8s %9s %11s %11s %11s" % ("inputs", "bytes", "valid_ms", "bogus_ms", "delta_ms"), flush=True)
pcur = 0
for n in COUNTS:
    vs, bs, size = [], [], 0
    for _ in range(a.reps):
        pts = pad_outpoints[pcur:pcur + n]
        pcur += n
        tx = CTransaction()
        tx.nVersion = wire.TX_VERSION
        for txid_i, vout_i in pts:
            tx.vin.append(CTxIn(COutPoint(txid_i, vout_i), b"", 0xffffffff))
        total_in = n * PAD_VALUE
        fee = max(20000, (n * 45 + 300) * 3)
        out = (total_in - fee) // 2
        tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
        tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
        for i in range(n):
            tx.vin[i].scriptSig = CScript([])
        tx.rehash()
        raw_hex = tx.serialize().hex()
        size = len(raw_hex) // 2
        t0 = time.time()
        _r, e = rpc_raw("sendrawtransaction", [raw_hex])
        vs.append((time.time() - t0) * 1000)
        if e:
            sys.exit("valid n=%d rejected: %s" % (n, e))

        txb = CTransaction()
        txb.nVersion = wire.TX_VERSION
        for _ in range(n):
            txb.vin.append(CTxIn(COutPoint(1, 0), b"", 0xffffffff))
        txb.vout = list(tx.vout)
        for i in range(n):
            txb.vin[i].scriptSig = CScript([])
        txb.rehash()
        t0 = time.time()
        _r, e = rpc_raw("sendrawtransaction", [txb.serialize().hex()])
        bs.append((time.time() - t0) * 1000)
        if not e:
            sys.exit("bogus n=%d was NOT rejected" % n)
        confirm_mempool()   # keep every measurement against an EMPTY mempool
    v = statistics.median(vs)
    b = statistics.median(bs)
    print("%8d %9d %11.1f %11.1f %11.1f" % (n, size, v, b, v - b), flush=True)
