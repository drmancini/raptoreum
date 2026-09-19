#!/usr/bin/env python3
"""F-91/F-93: does per-sigop validation cost scale with total preimage bytes,
not just with the sigop-bearing input's own size -- and if the answer turns
out to be no, what actually explodes?

bench_sigops.py's two points (P2SH 150-in ~133 us/key, bare 800-in ~196 us/key)
never separate "how many sigops" from "how many other inputs share the
transaction", because in both experiments every input was ALSO a sigop-bearing
multisig input. interpreter.cpp:SerializeInput blanks every OTHER input's
script to empty regardless of its real type or size (36B prevout + 1B empty
script + 4B sequence = 41B, fixed) -- so the cheapest possible padding input,
one that spends a trivially-true prevout with a genuinely empty scriptSig, is
*already* at its blanked minimum. It packs more "other inputs" per byte of
real transaction size than bench_sigops.py's bare-multisig-only construction
did, so that experiment is not the true worst case.

This holds sigops FIXED (one 1-of-N bare multisig input; --best signs with the
key tried first so only 1 CheckSig runs, the default signs so all N run) and
varies the padding count P of empty-scriptSig inputs spending OP_TRUE outputs.

RESULT (2026-09-19, see docs/findings.md F-91/F-93/F-94, R-34): us/sigop is
not the right lens. Total cost is the same whether 1 key or all N are tried
(under 3%), and the same again with the multisig input removed entirely
(bench_puretrue.py, zero sigops). The real driver is P alone: cost ~ 6.5e-5 *
P^2 ms, reaching ~370-385 ms at P~2400 (K-3's ~100kB per-tx ceiling) -- and it
lives in mempool ATMP specifically, not ConnectBlock (~70 ms cold for the
identical transaction, measured on a second node that never ran it through
its own mempool). D-18's sigop budget was derived to bound ConnectBlock's
worst case and stands; this surfaced a different, likely pre-existing mempool
DoS question instead (open: can this shape pass standardness policy at all).

Run against an isolated regtest node with acceptnonstdtxn=1 and checkmempool=0.
IMPORTANT: pick rpcport/port that don't collide with any real node -- this
script mines many blocks on whatever it's pointed at.
"""
import argparse
import base64
import hashlib
import http.client
import json
import os
import statistics
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.join(_HERE, ".."))

import coincurve  # noqa: E402
import wire  # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut  # noqa: E402
from test_framework.script import (  # noqa: E402
    CScript, CScriptOp, OP_0, OP_TRUE, OP_CHECKMULTISIG, SIGHASH_ALL, SignatureHash,
)

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--cookie", required=True)
ap.add_argument("--pad-counts", default="0,100,500,1000,1500,2000,2500")
ap.add_argument("--reps", type=int, default=3)
ap.add_argument("--keys", type=int, default=15)
ap.add_argument("--best", action="store_true",
                help="sign with the key tried FIRST (1 CheckSig attempt), control for --worst")
a = ap.parse_args()

auth = open(a.cookie).read().strip()
HDR = {"Content-Type": "application/json",
       "Authorization": "Basic " + base64.b64encode(auth.encode()).decode()}


def rpc_raw(method, params):
    c = http.client.HTTPConnection("127.0.0.1", a.port, timeout=600)
    c.request("POST", "/", json.dumps({"jsonrpc": "1.0", "id": "p",
                                       "method": method, "params": params}), HDR)
    d = json.loads(c.getresponse().read())
    c.close()
    return d.get("result"), d.get("error")


def rpc(method, params):
    r, e = rpc_raw(method, params)
    if e:
        sys.exit("rpc %s failed: %s" % (method, e))
    return r


# ---- funding key: one P2PKH UTXO from the wallet, spent to build the fanout

def derive_key(seed=b"rtm-f91-funding-v1"):
    return coincurve.PrivateKey(hashlib.sha256(seed).digest())


FUND_KEY = derive_key()
FUND_PUB = FUND_KEY.public_key.format(compressed=True)


def confirm_mempool():
    addr = rpc("getnewaddress", [])
    for _ in range(60):
        if rpc("getmempoolinfo", []).get("size", 1) == 0:
            return
        rpc("generatetoaddress", [1, addr])
    sys.exit("mempool would not confirm")


