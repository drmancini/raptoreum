#!/usr/bin/env python3
"""Prove -fetchbodyrange/-servebodyrange actually work end to end over real
P2P wire messages between two independent node processes (F-159's own noted
gap: "no functional test exercises -fetchbodyrange/-servebodyrange end to
end yet").

feature_body_refetch.py (F-111) already proves a withheld body eventually
gets re-requested -- but neither it nor feature_body_refetch_backoff.py ever
sets -fetchbodyrange/-servebodyrange or mentions the GETBODYRANGE/BODYRANGE
message types (confirmed by grep before writing this test), so with neither
flag set, that re-request is always the whole-block GETDATA fallback. This
test builds the same shape -- a real withheld body, forced to retry -- but
with -fetchbodyrange on a real fetching node and -servebodyrange on a real,
separately-processed serving node, and asserts the wire messages that only
exist on THIS path (GETBODYRANGE/BODYRANGE) genuinely round-trip between two
node processes, not a mocked P2P peer (P2PDataStore, as the existing tests
use, cannot exercise this: the real serving handler -- net_processing.cpp's
GETBODYRANGE dispatch arm, ValidateGetBodyRange/BuildBodyRangeResponse,
F-218's new per-connection serving budget -- only exists in the C++ node).

This test's own single legitimate GETBODYRANGE/BODYRANGE round trip also
exercises F-218's new serving-side budget's happy path directly (refill,
gate, spend) -- the budget's own boundary/decline behaviour is covered at
the unit level (bodyrange_tests.cpp), matching this project's established
split of pure logic (unit-tested) from net_processing.cpp wiring
(exercised, not boundary-tested, at the functional level -- see
feature_body_refetch.py's own precedent for the same split).

The server (node1) advertises NODE_COMMITMENTS via -commitmentblocks (the
real format has no activation height yet -- 4.6.1 still unbuilt,
commitments_negotiation.h's own ShouldNegotiateCommitmentsNow doc comment),
which is what makes the fetcher's own fFetchBodyRangeCapable gate
(net_processing.cpp) true for this specific peer. The fetcher (node0) needs
only its own -fetchbodyrange flag for that same gate's other half.

WITHHOLD_COUNT is deliberately 1, not 2+ -- a real, reproducible harness
interaction was found building this test (not fixed, recorded in
docs/findings.md's F-218 entry as a scope note): with count >= 2, this
node's OWN GETBODYRANGE fetch-completion path (ProcessFetchedBodyRange)
writes the real body it received from the server WITHOUT ever consulting
-perfwithholdcount's own attempt counter (that path exists to recover a
body a REAL peer withheld, not to participate in this harness's
simulation), so HaveBodies() becomes true while the harness's count is
still > 0 -- and ReadBlockFromDisk's own non-decrementing peek
(PerfWithholdStillActive, validation.cpp) then still reports "withheld"
for the READ-BACK ConnectTip performs immediately afterward, which is a
fatal AbortNode ("Failed to read block"), not a graceful decline: verified
directly, reproduced with count=2 before settling on count=1 here. Fixing
PerfWithholdStillActive to defer to HaveBodies() was tried and reverted --
it silently breaks three existing, deliberately-designed corruption-
simulation tests (acceptancebit_tests.cpp's verifydb_stops_at_a_bodies_gap.../
node_round_voting_getvote_..., commitmentblock_tests.cpp's
commitments_readable_when_bodies_are_not), which use this exact harness to
simulate "the index believes it has bodies but the underlying bytes are
genuinely unreadable" -- a real, load-bearing use case incompatible with
"HaveBodies() true implies readable". count=1 avoids the collision
entirely: AcceptBlock's own PerfWithholdBodies call withholds exactly once
(the count reaches 0 by the time GETBODYRANGE's response arrives), so by
the time ConnectTip reads the recovered body back, the harness's own
peek already agrees it is no longer withheld -- genuinely exercising one
full commitment-only-then-GETBODYRANGE-recovers cycle without touching
this DEBUG_ONLY test-harness's own cross-feature composition bug.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, connect_nodes, wait_until

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
PARENT_HEIGHT = 201       # 200 primed blocks + 1; 201 % 30 == 21, outside the window
WITHHOLD_COUNT = 1        # see the module doc above for why this is 1, not 2+


class FeatureBodyRangeE2ETest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-fetchbodyrange=1",
                "-perfwithholdheight=%d" % PARENT_HEIGHT,
                "-perfwithholdcount=%d" % WITHHOLD_COUNT,
            ],
            [
                "-commitmentblocks=1",
                "-servebodyrange=1",
            ],
        ]

    def run_test(self):
        fetcher, server = self.nodes

        self.log.info("priming the server alone to a height outside the DKG mining window")
        addr = server.getnewaddress()
        server.generatetoaddress(PARENT_HEIGHT - 1, addr)
        stage = PARENT_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
            "PARENT_HEIGHT must sit outside the DKG mining window"

        self.log.info("connecting the fetcher and letting it IBD-sync the primed chain normally "
                      "(none of these heights match -perfwithholdheight)")
        connect_nodes(fetcher, 1)
        wait_until(lambda: fetcher.getblockcount() == PARENT_HEIGHT - 1, timeout=30)
        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())

        self.log.info("giving the withheld block a real (non-coinbase) transaction -- an empty, "
                      "coinbase-only block completes with NO wire round trip at all (F-158's own "
                      "nWanted==0 short-circuit, net_processing.cpp), which would make this test "
                      "pass trivially without ever exercising GETBODYRANGE/BODYRANGE")
        server.sendtoaddress(server.getnewaddress(), 1)

        self.log.info("mining the withheld block and requiring a real GETBODYRANGE/BODYRANGE "
                      "round trip -- not the whole-block GETDATA fallback -- to recover it")
        with fetcher.assert_debug_log(["received: bodyrange"]), \
             server.assert_debug_log(["received: getbodyrange"]):
            server.generatetoaddress(1, addr)
            wait_until(lambda: fetcher.getblockcount() == PARENT_HEIGHT, timeout=30)

        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())

        # The two assert_debug_log blocks above already require a genuine
        # GETBODYRANGE (server received) / BODYRANGE (fetcher received) round
        # trip to have occurred for this test to pass at all -- F-158's own
        # fFetchBodyRangeCapable gate routed the withheld block's retries
        # through this path rather than the whole-block GETDATA fallback
        # feature_body_refetch.py's own (flagless) test exercises instead.
        self.log.info("confirmed: a real GETBODYRANGE/BODYRANGE round trip recovered the "
                      "withheld body between two independent node processes")


if __name__ == "__main__":
    FeatureBodyRangeE2ETest().main()
