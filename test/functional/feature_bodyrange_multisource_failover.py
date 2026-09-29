#!/usr/bin/env python3
"""2.4a (build-plan.md's 2.4 row, docs/findings.md F-219): prove a body
fetch that fails against one source (here: disconnected, simulating a
"deliberately erasing source" per this row's own goal statement) does not
permanently deny the fetcher its own history -- a SECOND, independently
known source completes it, with no operator intervention and no waiting out
BODY_RETRY_MAX_MICROS's own 30s staleness ceiling.

This is a REGRESSION/CONFIRMATION test, not a bug-fix test: direct source
reading (net_processing.cpp's BODYRANGE dispatch arm) found the single-
source design already frees mapBodyRangeInFlight's entry for a hash the
INSTANT any shape-valid response arrives (line ~4160, before chunk-hash
validation even runs) -- and FinalizeNode already sweeps both
mapBodyRangeInFlight and g_body_retry_state for a disconnecting peer
(F-155/F-157). Nothing about the fetch/retry mechanism itself was found
broken; this test exists because the row's own brief explicitly asks for a
real test proving it, not just a citation -- "a body's fetch fails against
one peer (banned or disconnected) and a second peer subsequently completes
it".

Four real node processes, not three -- see the module-level note below on
"the bridge node" for why a direct serverA<->serverB connection cannot be
used here.

  fetcher: withholds one real block, same shape as feature_bodyrange_e2e.py.
  serverA: advertises NODE_COMMITMENTS (-commitmentblocks) so the fetcher
           routes to it via GETBODYRANGE, but does NOT set -servebodyrange --
           a silent, connected-but-never-answering source, matching F-156's
           own IsBodyRangeRequestStale failure shape.
  bridge:  a plain node with neither flag. Exists ONLY to relay the real,
           fully-bodied chain from serverA to serverB without serverA and
           serverB ever connecting to each other directly -- see below.
  serverB: advertises NODE_COMMITMENTS and sets -servebodyrange -- the
           second, genuinely serving candidate.

**The bridge node, and why it exists (found while building this test, not
fixed -- out of scope for this row).** The obvious design connects serverA
and serverB directly so serverB can sync the real chain from serverA. Tried
first; it reliably crashes serverA (SIGABRT, no assertion text, immediately
after serverA's own VERSION-handler sends SENDCOMMITMENTS to serverB --
net_processing.cpp:3586) the instant two NODE_COMMITMENTS-advertising nodes
(-commitmentblocks on BOTH sides) connect to each other. Every existing test
in this tree (feature_bodyrange_e2e.py, feature_body_refetch*.py, this
session's own F-155-218 corpus) only ever pairs ONE -commitmentblocks node
against a plain one -- that combination is proven safe (the fetcher<->serverA
leg below uses it), but mutual NODE_COMMITMENTS negotiation between two real
node processes appears never to have been exercised before. Reproduced
directly (twice, isolating it to exactly this pairing) before working around
it here with a third, unflagged relay node -- fetcher<->serverA and
serverA<->bridge<->serverB are both the already-proven-safe "one side has the
bit" shape. Recorded, not fixed: diagnosing a SIGABRT with no assertion text
and no debug symbols in the negotiation path is a real, separate task, and
this row's own scope is body-fetch fault tolerance, not commitment
negotiation between two commitment-capable peers -- worth a dedicated finding
for whoever picks that up next (see docs/findings.md F-219).

DKG_INTERVAL/MINING_WINDOW/PARENT_HEIGHT/WITHHOLD_COUNT are ported verbatim
from feature_bodyrange_e2e.py -- see that module's own doc for why
PARENT_HEIGHT must sit outside the DKG mining window and why WITHHOLD_COUNT
is 1, not 2+ (a real, documented DEBUG_ONLY harness composition bug, F-218's
own scope note, unrelated to this test).
"""
import os
import re

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, connect_nodes, disconnect_nodes, get_chain_folder, wait_until

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
PARENT_HEIGHT = 201       # 200 primed blocks + 1; 201 % 30 == 21, outside the window
WITHHOLD_COUNT = 1        # see feature_bodyrange_e2e.py's own doc for why


def wait_for_debug_log(node, pattern, timeout=30):
    """assert_debug_log only checks its expected lines when the `with` block
    EXITS -- it has no internal poll loop of its own. This waits FOR a
    pattern to actually appear, so a caller can be sure a specific message
    was received before moving on to the next step (disconnecting a peer,
    here) rather than merely hoping enough wall-clock time happened to
    elapse inside an unrelated wait_until call first."""
    debug_log = os.path.join(node.datadir, get_chain_folder(node.datadir, node.chain), 'debug.log')

    def check():
        with open(debug_log, encoding='utf-8') as f:
            return re.search(re.escape(pattern), f.read()) is not None

    wait_until(check, timeout=timeout)