print("funding the seed P2PKH output from the wallet...", flush=True)
fund_addr = rpc("getnewaddress", [])
# send straight to our own key's P2PKH address instead, via a raw send:
from test_framework.address import key_to_p2pkh  # noqa: E402
our_addr = key_to_p2pkh(FUND_PUB)
txid = rpc("sendtoaddress", [our_addr, 39.0])
confirm_mempool()
# locate our funded output
raw = rpc("getrawtransaction", [txid, True])
seed_vout = None
seed_value = None
for o in raw["vout"]:
    spk = o["scriptPubKey"].get("hex", "")
    if spk == wire.p2pkh_script(FUND_PUB).hex():
        seed_vout = o["n"]
        seed_value = int(round(o["value"] * 100000000))
assert seed_vout is not None, "could not find our funded output"
print("  seed utxo: %s:%d value=%d" % (txid, seed_vout, seed_value), flush=True)

# ---- build the fanout: many OP_TRUE outputs, a few bare multisig outputs ----

PAD_COUNTS = [int(x) for x in a.pad_counts.split(",")]
NEED_PAD = sum(PAD_COUNTS) * a.reps
NEED_MULTI = len(PAD_COUNTS) * a.reps + 2   # bogus reuses no real multisig output

KEYS = [coincurve.PrivateKey(hashlib.sha256(b"rtm-f91-ms-v1" + i.to_bytes(4, "little")).digest())
        for i in range(a.keys)]
PUBS = [k.public_key.format(compressed=True) for k in KEYS]
REDEEM = CScript([CScriptOp.encode_op_n(1)] + PUBS +
                 [CScriptOp.encode_op_n(a.keys), OP_CHECKMULTISIG])
TRIVIAL_SPK = CScript([OP_TRUE])
PAD_VALUE = 600      # sat, just above the dust-ish floor for a script this small
MULTI_VALUE = 100000


def build_and_send(txid_int, vout, in_value, outputs, extra_sigops_input=None):
    """One funding round: spend (txid_int, vout) into `outputs` (list of (spk, value)),
    optionally leaving room for extra_sigops_input's own fee handling."""
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    tx.vin.append(CTxIn(COutPoint(txid_int, vout), b"", 0xffffffff))
    total_out = sum(v for _s, v in outputs)
    tx.vout = [CTxOut(v, s) for s, v in outputs]
    fee = in_value - total_out
    assert fee > 0, "fee went negative, shrink batch or raise input value"
    sighash, err = SignatureHash(wire.p2pkh_script(FUND_PUB), tx, 0, SIGHASH_ALL)
    assert err is None, err
    sig = FUND_KEY.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
    tx.vin[0].scriptSig = CScript([sig, FUND_PUB])
    tx.rehash()
    txh = rpc("sendrawtransaction", [tx.serialize().hex()])
    return int(txh, 16)


print("building fanout: %d OP_TRUE outputs, %d multisig outputs..." % (NEED_PAD, NEED_MULTI), flush=True)
pad_outpoints = []
multi_outpoints = []
cur_txid, cur_vout, cur_value = int(txid, 16), seed_vout, seed_value

BATCH = 3000  # outputs per funding tx, well under the 100kB tx-size cap
remaining_pad = NEED_PAD
remaining_multi = NEED_MULTI
while remaining_pad > 0 or remaining_multi > 0:
    n_pad = min(BATCH - 5, remaining_pad)
    n_multi = min(5, remaining_multi) if remaining_pad <= 0 or n_pad < BATCH - 5 else 0
    outs = [(TRIVIAL_SPK, PAD_VALUE) for _ in range(n_pad)] + \
           [(REDEEM, MULTI_VALUE) for _ in range(n_multi)]
    fee = 50000
    change = cur_value - sum(v for _s, v in outs) - fee
    if change > 1000:
        outs.append((wire.p2pkh_script(FUND_PUB), change))
        change_idx = len(outs) - 1
    else:
        change_idx = None
    new_txid = build_and_send(cur_txid, cur_vout, cur_value, outs)
    confirm_mempool()   # each batch spends the previous batch's change output;
                        # confirming here keeps the ancestor chain at length 1
    for i in range(n_pad):
        pad_outpoints.append((new_txid, i))
    for i in range(n_multi):
        multi_outpoints.append((new_txid, n_pad + i))
    remaining_pad -= n_pad
    remaining_multi -= n_multi
    if change_idx is not None:
        cur_txid, cur_vout, cur_value = new_txid, change_idx, change
    else:
        break
    print("  funded pad=%d/%d multi=%d/%d" %
          (len(pad_outpoints), NEED_PAD, len(multi_outpoints), NEED_MULTI), flush=True)

