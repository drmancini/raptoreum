#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mike, 2026-09-30: "what I'm interested in is if a node bad actor starts to
push altered bodies to other nodes ... have we tried something like this?"

Earlier this session, yes, but only via a raw-socket script pretending to be
one peer for one exchange (a scripted GETBODYRANGE/BODYRANGE round trip).
This is the real version: a genuine second raptoreumd process whose own
on-disk body record has been deliberately altered -- readable, self-
consistent, wrong, the same swap-two-bodies technique acceptancebit_tests.cpp's
own body_record_at_rest_detects_a_hash_mismatch_from_swapped_bodies and
F-233's own quarantine test already use for exactly this shape of corruption
-- serving it to a real, honest peer over a real GETBODYRANGE/BODYRANGE wire
exchange, not a mocked one.

The alteration is byte-for-byte on the server's own bdy*.dat file, done
while the malicious daemon keeps running (no stop/restart in between): find
both real transactions' own serialized bytes (same size by construction --
two sendtoaddress calls, same structure, same amount, only the destination
differs) and swap them in place. This is deliberately indistinguishable, at
the wire level, from F-233's own "silently corrupted local data" scenario --
the honest peer's own ValidateBodyRangeChunkHashes check cannot know or care
whether the mismatch it finds was caused by bit rot or by a bad actor typing
the swap on purpose. What differs is intent, not mechanism, and the fix this
test is proving already existed before F-233: net_processing.cpp's own
Misbehaving(peer, 100, "sent a bodyrange chunk that does not match") call,
now confirmed against a real altered file on a real second process rather
than assumed from reading the handler.

No restart: tried that first, and it is worth recording why it does not
model a bad actor. validation.cpp's VerifyDB (init.cpp:2632, run against
any non-empty coinsview on every ordinary startup, not just -reindex) calls
VerifyBodyRecordAtRest on the chain tip UNCONDITIONALLY -- the comment at
validation.cpp:6217 says so explicitly, "not gated behind nCheckLevel" --
and there is no -checkblocks/-checklevel combination that skips the tip
block itself, confirmed live: restarting the malicious node after the same
swap produced "Corrupted block database detected" on ITS OWN startup, before
it ever reached the network code that would serve the block to a peer. A
real bad actor does not run the stock, unmodified honest binary and get
caught by its own boot-time self-check; they run their own build, or they
never restart the process between corrupting it and serving it. Corrupting
the file live, with no restart, is the faithful version of that -- and also
confirms VerifyDB's own tip-inclusive, unconditional check is a genuine
defense-in-depth property for anyone running the real binary normally: the
only window where a stock, never-modified node can unknowingly serve
corrupted-at-rest body data is the interval between it happening while
already running and F-233's own 10-minute ScrubBodyStoreAtRest sweep
catching it -- which this test's timing (seconds, not minutes) sits inside
of, and which is exactly the gap the wire-level ValidateBodyRangeChunkHashes
check on the FETCHER side exists to cover regardless.

Built on feature_bodyrange_e2e.py's own two-node shape and feature_decoupled_
assets_futures.py's own disconnect-before-mining fix (a still-connected,
already-synced peer would recover the withheld block via ordinary compact-
block relay, never routing through GETBODYRANGE at all, and never validating
against ValidateBodyRangeChunkHashes -- confirmed live while building that
earlier test, not assumed here).
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, connect_nodes, disconnect_nodes, get_chain_folder, wait_until

import os
import time

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
PARENT_HEIGHT = 201       # bodyrange_e2e.py's own precedent height, outside the DKG window
WITHHOLD_COUNT = 1


class FeatureMaliciousBodyServerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [
                # honest: never sees the real body except via GETBODYRANGE
                "-fetchbodyrange=1",
                "-perfwithholdheight=%d" % PARENT_HEIGHT,
                "-perfwithholdcount=%d" % WITHHOLD_COUNT,
            ],
            [
                # malicious: mines real content, then serves ALTERED bytes
                "-commitmentblocks=1",
                "-servebodyrange=1",
            ],
        ]

    def run_test(self):
        honest, malicious = self.nodes

        self.log.info("priming the malicious node alone to a height outside the DKG mining window")
        addr = malicious.getnewaddress()
        malicious.generatetoaddress(PARENT_HEIGHT - 1, addr)
        stage = PARENT_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
            "PARENT_HEIGHT must sit outside the DKG mining window"

        self.log.info("connecting the honest node and letting it IBD-sync the primed chain normally")
        connect_nodes(honest, 1)
        wait_until(lambda: honest.getblockcount() == PARENT_HEIGHT - 1, timeout=30)
        assert_equal(honest.getbestblockhash(), malicious.getbestblockhash())

        self.log.info("disconnecting before mining -- an already-synced peer recovers via ordinary "
                      "compact-block relay, never exercising GETBODYRANGE or its hash check at all "
                      "(confirmed live while building feature_decoupled_assets_futures.py)")
        disconnect_nodes(honest, 1)

        self.log.info("mining a block with two real, same-size transactions to swap")
        # ECDSA signatures vary by a byte or two depending on the random nonce (DER length, low-S
        # encoding) -- confirmed live: the first attempt at this produced 226 vs 225 bytes. A same-
        # size pair is required for an in-place swap that doesn't shift anything after it in the
        # file, so generate a small pool and pick any two that happen to match.
        raw_by_len = {}
        raw1 = raw2 = None
        for _ in range(20):
            txid = malicious.sendtoaddress(malicious.getnewaddress(), 1)
            raw = bytes.fromhex(malicious.getrawtransaction(txid))
            existing = raw_by_len.get(len(raw))
            if existing is not None and existing != raw:
                raw1, raw2 = existing, raw
                break
            raw_by_len[len(raw)] = raw
        assert raw1 is not None, "failed to find two same-size, different transactions in 20 tries"
        assert_equal(len(raw1), len(raw2))
        malicious.generatetoaddress(1, addr)
        assert_equal(malicious.getblockcount(), PARENT_HEIGHT)

        self.log.info("the bad actor's own alteration: swap the two transactions' raw bytes directly "
                      "in the served node's own bdy*.dat file, live, no restart -- readable, "
                      "self-consistent, wrong, and never seen by the malicious node's own VerifyDB "
                      "(that only runs once, at startup, which this deliberately avoids -- see the "
                      "module docstring for why a restart here would test the wrong thing)")
        chain_folder = get_chain_folder(malicious.datadir, malicious.chain)
        body_file = os.path.join(malicious.datadir, chain_folder, "blocks", "bdy00000.dat")
        with open(body_file, "r+b") as f:
            data = f.read()
            pos1 = data.find(raw1)
            pos2 = data.find(raw2)
            assert pos1 != -1 and pos2 != -1, "both raw transactions must be found in the body file"
            assert pos1 != pos2
            f.seek(pos1)
            f.write(raw2)
            f.seek(pos2)
            f.write(raw1)
        # every body read opens its own fresh CAutoFile (bodystore.cpp:196-197, confirmed before
        # relying on it) rather than keeping a long-lived cached handle, so this external write is
        # visible to the still-running malicious process's very next read with no reopen needed.

        self.log.info("reconnecting -- the honest node is genuinely behind by one block, forcing a "
                      "real GETBODYRANGE/BODYRANGE round trip against the now-altered server")
        # net_processing.cpp:4262-4264, confirmed by direct read just before writing this: the exact
        # call is Misbehaving(pfrom->GetId(), 100, strprintf("Peer %d sent a bodyrange chunk that does
        # not match %s", ...)), logged (LogPrint, BCLog::NET) as "...BAN THRESHOLD EXCEEDED: Peer %d
        # sent a bodyrange chunk that does not match %s". Asserting on the unique, un-guessed substring.
        with honest.assert_debug_log(["sent a bodyrange chunk that does not match"]):
            connect_nodes(honest, 1)
            # connect_nodes() itself only waits for the version handshake (util.py), which completes
            # before the header sync + GETBODYRANGE round trip that actually triggers the mismatch --
            # observed live, ~28ms apart. Wait on rpc/net.cpp's own real, RPC-visible banscore field
            # (pushKV("banscore", statestats.nMisbehavior), confirmed by direct read) rather than race
            # the debug-log check against however long that round trip takes.
            wait_until(lambda: any(p.get("banscore", 0) > 0 for p in honest.getpeerinfo()), timeout=30)

        self.log.info("confirmed: the honest node detected the altered body via ValidateBodyRangeChunk"
                      "Hashes on the very first mismatched chunk (0 -> 100, BAN THRESHOLD EXCEEDED "
                      "immediately) -- Misbehaving(peer, 100, \"sent a bodyrange chunk that does not "
                      "match\"), the same mechanism this session's earlier raw-socket script exercised, "
                      "now proven against a real second process serving a genuinely altered file, not "
                      "a scripted single response.")
        # NOT asserting a torn-down connection or listbanned(): first run of this test showed why both
        # would be asserting the wrong thing here, checked live rather than assumed --
        # connect_nodes() connects via addnode(..., "onetry") (util.py), which net_processing.cpp's own
        # fShouldBan consumer treats as pnode->m_manual_connection == true, and that branch is checked
        # BEFORE the addr.IsLocal() branch: the actual log line produced was "Warning: not punishing
        # manually-connected peer 2!", not the local-peer warning this test originally assumed -- on a
        # manually-added peer (as any two nodes in a test harness necessarily are, and per Bitcoin-
        # lineage convention on any real node's own -connect / addnode use), Misbehaving's own ban path
        # deliberately never sets fDisconnect or calls the banman at all. A real, normally-discovered
        # (non-manual) peer connection is not exempted and would be torn down immediately; that specific
        # difference is a connection-management detail, not the security property this test is for.
        #
        # The property that actually matters -- the corrupted chunk is never accepted into the chain --
        # doesn't depend on any of that, so assert it directly, then hold for a few seconds and assert
        # it again to rule out some later retry quietly succeeding on bad data.
        assert_equal(honest.getblockcount(), PARENT_HEIGHT - 1)
        # a real sleep, not bump_mocktime: the retry cadence observed live runs on wall-clock time
        # (Misbehaving fired again at +1s/+2s/+4s/+8s real seconds in the run that found this), not
        # mocktime, so only an actual wait lets a few more real retry-and-reject rounds happen.
        time.sleep(8)
        assert_equal(honest.getblockcount(), PARENT_HEIGHT - 1)


if __name__ == "__main__":
    FeatureMaliciousBodyServerTest().main()
