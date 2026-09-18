#!/usr/bin/env python3
"""Pin what the node rejects a block for, and at which stage.

Build-plan item 0.2, the part that matters most to 1.3. Transaction decoupling
changes when a block's transactions are available, so it has to either preserve
each of these checks where it is or relocate it deliberately. A check that
silently stops running is the failure mode the whole design has to avoid, and it
is invisible unless the current behaviour is written down first.

This is a CHARACTERISATION test, not a specification. Every assertion records
what the node does today. If a change makes one fail, the question is not "which
assertion is wrong" but "was this change intended to move this rule".

The rows map onto docs/transaction-decoupling.md section 2.4, which classifies
each check by whether a commitment block -- header, coinbase in full, and a list
of identifiers -- can perform it at all:

    commitment-checkable   the rung can keep it
    body-dependent         needs the transactions, so it moves to connect
                           time, and three of them have no connect-time home
                           today (F-44): nLockTime finality, transaction
                           type/version, and the 100 kB per-transaction cap

Run against an UNMODIFIED tree. The perf rig branch carries an 8 MB
MAX_DIP0001_BLOCK_SIZE and the 0.1 probe's changes to validation.cpp, so
characterising there would pin the rig rather than the node (F-46b).
"""
from test_framework.blocktools import create_block, create_coinbase, create_transaction
from io import BytesIO
from test_framework.messages import CBlock, CTransaction, CTxIn, CTxOut, COutPoint, ToHex
from test_framework.script import CScript, OP_TRUE, OP_CHECKSIG, OP_RETURN
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class CharacteriseAcceptTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        self.observed = {}

        # Let the NODE build the priming chain. Hand-built blocks are rejected with
        # bad-qc-missing from height 10: regtest opens a DKG mining window and a
        # block inside it must carry the null quorum commitments, which
        # create_coinbase does not add. And the wallet supplies the transactions
        # for the mutations -- already signed and valid -- so each mutation is the
        # only fault in the block, which is the whole point.
        self.bootstrap_p2p()
        self.log.info("priming past the founder-payment start height (500)")
        addr = node.getnewaddress()
        # Past 500: regtest's founder payment starts there (FounderPayment(..., 500, ...)),
        # and below it the amount is zero and the coinbase has a single output -- so a
        # mutation that strips the founder output is a no-op on an unmutated block,
        # which reads as "the rule is not enforced" when nothing was tested at all.
        node.generatetoaddress(550, addr)
        self.pool = []
        for _ in range(8):
            txid = node.sendtoaddress(node.getnewaddress(), 1)
            tx = CTransaction()
            tx.deserialize(BytesIO(bytes.fromhex(node.getrawtransaction(txid))))
            tx.rehash()
            self.pool.append(tx)
        # The pool stays UNCONFIRMED on purpose: mining it would spend those inputs
        # and every reuse would fail as inputs-missing-or-spent, which is not the
        # rule under test. A hand-built base block carries only the coinbase, so
        # the mempool never leaks into it.
        self.tip_hash = int(node.getbestblockhash(), 16)
        self.tip_height = node.getblockcount()
        self.tip_time = node.getblock(node.getbestblockhash())["time"]
        self.next_tx = 0
        self.log.info("  tip %d, %d valid transactions in the pool", self.tip_height, len(self.pool))

        # --- commitment-checkable rows: a commitment block could still do these ---
        self.check("merkle root", self.mutate_merkle_root)
        self.check("merkle malleation, duplicate transaction", self.mutate_duplicate_tx)
        self.check("first transaction is a coinbase", self.mutate_no_coinbase)
        self.check("coinbase: founder payment", self.mutate_founder_payment)
        self.check("coinbase: DIP3 type", self.mutate_coinbase_type)

        # --- body-dependent rows: these need the transactions ---
        self.check("a second coinbase", self.mutate_two_coinbases)
        self.check("CheckTransaction: no outputs", self.mutate_tx_no_outputs)
        self.check("CheckTransaction: negative output", self.mutate_tx_negative_value)
        self.check("CheckTransaction: duplicate input", self.mutate_tx_duplicate_input)

        self.log.info("=" * 68)
        self.log.info("characterised rejection reasons (docs/transaction-decoupling.md 2.4)")
        for name, got in self.observed.items():
            self.log.info("  %-44s %s" % (name, got))
        self.log.info("  not characterisable on regtest: header proof of work")
        self.log.info("    minimal difficulty means an unsolved header usually meets the target,")
        self.log.info("    so the mutation cannot express the rule being violated")
        self.log.info("=" * 68)

        accepted = [n for n, g in self.observed.items() if g.startswith("ACCEPTED")]
        broken = [n for n, g in self.observed.items() if g.startswith("HARNESS")]
        assert not broken, "harness could not express: %s" % ", ".join(broken)
        assert not accepted, (
            "these rules are NOT enforced at accept time, though section 2.4 assumes "
            "they are: %s" % ", ".join(accepted))

    # ---- harness -----------------------------------------------------------

    def base_block(self):
        """A block that would be accepted, so each mutation is the only fault."""
        coinbase = create_coinbase(self.tip_height + 1)
        block = create_block(self.tip_hash, coinbase, self.tip_time + 1)
        return block

    def submit(self, block):
        """Submit over RPC. Coarse, but fine for priming where only success matters."""
        block.rehash()
        return self.nodes[0].submitblock(ToHex(block))

    def submit_p2p(self, block):
        """Send over p2p and return the node's own reject reason, or None if accepted.

        This is the door that carries the reason. send_blocks_and_test records
        whatever arrives; asking it to ASSERT a reason would make this a
        specification, and the point is to find out what the node says.
        """
        block.rehash()
        p2p = self.nodes[0].p2p
        with mininode_lock:
            p2p.reject_code_received = None
            p2p.reject_reason_received = None
        before = self.nodes[0].getbestblockhash()
        try:
            p2p.send_blocks_and_test([block], self.nodes[0], success=False, request_block=True, timeout=10)
        except AssertionError:
            # The tip advanced, i.e. the node accepted it.
            if self.nodes[0].getbestblockhash() != before:
                return None
        with mininode_lock:
            reason = p2p.reject_reason_received
        return reason.decode() if isinstance(reason, bytes) else (reason or "rejected, no reason given")

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)

    def reconnect_p2p(self):
        """Every row here is a DoS-scoring block, so the node disconnects us each
        time. Without reconnecting, the first row is characterised and the other
        eight report 'Not connected' -- which looks like a harness that works."""
        self.nodes[0].disconnect_p2ps()
        network_thread_join()
        self.bootstrap_p2p()

    def check(self, name, mutate):
        try:
            self.reconnect_p2p()
            block = self.base_block()
            mutate(block)
            block.solve()
            got = self.submit_p2p(block)
        except Exception as e:
            # A harness error is not a node behaviour; say which it is.
            self.observed[name] = "HARNESS ERROR: %s" % e
            self.log.info("  %-44s -> %s", name, self.observed[name])
            return
        self.observed[name] = got if got is not None else "ACCEPTED (no rejection)"
        self.log.info("  %-44s -> %s", name, self.observed[name])

    def reseal(self, block):
        """Refresh every transaction hash, then rebuild the merkle root.

        Without the refresh the root commits to the transactions as they were
        before the mutation, so the node reports bad-txnmrklroot and the rule
        under test is never reached -- which reads as though the merkle check is
        the only thing the node enforces.
        """
        for tx in block.vtx:
            tx.sha256 = None
            tx.hash = None
            tx.calc_sha256()
        block.hashMerkleRoot = block.calc_merkle_root()

    def take_tx(self):
        """A valid, signed, already-confirmed transaction to place in a block."""
        tx = self.pool[self.next_tx]
        self.next_tx += 1
        return tx

    # ---- mutations, one rule each ------------------------------------------

    def mutate_merkle_root(self, block):
        block.hashMerkleRoot += 1

    def mutate_duplicate_tx(self, block):
        # ComputeMerkleRoot compares hashes[pos] with hashes[pos+1] only for EVEN
        # pos, so a duplicate pair has to land on an even boundary to be seen.
        # [coinbase, tx, tx] puts the pair at positions 1,2 and is never compared:
        # the block then sails past the malleation check entirely. Four leaves put
        # the pair at 2,3, where it is caught. This is narrower than "non-adjacent
        # duplicates pass" (F-43b) -- the requirement is even alignment, which
        # makes identifier uniqueness a strictly necessary rung rule.
        a, b = self.take_tx(), self.take_tx()
        block.vtx += [a, b, b]
        self.reseal(block)

    def mutate_no_coinbase(self, block):
        block.vtx = [self.take_tx()]
        self.reseal(block)

    def mutate_founder_payment(self, block):
        # Strip the founder output. CheckTransaction enforces this and it is the
        # ONLY place in the codebase that does, which is why F-45's fChecked
        # caching matters: the verdict depends on the height it was computed at.
        cb = block.vtx[0]
        cb.vout = cb.vout[:1]
        self.reseal(block)

    def mutate_coinbase_type(self, block):
        # A version-3 coinbase that is not a CbTx.
        cb = block.vtx[0]
        cb.nType = 0
        cb.vExtraPayload = b""
        self.reseal(block)

    def mutate_two_coinbases(self, block):
        second = create_coinbase(self.tip_height + 1)
        second.vout[0].scriptPubKey = CScript([OP_RETURN])
        block.vtx.append(second)
        self.reseal(block)

    def mutate_tx_no_outputs(self, block):
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.take_tx().sha256, 0), b"", 0xffffffff))
        tx.vout = []
        block.vtx.append(tx)
        self.reseal(block)

    def mutate_tx_negative_value(self, block):
        tx = self.take_tx()
        tx.vout[0].nValue = -1
        block.vtx.append(tx)
        self.reseal(block)

    def mutate_tx_duplicate_input(self, block):
        tx = self.take_tx()
        tx.vin.append(tx.vin[0])
        block.vtx.append(tx)
        self.reseal(block)


if __name__ == "__main__":
    CharacteriseAcceptTest().main()
