#!/usr/bin/env python3
"""Drive a DKG round at a pace the network can actually keep up with.

The reason three earlier attempts failed identically. On regtest the DKG phase is
derived purely from tip height -- CDKGSessionHandler::UpdatedBlockTip computes it as
quorumStageInt / dkgPhaseBlocks + 1 -- and CDKGSessionHandler::SleepBeforePhase
returns immediately when Params().MineBlocksOnDemand(), so nothing paces members.
CDKGSession::VerifyAndComplain then marks every member whose contributions are still
empty as bad, with no second chance.

Contributions do not broadcast: CDKGSession::Init builds relayMembers as a ring
(member i pushes to i+1 and i+2, see CLLMQUtils::GetQuorumRelayMembers), and each
hop is INV -> GETDATA -> QCONTRIB plus a BLS verify. Across US/EU/Asia the far side
of an 8-member ring is four hops, so a contribution needs seconds, not milliseconds.

Mining a 30-block window in under a minute gives the contribute phase about one
second. Every member except the local one is then marked bad and the round is dead
before it starts -- which is exactly what "badMembers 7, receivedContributions 1"
was telling us.

So: mine to the window boundary, then advance two blocks per phase and WAIT for the
network between them, gating on what each phase is supposed to produce rather than
on a timer.

    ./dkg-pace.py --miner dev --type llmq_test
"""
import argparse
import json
import subprocess
import sys
import time

DKG_INTERVAL = 30          # llmq_test / llmq_test_v17, quorums_parameters.h
PHASE_BLOCKS = 2           # dkgPhaseBlocks
MINING_START = 10          # dkgMiningWindowStart


def sh(args, timeout=120):
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def rpc(alias, *args, parse=True):
    out = sh(["./swarmctl.sh", "rpc", alias, *[str(a) for a in args]])
    if not parse:
        return out
    try:
        return json.loads(out)
    except Exception:
        return None


def aliases():
    return sh(["./swarmctl.sh", "aliases"]).split()


def smartnodes():
    """Aliases that are live smartnodes, i.e. can actually be quorum members."""
    out = []
    for a in aliases():
        st = rpc(a, "smartnode", "status")
        if st and st.get("state") == "READY":
            out.append(a)
    return out


def height(alias):
    v = rpc(alias, "getblockcount", parse=False)
    return int(v) if v.lstrip("-").isdigit() else -1


def session(alias, qtype):
    st = rpc(alias, "quorum", "dkgstatus")
    if not st:
        return None
    return (st.get("session") or {}).get(qtype)


def mine(miner, n, addr):
    rpc(miner, "generatetoaddress", n, addr, parse=False)


def wait_for(members, qtype, field, want, label, budget=90):
    """Poll every member until `field` reaches `want`, or the budget runs out.

    Returns the per-member values seen last, so a partial result is still reported
    rather than silently treated as success.
    """
    t0 = time.time()
    seen = {}
    while time.time() - t0 < budget:
        seen = {}
        for a in members:
            s = session(a, qtype)
            seen[a] = None if not s else s.get(field)
        vals = [v for v in seen.values() if isinstance(v, int)]
        if vals and min(vals) >= want:
            print("    %s: all %d members at %s>=%d after %.0fs"
                  % (label, len(vals), field, want, time.time() - t0))
            return True, seen
        time.sleep(3)
    got = ", ".join("%s=%s" % (a, seen.get(a)) for a in members)
    print("    %s: TIMEOUT after %.0fs waiting for %s>=%d -- %s"
          % (label, time.time() - t0, field, want, got))
    return False, seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--miner", default="dev", help="alias with a wallet that mines")
    ap.add_argument("--addr", required=True, help="address to mine to")
    ap.add_argument("--type", dest="qtype", default="llmq_test")
    ap.add_argument("--budget", type=int, default=90, help="seconds to wait per gate")
    ap.add_argument("--qsize", type=int, default=0,
                    help="quorum size; gates count THIS many members, not every smartnode")
    a = ap.parse_args()

    mns = smartnodes()
    print("live smartnodes: %d (%s)" % (len(mns), " ".join(mns)))
    if len(mns) < 2:
        sys.exit("need at least two READY smartnodes")

    h = height(a.miner)
    to_boundary = (DKG_INTERVAL - (h % DKG_INTERVAL)) % DKG_INTERVAL
    if to_boundary:
        print("mining %d blocks to the window boundary (from %d)" % (to_boundary, h))
        mine(a.miner, to_boundary, a.addr)
    qh = height(a.miner)
    print("window opens at height %d" % qh)

    # Phase 1 init -> 2 contribute -> 3 complain -> 4 justify -> 5 commit -> 6 mining.
    qsize = a.qsize or len(mns)
    gates = [
        (2, "receivedContributions",          qsize, "contribute"),
        (3, None,                             0,     "complain"),
        (4, None,                             0,     "justify"),
        (5, "receivedPrematureCommitments",   qsize, "commit"),
    ]
    print("gating on quorum size %d" % qsize)
    for phase, field, want, label in gates:
        mine(a.miner, PHASE_BLOCKS, a.addr)
        h = height(a.miner)
        print("  phase %d (%s) at height %d" % (phase, label, h))
        if field:
            wait_for(mns, a.qtype, field, want, label, a.budget)
        else:
            time.sleep(10)          # nothing to count; give the hop time anyway

    # Walk to the start of the mining window one block at a time, then wait for
    # the miner itself to hold a mineable commitment before mining it in.
    while height(a.miner) - qh < MINING_START:
        mine(a.miner, 1, a.addr)
    print("  mining window open at stage %d" % (height(a.miner) - qh))

    t0 = time.time()
    while time.time() - t0 < a.budget:
        st = rpc(a.miner, "quorum", "dkgstatus")
        if st and (st.get("mineableCommitments") or {}).get(a.qtype):
            print("    miner holds a mineable %s commitment after %.0fs" % (a.qtype, time.time() - t0))
            break
        time.sleep(3)
    else:
        print("    miner never saw a mineable commitment")

    # One block at a time, checking after each, while the window is still open.
    ql, n = {}, 0
    while height(a.miner) - qh <= 18:
        mine(a.miner, 1, a.addr)
        time.sleep(3)
        ql = rpc(a.miner, "quorum", "list") or {}
        n = len(ql.get(a.qtype) or [])
        if n:
            print("    commitment mined at stage %d" % (height(a.miner) - qh))
            break
    print("height %d -- %s quorums: %d" % (height(a.miner), a.qtype, n))
    if n:
        print(json.dumps(ql, indent=1)[:700])
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
