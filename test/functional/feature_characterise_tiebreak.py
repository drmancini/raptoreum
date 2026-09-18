#!/usr/bin/env python3
"""Pin how the node breaks a tie between two chains of equal work.

Build-plan item 0.2. This is the only behaviour the 0.1 probe was measured to
CHANGE (F-35, F-42), so it is the one place where 1.3 can alter what a node does
without altering what it accepts -- and nothing else in the suite would notice.

The rule today: CBlockIndexWorkComparator orders by nChainWork, then by lower
nSequenceId, then by pointer address. nSequenceId is assigned in
ReceivedBlockTransactions, so it means FIRST SEEN. Two blocks of equal work on
the same parent therefore resolve to whichever arrived first, and the second
never displaces it.

Why 1.3 threatens it from both directions:

  re-stamping   ReceivedBlockTransactions runs again when bodies arrive and
                re-assigns nSequenceId to the block and its whole descendant
                subtree, so a branch that arrived first can lose a tie it had
                already won (F-35).
  not re-stamping
                a block whose COMMITMENTS arrived early keeps that early id, so
                when its bodies land it can displace an equal-work rival that
                was already connected -- which unmodified code never does
                (F-42). A miner could reserve a tie by announcing commitments
                early and bodies late.

Neither is "unchanged", so the current behaviour has to be written down.

Run against an upstream-equivalent tree: the rig branch carries the probe and an
8 MB block size (F-58).
"""
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import ToHex
from test_framework.mininode import P2PDataStore, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

DKG_INTERVAL = 30       # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End


class CharacteriseTiebreakTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]

        # A hand-built block inside the DKG mining window is rejected
        # bad-qc-missing, because create_coinbase does not add the null quorum
        # commitments the window requires (F-63). Stop where the NEXT height is
        # clear of it, so the tie-break is the only thing being tested.
        self.log.info("priming to a height outside the DKG mining window")
        addr = node.getnewaddress()
        node.generatetoaddress(200, addr)
        while not self.next_height_is_clear(node.getblockcount() + 1):
            node.generatetoaddress(1, addr)
        self.log.info("  tip %d, next height %d is outside the window %s",
                      node.getblockcount(), node.getblockcount() + 1, MINING_WINDOW)

        self.bootstrap_p2p()
        self.first_seen_wins()
        self.log.info("=" * 64)
        self.log.info("equal-work tie-break: FIRST SEEN wins, and the loser does")
        self.log.info("not displace it on arrival. Assigned in")
        self.log.info("ReceivedBlockTransactions; compared in CBlockIndexWorkComparator.")
        self.log.info("=" * 64)

    def next_height_is_clear(self, h):
        stage = h % DKG_INTERVAL
        return not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1])

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)

    def competing_pair(self, node):
        """Two valid blocks on the same parent, so their work is identical."""
        tip = int(node.getbestblockhash(), 16)
        height = node.getblockcount() + 1
        t = node.getblock(node.getbestblockhash())["time"] + 1
        first = create_block(tip, create_coinbase(height), t)
        first.solve()
        # A different coinbase output makes a distinct block at the same height,
        # height and difficulty -- so equal work, and the tie-break decides.
        second_cb = create_coinbase(height)
        second_cb.vin[0].nSequence = 0xfffffffe
        second_cb.sha256 = None
        second_cb.calc_sha256()
        second = create_block(tip, second_cb, t)
        second.solve()
        assert first.sha256 != second.sha256, "the pair must be distinct blocks"
        return first, second

    def first_seen_wins(self):
        node = self.nodes[0]
        first, second = self.competing_pair(node)

        self.log.info("sending the first of two equal-work blocks")
        node.submitblock(ToHex(first))
        assert_equal(node.getbestblockhash(), first.hash)

        self.log.info("sending the second: it must NOT displace the first")
        node.submitblock(ToHex(second))
        assert_equal(node.getbestblockhash(), first.hash)

        # Both are known, and the loser sits at equal height on a side branch.
        tips = {t["hash"]: t for t in node.getchaintips()}
        assert first.hash in tips, "the winner should be a chain tip"
        assert second.hash in tips, "the loser should still be known as a tip"
        assert_equal(tips[first.hash]["status"], "active")
        self.log.info("  winner %s active; loser %s status=%s",
                      first.hash[:16], second.hash[:16], tips[second.hash]["status"])
        assert_equal(tips[first.hash]["height"], tips[second.hash]["height"])


if __name__ == "__main__":
    CharacteriseTiebreakTest().main()
