#!/usr/bin/env python3
"""Prove a missing body gets re-requested instead of stranded (1.3.6, F-111).

F-25e2 (found on the acceptance-layer probe, never actually fixed until
1.3.6): FindNextBlocksToDownload's cursor (pindexLastCommonBlock) is only
supposed to advance past a block once "all ancestors are already
downloaded". But ReceivedBlockTransactions counts a commitment-only block's
commitments as "downloaded" for nChainTx -- decoupling's whole point is that
the chain can extend on commitments alone -- so a FULLY BODIED descendant of
a still-gapped block also reads HaveTxsDownloaded()==true, and the old code
let the cursor jump straight past the gap onto it. Once past, nothing walks
back: the observed behaviour was "re-requested twice, then dropped".

This test builds exactly that shape -- a commitment-only PARENT with a
fully-bodied CHILD already sitting above it -- entirely through the P2P
interface a real peer would use (headers, then blocks), and proves the node
asks for the parent's body a SECOND time rather than only once. Without the
fix, the second getdata never arrives and this test times out.

-perfwithholdcount (1.3.6, F-111) makes the node's own accept path pretend a
matching block's body isn't held for exactly N attempts, then let it
through -- simulating "genuinely couldn't get it yet, but a later retry
succeeds" live, in one session, rather than only across a restart (F-40b's
own open question).

Run against the rig branch (F-58's 8 MB block size; matches the other
feature_characterise_*.py tests, none of which run against an unmodified
tree either once 1.3's own code is what's under test).
"""
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
PARENT_HEIGHT = 201       # 200 primed blocks + 1; 201 % 30 == 21, outside the window


class FeatureBodyRefetchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-perfwithholdheight=%d" % PARENT_HEIGHT,
            "-perfwithholdcount=2",
        ]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("priming to a height outside the DKG mining window")
        addr = node.getnewaddress()
        node.generatetoaddress(200, addr)
        assert_equal(node.getblockcount() + 1, PARENT_HEIGHT)
        stage = PARENT_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
            "PARENT_HEIGHT must sit outside the DKG mining window"

        node.add_p2p_connection(P2PDataStore())
        network_thread_start()
        node.p2p.wait_for_verack()

        tip = int(node.getbestblockhash(), 16)
        t = node.getblock(node.getbestblockhash())["time"] + 1
        parent = create_block(tip, create_coinbase(PARENT_HEIGHT), t)
        parent.solve()
        child = create_block(parent.sha256, create_coinbase(PARENT_HEIGHT + 1), t + 1)
        child.solve()

        # Populate the store BEFORE announcing anything -- on_getdata only
        # answers a request if the block is already there, and the node may
        # ask within milliseconds of the headers arriving.
        node.p2p.block_store[parent.sha256] = parent
        node.p2p.block_store[child.sha256] = child

        self.log.info("announcing both headers -- a real peer would, and the node "
                      "must request both bodies on its own initiative")
        node.p2p.send_message(msg_headers([CBlockHeader(parent), CBlockHeader(child)]))

        def requests_for(block_hash):
            with mininode_lock:
                return len([h for h in node.p2p.getdata_requests if h == block_hash])

        wait_until(lambda: requests_for(parent.sha256) >= 1, timeout=10)
        wait_until(lambda: requests_for(child.sha256) >= 1, timeout=10)

        # Convergence in this in-process, all-local setup is near-instant once
        # it happens at all (every retry round-trip is sub-millisecond), so
        # there is no reliable intermediate state to pause on and check --
        # the meaningful assertion is that it converges at all. Without the
        # fix, F-25e2's exact symptom is that it does NOT: the cursor jumps
        # the gap the first time the child's presence is seen, the parent is
        # never asked for again, and this wait times out with the tip stuck
        # at PARENT_HEIGHT - 1.
        self.log.info("waiting for convergence -- F-25e2's fix means the parent keeps "
                      "getting re-requested until the withhold count exhausts, rather "
                      "than being stranded once the fully-bodied child exists")
        wait_until(lambda: node.getblockcount() == PARENT_HEIGHT + 1, timeout=20)
        assert_equal(node.getbestblockhash(), child.hash)

        # And it must have taken genuine re-fetching to get there, not luck:
        # -perfwithholdcount=2 means the parent's body could only be accepted
        # on (at least) a second delivery.
        parent_requests = requests_for(parent.sha256)
        self.log.info("parent was requested %d time(s) before converging", parent_requests)
        assert parent_requests >= 2, \
            "expected at least 2 getdata requests for the withheld parent (count=2), saw %d" % parent_requests

        node.disconnect_p2ps()
        network_thread_join()


if __name__ == "__main__":
    FeatureBodyRefetchTest().main()
