#!/usr/bin/env python3
"""D-18 redo: does ConnectBlock's real (crypto-bearing) per-sigop cost track
ATMP's, or does it show the same ~5x reduction F-94 found for a ZERO-sigop
shape? F-12 (130 us/sigop, n=150) and F-13 (196 us/sigop, n=800) are both ATMP
measurements; nobody has measured ConnectBlock directly for a sigop-heavy,
large-n transaction.

Fixed: 15 bare 1-of-15 multisig inputs, signed worst-case (every key tried) =
225 real sigops, regardless of padding. Varies: padding input count (bare
OP_TRUE spends, empty scriptSig, cheapest possible per input) from 0 up to
near K-3's ~100kB per-tx ceiling. For each point, measures BOTH the ATMP time
(node1, sendrawtransaction) AND the cold ConnectBlock time (node2, IBD-synced,
never ran the tx through its own mempool -- same two-node technique as F-94).
Timing is split into atmp_ms / mine_warm_ms (node1's own mining, WARM since it
just validated the tx via ATMP) / connect_cold_ms (node2's genuinely cold
path, timer started only after node1's mining call returns).

RESULT (2026-09-19, see docs/findings.md F-97, D-18 redone): unlike the
zero-sigop case, ConnectBlock does NOT get a big warm/cold-style discount here
-- at n_pad=0 ATMP and cold-connect are both close to F-12's 130 us/sigop,
because node2 has never validated this specific transaction either way (no
cache advantage exists to lose). But cost DOES climb with total input count,
confirmed at K-3's real ~100kB ceiling (2,310 padding + 15 multisig = 2,325
inputs, 96,511 B) with a 6-rep median: connect_cold ~72 ms = ~320 us/sigop,
consistent with a single-rep cross-check (~302 us/sigop). This is the FIRST
direct ConnectBlock measurement at the real worst-case shape, not an
extrapolation from ATMP points measured at much smaller n.
"""
import argparse, base64, hashlib, http.client, json, os, statistics, sys, time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/home/mike/forge/projects/raptoreum/test/perf/swarm")
sys.path.insert(0, "/home/mike/forge/projects/raptoreum/test/perf")
import coincurve, wire  # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut  # noqa: E402
from test_framework.script import (  # noqa: E402
    CScript, CScriptOp, OP_0, OP_TRUE, OP_CHECKMULTISIG, SIGHASH_ALL, SignatureHash,
)
from test_framework.address import key_to_p2pkh  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--port1", type=int, required=True)
ap.add_argument("--cookie1", required=True)
ap.add_argument("--port2", type=int, required=True)
ap.add_argument("--cookie2", required=True)
ap.add_argument("--pad-counts", default="0,785,2310")
ap.add_argument("--reps", type=int, default=1)
a = ap.parse_args()


def make_rpc(port, cookiepath):
    cookie = open(cookiepath).read().strip()
    HDR = {"Content-Type": "application/json",
           "Authorization": "Basic " + base64.b64encode(cookie.encode()).decode()}

    def rpc(m, p):
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=600)
        c.request("POST", "/", json.dumps({"jsonrpc": "1.0", "id": "d18", "method": m, "params": p}), HDR)
        d = json.loads(c.getresponse().read())
        c.close()
        if d.get("error"):
            sys.exit("rpc %s failed: %s" % (m, d["error"]))
        return d["result"]

    return rpc


rpc1 = make_rpc(a.port1, a.cookie1)
rpc2 = make_rpc(a.port2, a.cookie2)

FUND_KEY = coincurve.PrivateKey(hashlib.sha256(b"rtm-d18-fund-v1").digest())
FUND_PUB = FUND_KEY.public_key.format(compressed=True)
KEYS = [coincurve.PrivateKey(hashlib.sha256(b"rtm-d18-ms-v1" + i.to_bytes(4, "little")).digest())
        for i in range(15)]
PUBS = [k.public_key.format(compressed=True) for k in KEYS]
REDEEM = CScript([CScriptOp.encode_op_n(1)] + PUBS + [CScriptOp.encode_op_n(15), OP_CHECKMULTISIG])
SIGNER = KEYS[0]   # tried last in script order -> forces all 15 keys attempted
TRIVIAL_SPK = CScript([OP_TRUE])
PAD_VALUE = 600
MULTI_VALUE = 100000


def confirm_both():
    addr = rpc1("getnewaddress", [])
    for _ in range(120):
        if rpc1("getmempoolinfo", []).get("size", 1) == 0:
            break
        rpc1("generatetoaddress", [1, addr])
    else:
        sys.exit("node1 mempool would not confirm")
    h1 = rpc1("getblockcount", [])
    for _ in range(120):
        if rpc2("getblockcount", []) >= h1:
            return
        time.sleep(0.2)
    sys.exit("node2 did not catch up")


our_addr = key_to_p2pkh(FUND_PUB)
seed_txid = rpc1("sendtoaddress", [our_addr, 39.0])
confirm_both()
raw = rpc1("getrawtransaction", [seed_txid, True])
seed_vout = next(o["n"] for o in raw["vout"] if o["scriptPubKey"].get("hex", "") == wire.p2pkh_script(FUND_PUB).hex())
seed_value = int(round(next(o["value"] for o in raw["vout"] if o["n"] == seed_vout) * 100000000))

PAD_COUNTS = [int(x) for x in a.pad_counts.split(",")]
NEED_PAD = sum(PAD_COUNTS) * a.reps
NEED_MULTI = len(PAD_COUNTS) * a.reps * 15

