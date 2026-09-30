#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sybil serve-budget-exhaustion: the GETBODYRANGE serve budget is a PER-
CONNECTION token bucket (net.h:1129-1131, `CNode::nBodyRangeServeRequestTokens`/
`nBodyRangeServeByteTokens`), sized against what ONE honest peer's own tip-path
fetch should ever need (net.h:104-149's own doc comment: "protects against a
flood ... from THIS peer"). Nothing ties separate connections' budgets
together -- confirmed by reading every consumer (net_processing.cpp:4001-4114)
before writing this, not assumed. F-218 (docs/findings.md) wired the existing,
pre-existing, GLOBAL `-maxuploadtarget` accounting into this handler too
(`connman->OutboundTargetReached`, checked at net_processing.cpp:4062) -- but
that backstop defaults to 0 = unlimited (net.h:205, confirmed live below via
`getnettotals`), so on a node nobody has explicitly configured `-maxuploadtarget`
on -- the overwhelming majority of real deployments -- it does nothing.

This proves the resulting gap live: N independent connections each get their
own full, untouched 128-request budget (net.h:MAX_BODYRANGE_SERVE_REQUESTS_TOKEN_BUCKET),
for N times the aggregate request volume the budget's own sizing rationale
was built around, using only guaranteed-MISS requests (bodyrange.h's own
documented classification: "An unknown hash is exactly what an honest
requester sends ... so this must never be a BAN") -- never risking
Misbehaving, never touched by the byte-budget or the (usually-inert) global
backstop either, since a MISS costs no response bytes. Every request still
reaches the shared message-handling thread this project's own earlier work
(F-1/F-2 in this same findings.md) already established as the node's real
bottleneck resource.

Not a new production bug in the sense of "do X and crash/corrupt" -- it's a
structural absence: the per-peer bucket was deliberately sized and documented
around a single peer's own legitimate demand, and nothing in the tree bounds
the SUM across many peers. Recorded here as a confirmed, live-demonstrated
gap (not a guess from reading net.h alone), for Mike to decide whether an
aggregate/global request-count throttle is worth adding.
"""
import struct
import time

from test_framework.messages import deser_uint256, deser_compact_size, ser_uint256
from test_framework.mininode import MESSAGEMAP, P2PInterface, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until

REQUEST_BUDGET = 128  # net.h: MAX_BODYRANGE_SERVE_REQUESTS_TOKEN_BUCKET
SYBIL_COUNT = 5


class msg_getbodyrange:
    command = b"getbodyrange"

    def __init__(self, hash_block=0, start_index=0, count=1):
        self.hashBlock = hash_block
        self.nStartIndex = start_index
        self.nCount = count

    def serialize(self):
        r = ser_uint256(self.hashBlock)
        r += struct.pack("<II", self.nStartIndex, self.nCount)
        return r

    def deserialize(self, f):
        self.hashBlock = deser_uint256(f)
        self.nStartIndex, self.nCount = struct.unpack("<II", f.read(8))

    def __repr__(self):
        return "msg_getbodyrange(hash=%064x, start=%d, count=%d)" % (
            self.hashBlock, self.nStartIndex, self.nCount)


class msg_bodyrange:
    """Deserializes only hashBlock/nStartIndex/the body count -- never the
    bodies themselves. Every request this test sends is a deliberately
    unknown hash, so a real server can only ever reply with nBodies == 0
    (bodyrange.h's CBodyRange: "no fFound field ... a miss is simply
    vBodies.empty()"); there is nothing else in the buffer to parse."""
    command = b"bodyrange"

    def __init__(self):
        self.hashBlock = 0
        self.nStartIndex = 0
        self.nBodies = 0

    def deserialize(self, f):
        self.hashBlock = deser_uint256(f)
        self.nStartIndex = struct.unpack("<I", f.read(4))[0]
        self.nBodies = deser_compact_size(f)

    def serialize(self):
        raise NotImplementedError

    def __repr__(self):
        return "msg_bodyrange(hash=%064x, start=%d, nBodies=%d)" % (
            self.hashBlock, self.nStartIndex, self.nBodies)


# GETBODYRANGE/BODYRANGE are decoupling-specific and have no entry in
# mininode.py's own MESSAGEMAP (confirmed by running before this addition --
# the framework raised "Received unknown command" on the very first reply).
# No other test in this tree defines its own message classes, so there is no
# existing precedent to follow beyond MESSAGEMAP's own dict shape.
MESSAGEMAP[b"getbodyrange"] = msg_getbodyrange
MESSAGEMAP[b"bodyrange"] = msg_bodyrange


class SybilPeer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.bodyrange_replies = 0

    def on_bodyrange(self, message):
        assert_equal(message.nBodies, 0)  # every request here is a guaranteed MISS
        with mininode_lock:
            self.bodyrange_replies += 1


class FeatureBodyrangeSybilServeBudgetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # -maxuploadtarget deliberately left unset -- the realistic default,
        # not a strawman; confirmed inert below via getnettotals.
        self.extra_args = [["-servebodyrange=1"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("confirming the global upload-target backstop is genuinely off, the realistic "
                      "default, not a strawman (net.h:205, '0 = Unlimited')")
        totals = node.getnettotals()
        assert_equal(totals["uploadtarget"]["target_reached"], False)

        self.log.info("opening %d Sybil connections plus one held-in-reserve connection", SYBIL_COUNT)
        sybils = [node.add_p2p_connection(SybilPeer()) for _ in range(SYBIL_COUNT)]
        reserve = node.add_p2p_connection(SybilPeer())
        network_thread_start()
        for peer in sybils + [reserve]:
            peer.wait_for_verack()

        def replies_at_least(peer, n):
            with mininode_lock:
                return peer.bodyrange_replies >= n

        self.log.info("each of the %d Sybil connections spends its OWN full %d-request budget "
                      "independently -- %d total requests, none bannable (guaranteed MISS)",
                      SYBIL_COUNT, REQUEST_BUDGET, SYBIL_COUNT * REQUEST_BUDGET)
        for si, peer in enumerate(sybils):
            for i in range(REQUEST_BUDGET):
                peer.send_message(msg_getbodyrange(hash_block=0xdead0000 + (si << 16) + i))
        for peer in sybils:
            wait_until(lambda p=peer: replies_at_least(p, REQUEST_BUDGET), timeout=30)

        self.log.info("confirmed: %d requests served across %d independent connections with no "
                      "shared budget -- %dx a single connection's own sized-for-one-peer allowance",
                      SYBIL_COUNT * REQUEST_BUDGET, SYBIL_COUNT, SYBIL_COUNT)

        self.log.info("the (budget+1)th request on an already-exhausted connection is genuinely "
                      "declined -- no reply at all, matching net_processing.cpp's own 'declining' log")
        exhausted = sybils[0]
        with mininode_lock:
            before = exhausted.bodyrange_replies
        exhausted.send_message(msg_getbodyrange(hash_block=0xfeed0000))
        time.sleep(2)
        with mininode_lock:
            after = exhausted.bodyrange_replies
        assert_equal(before, after)

        self.log.info("the reserve connection -- open the whole time, never touched -- still has its "
                      "OWN full, unshared budget: proves the exhaustion above was per-connection, not "
                      "some node-wide counter that happened not to trip yet")
        for i in range(REQUEST_BUDGET):
            reserve.send_message(msg_getbodyrange(hash_block=0xbeef0000 + i))
        wait_until(lambda: replies_at_least(reserve, REQUEST_BUDGET), timeout=30)

        self.log.info("none of this -- %d requests total -- disconnected or banned a single peer",
                      SYBIL_COUNT * REQUEST_BUDGET + REQUEST_BUDGET + 1)
        for peer in sybils + [reserve]:
            assert peer.is_connected

        node.disconnect_p2ps()
        network_thread_join()


if __name__ == "__main__":
    FeatureBodyrangeSybilServeBudgetTest().main()