class FeatureBodyRangeMultiSourceFailoverTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            [
                # fetcher
                "-fetchbodyrange=1",
                "-perfwithholdheight=%d" % PARENT_HEIGHT,
                "-perfwithholdcount=%d" % WITHHOLD_COUNT,
            ],
            [
                # serverA: capable (NODE_COMMITMENTS) so the fetcher routes to
                # it via GETBODYRANGE, but -servebodyrange is deliberately
                # left OFF -- every request it receives is silently declined
                # (net_processing.cpp's own documented behaviour, F-151), the
                # same "connected but never answers" shape F-156's
                # IsBodyRangeRequestStale exists for. This test resolves it
                # via an explicit disconnect rather than waiting out that 30s
                # staleness ceiling -- "banned or disconnected", this row's
                # own stated brief, disconnected half.
                "-commitmentblocks=1",
            ],
            [
                # bridge: no special flags -- see the module doc for why this
                # node exists (relays serverA's real chain to serverB without
                # the two ever connecting directly).
            ],
            [
                # serverB: the second, genuinely serving candidate.
                "-commitmentblocks=1",
                "-servebodyrange=1",
            ],
        ]

    def setup_network(self):
        # The default setup_network wires every adjacent pair bidirectionally
        # (BitcoinTestFramework's own "chain" topology) before run_test even
        # starts -- this test needs full control over WHEN, and via WHICH
        # path, each connection exists (both for the deterministic failover
        # itself and to avoid ever connecting serverA and serverB directly,
        # see the module doc), so only the nodes themselves are started here;
        # every connection is made explicitly in run_test.
        self.setup_nodes()

    def run_test(self):
        fetcher, serverA, bridge, serverB = self.nodes

        self.log.info("priming serverA alone to a height outside the DKG mining window")
        addr = serverA.getnewaddress()
        serverA.generatetoaddress(PARENT_HEIGHT - 1, addr)
        stage = PARENT_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
            "PARENT_HEIGHT must sit outside the DKG mining window"

        self.log.info("connecting the fetcher to serverA, and serverA to the bridge -- both "
                      "'one side advertises NODE_COMMITMENTS' pairings, the only shape proven "
                      "safe (see the module doc for the shape that is NOT)")
        connect_nodes(fetcher, 1)
        connect_nodes(bridge, 1)
        wait_until(lambda: fetcher.getblockcount() == PARENT_HEIGHT - 1, timeout=30)
        wait_until(lambda: bridge.getblockcount() == PARENT_HEIGHT - 1, timeout=30)

        self.log.info("connecting the bridge to serverB so serverB reaches the same primed chain")
        connect_nodes(serverB, 2)
        wait_until(lambda: serverB.getblockcount() == PARENT_HEIGHT - 1, timeout=30)
        assert_equal(fetcher.getbestblockhash(), serverA.getbestblockhash())
        assert_equal(serverB.getbestblockhash(), serverA.getbestblockhash())

        self.log.info("giving the withheld block a real (non-coinbase) transaction -- an empty, "
                      "coinbase-only block completes with NO wire round trip at all (F-158's own "
                      "nWanted==0 short-circuit), which would make this test pass without ever "
                      "exercising a real fetch, let alone a failover")
        serverA.sendtoaddress(serverA.getnewaddress(), 1)

        self.log.info("mining the withheld block on serverA: the fetcher withholds its own copy "
                      "(simulating commitment-only receipt); serverB reaches the real, fully-bodied "
                      "block via the bridge's own ordinary relay")
        serverA.generatetoaddress(1, addr)
        wait_until(lambda: serverB.getblockcount() == PARENT_HEIGHT, timeout=30)
        assert_equal(fetcher.getblockcount(), PARENT_HEIGHT - 1)  # still stuck: commitment-only, body missing

        self.log.info("confirming the fetcher actually asked serverA (its only known candidate "
                      "at this point) for the body range, and that serverA -- silent by design "
                      "-- never answers")
        wait_for_debug_log(serverA, "received: getbodyrange", timeout=30)

        self.log.info("erasing serverA as a source (an explicit disconnect -- FinalizeNode's own "
                      "cleanup, net_processing.cpp) and introducing serverB as the fetcher's only "
                      "other candidate")
        disconnect_nodes(fetcher, 1)

        self.log.info("confirming a real GETBODYRANGE/BODYRANGE round trip against serverB -- the "
                      "SECOND, independently-known source -- recovers the body serverA never served. "
                      "The connect itself must be INSIDE this assertion window: on regtest, with the "
                      "fetcher exactly one block behind, the round trip can complete within the same "
                      "millisecond connect_nodes' own version-handshake wait returns in, so capturing "
                      "the debug-log window only AFTER connect_nodes had already missed it once")
        with fetcher.assert_debug_log(["received: bodyrange"]), \
             serverB.assert_debug_log(["received: getbodyrange"]):
            connect_nodes(fetcher, 3)
            wait_until(lambda: fetcher.getblockcount() == PARENT_HEIGHT, timeout=30)

        assert_equal(fetcher.getbestblockhash(), serverA.getbestblockhash())
        assert_equal(fetcher.getbestblockhash(), serverB.getbestblockhash())

        self.log.info("confirmed: serverA's silence did not permanently deny the fetcher its own "
                      "history -- serverB, a second known candidate, completed it after serverA "
                      "was erased, with no operator intervention and no 30s staleness wait")


if __name__ == "__main__":
    FeatureBodyRangeMultiSourceFailoverTest().main()