confirm_mempool()
print("fanout confirmed. pad=%d multi=%d" % (len(pad_outpoints), len(multi_outpoints)), flush=True)

# ---- the timed packed transaction -------------------------------------

SIGNER = KEYS[-1] if a.best else KEYS[0]  # KEYS[-1]: matches on the first attempt (1 CheckSig);
                                          # KEYS[0]: tried last, so every key is attempted
SCRIPTCODE = REDEEM


def spend(n_pad, pad_pts, multi_pt, bogus=False):
    """bogus=True: input 0 doesn't exist, so ATMP rejects at the "do all
    inputs exist" check before touching any other input -- the remaining pad
    slots and the multisig slot never need to be real, valid, or even
    internally consistent, only present so the transaction is the SAME SIZE
    as the real one (isolating validation cost from transport/parse cost)."""
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    if bogus:
        for _ in range(n_pad):
            tx.vin.append(CTxIn(COutPoint(1, 0), b"", 0xffffffff))
        multi_index = len(tx.vin)
        tx.vin.append(CTxIn(COutPoint(1, 0), b"", 0xffffffff))
    else:
        for txid_i, vout_i in pad_pts:
            tx.vin.append(CTxIn(COutPoint(txid_i, vout_i), b"", 0xffffffff))
        multi_index = len(tx.vin)
        tx.vin.append(CTxIn(COutPoint(*multi_pt), b"", 0xffffffff))
    total_in = n_pad * PAD_VALUE + MULTI_VALUE
    fee = max(20000, (n_pad * 45 + 300) * 3)
    out = (total_in - fee) // 2
    tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
    tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
    # pad inputs need NO scriptSig at all: OP_TRUE requires nothing on the
    # stack beyond what it pushes itself.
    for i in range(n_pad):
        tx.vin[i].scriptSig = CScript([])
    if bogus:
        # never validated (rejected at input 0 first) -- any push of the
        # right rough length keeps the transaction byte-for-byte comparable.
        tx.vin[multi_index].scriptSig = CScript([OP_0, b"\x00" * 71])
    else:
        sighash, err = SignatureHash(SCRIPTCODE, tx, multi_index, SIGHASH_ALL)
        assert err is None, err
        sig = SIGNER.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
        tx.vin[multi_index].scriptSig = CScript([OP_0, sig])
    tx.rehash()
    return tx


print("%8s %9s %11s %11s %11s %9s" % ("pad_in", "bytes", "valid_ms", "bogus_ms", "delta_ms", "us/sigop"), flush=True)
pcur = 0
mcur = 0
for n in PAD_COUNTS:
    vs, bs, size = [], [], 0
    for _ in range(a.reps):
        pad_pts = pad_outpoints[pcur:pcur + n] if n > 0 else []
        pcur += n
        multi_pt = multi_outpoints[mcur]
        mcur += 1
        tx = spend(n, pad_pts, multi_pt)
        raw_hex = tx.serialize().hex()
        size = len(raw_hex) // 2
        t0 = time.time()
        _r, e = rpc_raw("sendrawtransaction", [raw_hex])
        vs.append((time.time() - t0) * 1000)
        if e:
            sys.exit("valid spend n=%d rejected: %s" % (n, e))
        txb = spend(n, None, None, bogus=True)
        t0 = time.time()
        _r, e = rpc_raw("sendrawtransaction", [txb.serialize().hex()])
        bs.append((time.time() - t0) * 1000)
        if not e:
            sys.exit("bogus spend n=%d was NOT rejected -- test is broken" % n)
        # F-91 methodology fix: mine the valid tx away immediately, so every
        # measurement starts from an EMPTY mempool. Left unmined, mempool-wide
        # bookkeeping (fee sort, ancestor sets, conflict scans) accumulates
        # across the sweep and confounds "cost vs this tx's input count" with
        # "cost vs total mempool size" -- the first run of this script did
        # exactly that (24 unconfirmed txs, 1.25 MB, by the last data point).
        confirm_mempool()
    v = statistics.median(vs)
    b = statistics.median(bs)
    delta = v - b
    print("%8d %9d %11.1f %11.1f %11.1f %9.2f"
          % (n, size, v, b, delta, delta * 1000.0 / (1 if a.best else a.keys)), flush=True)
