#!/usr/bin/env python3
"""Pin what the node does with a block it knows about but does not hold.

Build-plan item 0.2. This is the state transaction decoupling creates on purpose,
and pruning is the only way to reach it on an unmodified node: the index entry
survives with nTx > 0 while BLOCK_HAVE_DATA is cleared, which is exactly
"I know this block exists, I know where it sits, and I cannot produce it".

Two things are being characterised, and they pull in opposite directions for 1.3.

  the graceful half   chain selection, block serving and the RPC surface already
                      cope with a known-but-absent block, which is why 0.1 found
                      the acceptance layer's hard half already built (F-25f).

  the trap            AcceptBlock reads `nTx != 0` as "a previously-processed
                      block that was pruned" and returns early for anything
                      UNREQUESTED, and the compact-block handler does the same
                      (F-36). Under decoupling that same test means "I hold the
                      commitments", so a body arriving unrequested -- or by
                      compact-block relay, the mainline path -- is DISCARDED.
                      The node has the data offered to it and throws it away.

Run against an upstream-equivalent tree (F-58).
"""
from test_framework.messages import CBlock, FromHex, ToHex, msg_block
from test_framework.mininode import P2PDataStore, network_thread_start
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, mine_large_block, wait_until
import os

PRUNE_AFTER_HEIGHT = 1000     # CRegTestParams
MIN_BLOCKS_TO_KEEP = 288      # validation.h


class CharacterisePrunedArrivalTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Manual pruning: -prune=1 lets the test decide when, via pruneblockchain.
        self.extra_args = [["-prune=1", "-txindex=0"]]

    def run_test(self):
        node = self.nodes[0]

        # Pruning deletes whole block FILES (128 MiB each), and a file goes only
        # when everything in it is below the target. 1,348 coinbase-only regtest
        # blocks all fit in blk00000.dat, so nothing can ever be pruned -- and
        # pruneblockchain still returns the target and sets pruned=true while
        # doing nothing at all. That is how the first version of this test came
        # to characterise an unpruned chain.
        #
        # So: mine blocks with real data until a SECOND file exists.
        self.log.info("mining past PruneAfterHeight (%d)", PRUNE_AFTER_HEIGHT)
        addr = node.getnewaddress()
        want = PRUNE_AFTER_HEIGHT + MIN_BLOCKS_TO_KEEP + 60
        while node.getblockcount() < want:
            node.generatetoaddress(min(150, want - node.getblockcount()), addr)
        self.log.info("  height %d", node.getblockcount())

        # Keep the bytes for a spread of candidate heights while everything is
        # still available: the loop below prunes as it searches, so anything
        # captured afterwards is already gone.
        candidate_heights = (100, 300, 500, 700, 900, 1100, 1300)
        kept = {h: node.getblock(node.getblockhash(h), 0) for h in candidate_heights}

        blockdir = os.path.join(node.datadir, "regtest", "blocks")
        # A second file is necessary but not sufficient: blk00000.dat spans every
        # early height AND the large blocks that filled it, so it still contains
        # blocks above any legal prune target -- MIN_BLOCKS_TO_KEEP forbids a
        # target within 288 of the tip. The file can only go once the tip is more
        # than 288 beyond its last block. Rather than compute that, drive to the
        # condition and let the node report when it is met.
        self.log.info("filling blocks until a prune actually removes something")
        utxos = []
        for attempt in range(40):
            for _ in range(50):
                mine_large_block(node, utxos)
            tip = node.getblockcount()
            node.pruneblockchain(tip - MIN_BLOCKS_TO_KEEP - 1)
            ph = node.getblockchaininfo().get("pruneheight", 0)
            files = len([f for f in os.listdir(blockdir) if f.startswith("blk")])
            self.log.info("  height %d, %d files, pruneheight %s", tip, files, ph)
            if ph and ph > 0:
                break
        else:
            raise AssertionError("could not get the node to prune anything")

        tip = node.getblockcount()
        prune_height = node.getblockchaininfo()["pruneheight"]
        # Candidates below the line the node has actually pruned to.
        candidates = [h for h in candidate_heights if h < prune_height]

        info = node.getblockchaininfo()
        self.log.info("  pruned=%s, pruneheight=%s", info["pruned"], prune_height)
        gone = list(candidates)
        assert gone, ("nothing below pruneheight=%s among %s -- pruning removed no "
                      "file covering these heights, so there is nothing to characterise"
                      % (prune_height, candidates))
        target_height = gone[-1]
        target_hash = node.getblockhash(target_height)
        raw = kept[target_height]
        header = node.getblockheader(target_hash)
        assert header["nTx"] > 0, "the entry must know its transaction count"
        self.log.info("  characterising height %d, below pruneheight %d",
                      target_height, prune_height)

        # ---- the state itself --------------------------------------------
        h = node.getblockheader(target_hash)
        assert_equal(h["nTx"], header["nTx"])
        self.log.info("index entry survives: height %d still reports nTx=%d",
                      target_height, h["nTx"])

        assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                node.getblock, target_hash)
        self.log.info("  getblock  -> Block not available (pruned data)")

        # The header is still served from the index, so "known" and "held" are
        # already distinct facts on the RPC surface.
        assert_equal(node.getblockheader(target_hash)["height"], target_height)
        self.log.info("  getblockheader -> still answers, from the index")

        # ---- the trap: offer the block back, unrequested ------------------
        self.bootstrap_p2p()
        block = FromHex(CBlock(), raw)
        block.rehash()
        assert_equal(block.hash, target_hash)

        self.log.info("offering the pruned block back, unrequested")
        self.nodes[0].p2p.send_message(msg_block(block))
        self.nodes[0].p2p.sync_with_ping()

        # If the node had stored it, getblock would now succeed. It does not:
        # AcceptBlock returns early on `nTx != 0` for an unrequested block.
        assert_raises_rpc_error(-1, "Block not available (pruned data)",
                                node.getblock, target_hash)
        self.log.info("  still not available: the block was DISCARDED")

        self.log.info("=" * 70)
        self.log.info("characterised: a known-but-absent block")
        self.log.info("  index keeps the entry and its transaction count")
        self.log.info("  getblockheader answers; getblock refuses")
        self.log.info("  an UNREQUESTED body for it is silently discarded")
        self.log.info("  -> under decoupling this same path discards bodies the")
        self.log.info("     node was offered and needs (F-36)")
        self.log.info("=" * 70)

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)


if __name__ == "__main__":
    CharacterisePrunedArrivalTest().main()