print("funding %d pad + %d multisig outputs..." % (NEED_PAD, NEED_MULTI), flush=True)
pad_outpoints = []
multi_outpoints = []
cur_txid, cur_vout, cur_value = int(seed_txid, 16), seed_vout, seed_value
BATCH = 3000
remaining_pad = NEED_PAD
remaining_multi = NEED_MULTI
while remaining_pad > 0 or remaining_multi > 0:
    n_pad = min(BATCH - 15, remaining_pad)
    n_multi = min(15, remaining_multi) if remaining_pad <= 0 or n_pad < BATCH - 15 else 0
    outs = [(TRIVIAL_SPK, PAD_VALUE) for _ in range(n_pad)] + [(REDEEM, MULTI_VALUE) for _ in range(n_multi)]
    fee = 50000
    change = cur_value - sum(v for _s, v in outs) - fee
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    tx.vin.append(CTxIn(COutPoint(cur_txid, cur_vout), b"", 0xffffffff))
    tx.vout = [CTxOut(v, s) for s, v in outs]
    change_idx = None
    if change > 1000:
        tx.vout.append(CTxOut(change, wire.p2pkh_script(FUND_PUB)))
        change_idx = len(tx.vout) - 1
    sighash, err = SignatureHash(wire.p2pkh_script(FUND_PUB), tx, 0, SIGHASH_ALL)
    assert err is None, err
    sig = FUND_KEY.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
    tx.vin[0].scriptSig = CScript([sig, FUND_PUB])
    tx.rehash()
    new_txid_hex = rpc1("sendrawtransaction", [tx.serialize().hex()])
    confirm_both()
    new_txid = int(new_txid_hex, 16)
    for i in range(n_pad):
        pad_outpoints.append((new_txid, i))
    for i in range(n_multi):
        multi_outpoints.append((new_txid, n_pad + i))
    remaining_pad -= n_pad
    remaining_multi -= n_multi
    if change_idx is None:
        break
    cur_txid, cur_vout, cur_value = new_txid, change_idx, change
    print("  funded pad=%d/%d multi=%d/%d" % (len(pad_outpoints), NEED_PAD, len(multi_outpoints), NEED_MULTI),
          flush=True)

print("fanout ready: pad=%d multi=%d" % (len(pad_outpoints), len(multi_outpoints)), flush=True)


def build_spend(pad_pts, multi_pts):
    tx = CTransaction()
    tx.nVersion = wire.TX_VERSION
    for txid_i, vout_i in pad_pts:
        tx.vin.append(CTxIn(COutPoint(txid_i, vout_i), b"", 0xffffffff))
    for txid_i, vout_i in multi_pts:
        tx.vin.append(CTxIn(COutPoint(txid_i, vout_i), b"", 0xffffffff))
    total_in = len(pad_pts) * PAD_VALUE + len(multi_pts) * MULTI_VALUE
    fee = max(20000, (len(pad_pts) * 45 + len(multi_pts) * 200 + 300) * 3)
    out = (total_in - fee) // 2
    tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
    tx.vout.append(CTxOut(out, wire.p2pkh_script(FUND_PUB)))
    for i in range(len(pad_pts)):
        tx.vin[i].scriptSig = CScript([])
    for j in range(len(multi_pts)):
        idx = len(pad_pts) + j
        sighash, err = SignatureHash(REDEEM, tx, idx, SIGHASH_ALL)
        assert err is None, err
        sig = SIGNER.sign(sighash, hasher=None) + bytes([SIGHASH_ALL])
        tx.vin[idx].scriptSig = CScript([OP_0, sig])
    tx.rehash()
    return tx


print("%8s %9s %6s %11s %13s %15s %11s %11s" %
      ("pad_in", "bytes", "sigops", "atmp_ms", "mine_warm_ms", "connect_cold_ms", "us/sig(A)", "us/sig(C)"),
      flush=True)
pcur = 0
mcur = 0
for n in PAD_COUNTS:
    atmp_list, mine_list, connect_list = [], [], []
    size = sigops = 0
    for _ in range(a.reps):
        pad_pts = pad_outpoints[pcur:pcur + n]
        pcur += n
        multi_pts = multi_outpoints[mcur:mcur + 15]
        mcur += 15
        tx = build_spend(pad_pts, multi_pts)
        raw_hex = tx.serialize().hex()
        size = len(raw_hex) // 2
        sigops = 15 * 15  # 15 multisig inputs, 15 keys each, worst case

        t0 = time.time()
        rpc1("sendrawtransaction", [raw_hex])
        atmp_list.append((time.time() - t0) * 1000)

        # node1 mines WARM: it just validated this exact tx via ATMP above, so
        # its own TestBlockValidity/ConnectBlock benefits from the sig/script
        # cache F-16 already documents a 20-25x warm/cold swing for. Timed
        # separately so it isn't folded into node2's COLD figure.
        addr = rpc1("getnewaddress", [])
        h2_before = rpc2("getblockcount", [])   # before mining: no race with fast relay
        t0 = time.time()
        rpc1("generatetoaddress", [1, addr])
        mine_list.append((time.time() - t0) * 1000)

        # node2 never ran this tx through its own mempool -- the genuinely
        # cold ConnectBlock path, same technique as F-94. Timer starts only
        # after node1's own mining call returns, since node1 announces the
        # block to peers only once it has connected it locally.
        t1 = time.time()
        while True:
            if rpc2("getblockcount", []) > h2_before:
                break
            time.sleep(0.005)
        connect_list.append((time.time() - t1) * 1000)

    atmp_ms = statistics.median(atmp_list)
    mine_warm_ms = statistics.median(mine_list)
    connect_cold_ms = statistics.median(connect_list)
    print("%8d %9d %6d %11.1f %13.1f %15.1f %11.2f %11.2f"
          % (n, size, sigops, atmp_ms, mine_warm_ms, connect_cold_ms,
             atmp_ms * 1000.0 / sigops, connect_cold_ms * 1000.0 / sigops),
          flush=True)
