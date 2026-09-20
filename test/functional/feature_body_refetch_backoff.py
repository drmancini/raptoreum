#!/usr/bin/env python3
"""Prove a permanently-unresolved body gets rate-limited, not hammered (H-2).

A second adversarial review of 1.3.6 (2026-09-20, after F-111/F-113 shipped)
found that the existing peer-timeout-and-reassignment machinery those two
findings leaned on only detects a peer going SILENT. It does nothing when a
peer keeps answering but the answer never resolves the gap -- exactly the
shape -perfwithholdcount(forever) or a real Phase 2 body that keeps failing
validation would take. MarkBlockAsReceived frees the in-flight slot on every
response regardless of outcome, so FindNextBlocksToDownload re-requests on
the very next cycle with no pacing at all.

Measured before the fix: with a single peer and a permanently-withheld
block, a bit over one thousand getdata requests per second for that one
block, unbounded. This test proves the fix: request 0 always goes through
immediately (no artificial delay on first contact), but the retry rate is
bounded and small over a real, several-second sampling window.

feature_body_refetch.py proves eventual convergence once a withhold count
is configured; this test proves the OTHER property -- what happens when it
never converges at all.
"""
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import wait_until

DKG_INTERVAL = 30
MINING_WINDOW = (10, 18)
PARENT_HEIGHT = 201          # same priming as feature_body_refetch.py
SAMPLE_SECONDS = 8
# The base backoff is 1s (BODY_RETRY_BASE_MICROS, net_processing.cpp),
# doubling up to a 30s cap -- an unbounded-request bug would produce
# thousands in this window; a healthy one produces a small, fixed number
# independent of how long the sample runs.
MAX_EXPECTED_REQUESTS = 20


class FeatureBodyRefetchBackoffTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # No -perfwithholdcount: this block is withheld forever.
        self.extra_args = [["-perfwithholdheight=%d" % PARENT_HEIGHT]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("priming to a height outside the DKG mining window")
        addr = node.getnewaddress()
        node.generatetoaddress(200, addr)
        stage = PARENT_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1])

        node.add_p2p_connection(P2PDataStore())
        network_thread_start()
        node.p2p.wait_for_verack()

        tip = int(node.getbestblockhash(), 16)
        t = node.getblock(node.getbestblockhash())["time"] + 1
        parent = create_block(tip, create_coinbase(PARENT_HEIGHT), t)
        parent.solve()

        node.p2p.block_store[parent.sha256] = parent
        node.p2p.send_message(msg_headers([CBlockHeader(parent)]))

        def requests_for(block_hash):
            with mininode_lock:
                return len([h for h in node.p2p.getdata_requests if h == block_hash])

        self.log.info("first request must arrive without any artificial delay")
        wait_until(lambda: requests_for(parent.sha256) >= 1, timeout=10)

        self.log.info("sampling the request rate for %ds -- must stay bounded, not climb without limit",
                      SAMPLE_SECONDS)
        n0 = requests_for(parent.sha256)
        time.sleep(SAMPLE_SECONDS)
        n1 = requests_for(parent.sha256)
        rate = n1 - n0
        self.log.info("  %d requests in %ds (%.1f/s)", rate, SAMPLE_SECONDS, rate / SAMPLE_SECONDS)
        assert rate <= MAX_EXPECTED_REQUESTS, \
            "expected a bounded, backed-off retry rate (<= %d over %ds), saw %d -- " \
            "the per-block backoff (net_processing.cpp, H-2) is not engaging" % (
                MAX_EXPECTED_REQUESTS, SAMPLE_SECONDS, rate)

        node.disconnect_p2ps()
        network_thread_join()


if __name__ == "__main__":
    FeatureBodyRefetchBackoffTest().main()
