#!/usr/bin/env python3
"""Validation cost of multisig inputs, and whether the sigop counters price it.

`bench_inputs.py` measured P2PKH and found cost linear in input count. That is
the cheap shape. The expensive one is CHECKMULTISIG: the legacy interpreter has
no BASE sighash cache, so it recomputes the whole-transaction sighash once per
public key *tried*. A 1-of-15 whose signature matches the last key therefore
costs 15 sighashes and 15 ECDSA verifications for one input.

Two shapes, because the node's accounting treats them completely differently:

  p2sh  scriptPubKey OP_HASH160 <h> OP_EQUAL, redeem 1-of-15
        GetP2SHSigOpCount charges the redeem script's ACCURATE count at spend
        time -- 15 sigops per input -- so the cost is visible to MaxBlockSigOps.

  bare  scriptPubKey OP_1 <15 keys> OP_15 OP_CHECKMULTISIG
        The scriptSig is OP_0 <sig>, which contains no sigop opcodes, and the
        spent scriptPubKey is never examined at spend time. Charged ZERO. The
        cost is invisible to every counter the node has.

Both are consensus-valid. Bare 15-key multisig is non-standard, so it does not
relay on mainnet -- a miner can still mine it, and regtest has
fRequireStandard=false, which is why it is measurable here.

Funding spends P2PKH outputs from a fanout set and is confirmed before the
timed spends, so nothing hits the 25-ancestor mempool chain limit.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import coincurve  # noqa: E402
import wire  # noqa: E402
from test_framework.address import key_to_p2pkh  # noqa: E402
from test_framework.messages import (  # noqa: E402
    COutPoint, CTransaction, CTxIn, CTxOut,
)
from test_framework.script import (  # noqa: E402
    CScript, CScriptOp, OP_0, OP_CHECKMULTISIG, OP_EQUAL, OP_HASH160,
    SIGHASH_ALL, SignatureHash, hash160,
)

ap = argparse.ArgumentParser()
ap.add_argument("--utxos", required=True, help="P2PKH fanout set with secrets")
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--cookie", required=True)
ap.add_argument("--shape", choices=["p2sh", "bare"], required=True)
ap.add_argument("--counts", default="10,25,50,100,150")
ap.add_argument("--reps", type=int, default=2)
ap.add_argument("--keys", type=int, default=15, help="pubkeys in the 1-of-N")
ap.add_argument("--value", type=int, default=100000, help="sat per funded output")
ap.add_argument("--worst", action="store_true",
                help="sign with the key tried LAST, so every key is attempted")
a = ap.parse_args()

auth = open(a.cookie).read().strip()
HDR = {"Content-Type": "application/json",
       "Authorization": "Basic " + base64.b64encode(auth.encode()).decode()}


def rpc(method, params):
    c = http.client.HTTPConnection("127.0.0.1", a.port, timeout=600)
    c.request("POST", "/", json.dumps({"jsonrpc": "1.0", "id": "s",
                                       "method": method, "params": params}), HDR)
    d = json.loads(c.getresponse().read())
    c.close()
    return d.get("result"), d.get("error")


# ---- the 1-of-N and its two encodings -------------------------------------

def derive_keys(n, seed=b"rtm-bench-sigops-v1"):
    out = []
    for i in range(n):
        sk = hashlib.sha256(seed + i.to_bytes(4, "little")).digest()
        out.append(coincurve.PrivateKey(sk))
    return out


KEYS = derive_keys(a.keys)
PUBS = [k.public_key.format(compressed=True) for k in KEYS]
# CHECKMULTISIG reads pubkeys off the stack from the top down (ikey starts at
# stacktop(-2), which is the LAST key the script pushed) so keys are tried in
# REVERSE script order. Signing with KEYS[0] therefore makes the interpreter
# try all N keys, paying a full whole-transaction sighash for each; signing
# with KEYS[-1] matches on the first attempt. --worst selects which.
SIGNER = KEYS[0] if a.worst else KEYS[-1]
# encode_op_n gives a real opcode; a bare int would be encoded as a number
# PUSH instead, which is a different script and would not verify.
REDEEM = CScript([CScriptOp.encode_op_n(1)] + PUBS +
                 [CScriptOp.encode_op_n(a.keys), OP_CHECKMULTISIG])
P2SH_SPK = CScript([OP_HASH160, hash160(REDEEM), OP_EQUAL])
SPK = P2SH_SPK if a.shape == "p2sh" else REDEEM
SCRIPTCODE = REDEEM  # both shapes sign over the multisig script itself


def script_sig(sig):
    if a.shape == "p2sh":
        return CScript([OP_0, sig, REDEEM])
    return CScript([OP_0, sig])


# ---- funding: P2PKH in, multisig out --------------------------------------

U = json.load(open(a.utxos))
print("fanout set: %d P2PKH utxos" % len(U), flush=True)
cur = 0


def take_p2pkh(n):
    global cur
    s = U[cur:cur + n]
    cur += n
    if len(s) < n:
        sys.exit("fanout set exhausted: need %d more" % (n - len(s)))
    return [(int(u["txid"], 16), u["vout"], u["value"],
             bytes.fromhex(u["pubkey"]), bytes.fromhex(u["secret"])) for u in s]


def fund(n_out, per_tx):
    """Create n_out outputs of SPK, confirmed. Returns [(txid_int, vout)]."""
    made = []
    while len(made) < n_out:
        batch = min(per_tx, n_out - len(made))
        ins = take_p2pkh(batch + 1)          # one extra input pays the fee
        tx = CTransaction()
        tx.nVersion = wire.TX_VERSION
        for txid, vout, _v, _pk, _sk in ins:
            tx.vin.append(CTxIn(COutPoint(txid, vout), b"", 0xffffffff))
        for _ in range(batch):
            tx.vout.append(CTxOut(a.value, SPK))
        for i, (_t, _v, _amt, pk, sk) in enumerate(ins):
            spk = wire.p2pkh_script(pk)
            sighash, err = SignatureHash(spk, tx, i, SIGHASH_ALL)
            assert err is None, err
            sig = coincurve.PrivateKey(sk).sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
            tx.vin[i].scriptSig = CScript([sig, pk])
        tx.rehash()
        raw = tx.serialize().hex()
        txid, err = rpc("sendrawtransaction", [raw])
        if err:
            sys.exit("funding rejected (%d outputs, %d bytes): %s"
                     % (batch, len(raw) // 2, str(err)[:160]))
        tid = int(txid, 16)
        made += [(tid, i) for i in range(batch)]
        print("  funded %d/%d (%d B tx)" % (len(made), n_out, len(raw) // 2), flush=True)
    # Every funded output must be CONFIRMED before the timed spends run. Left
    # unconfirmed, each funding transaction is an ancestor of several spends and
    # the run dies on `too-long-mempool-chain, exceeds descendant size limit`
    # at the larger input counts -- which reads as a limit on the thing being
    # measured rather than on the scaffolding. One block does not always hold
    # all of the funding, so mine until the mempool is actually empty.
    addr = key_to_p2pkh(PUBS[0])
    for _ in range(40):
        info, _e = rpc("getmempoolinfo", [])
        if info and info.get("size", 1) == 0:
            break
        rpc("generatetoaddress", [1, addr])
    else:
        sys.exit("funding would not confirm: mempool still occupied")
    return made


# ---- the timed spend ------------------------------------------------------

def spend(outpoints, bogus=False):
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    pts = list(outpoints)
    if bogus:
        pts[0] = (1, 0)                      # prevout that cannot exist
    for txid, vout in pts:
        tx.vin.append(CTxIn(COutPoint(txid, vout), b"", 0xffffffff))
    total = a.value * len(pts)
    fee = max(20000, (len(pts) * 700 + 300) * 3)
    out = (total - fee) // 2
    tx.vout.append(CTxOut(out, wire.p2pkh_script(PUBS[0])))
    tx.vout.append(CTxOut(out, wire.p2pkh_script(PUBS[1])))
    for i in range(len(pts)):
        sighash, err = SignatureHash(SCRIPTCODE, tx, i, SIGHASH_ALL)
        assert err is None, err
        sig = SIGNER.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
        tx.vin[i].scriptSig = script_sig(sig)
    tx.rehash()
    return tx


counts = [int(x) for x in a.counts.split(",")]
per_tx_fund = 400 if a.shape == "p2sh" else 120
need = sum(counts) * a.reps * 2
print("shape=%s keys=%d -> need %d funded outputs" % (a.shape, a.keys, need), flush=True)
pool = fund(need, per_tx_fund)
pcur = 0

print("%6s %9s %11s %11s %11s %9s %9s %11s"
      % ("inputs", "bytes", "valid_ms", "bogus_ms", "delta_ms", "us/input",
         "sigops", "us/sigop"))
for n in counts:
    vs, bs, size = [], [], 0
    for _ in range(a.reps):
        pts = pool[pcur:pcur + n]
        pcur += n
        tx = spend(pts)
        raw = tx.serialize().hex()
        size = len(raw) // 2
        t0 = time.time()
        _r, e = rpc("sendrawtransaction", [raw])
        vs.append((time.time() - t0) * 1000)
        if e and "already" not in str(e).lower() and "missing" not in str(e).lower():
            print("  n=%d rejected: %s" % (n, str(e)[:120]))
            vs.pop()
            break
        pts2 = pool[pcur:pcur + n]
        pcur += n
        txb = spend(pts2, bogus=True)
        t0 = time.time()
        rpc("sendrawtransaction", [txb.serialize().hex()])
        bs.append((time.time() - t0) * 1000)
    if not vs:
        continue
    v = statistics.median(vs)
    b = statistics.median(bs) if bs else 0.0
    # What the node charges: legacy counts this tx's two P2PKH outputs; the
    # P2SH pass charges the redeem script's accurate count per input, and
    # charges bare multisig nothing at all.
    sigops = 2 + (a.keys * n if a.shape == "p2sh" else 0)
    print("%6d %9d %11.1f %11.1f %11.1f %9.1f %9d %11.1f"
          % (n, size, v, b, v - b, (v - b) * 1000.0 / n, sigops,
             (v - b) * 1000.0 / sigops), flush=True)
