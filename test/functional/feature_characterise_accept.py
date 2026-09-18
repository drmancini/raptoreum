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
        """Submit and return the node's reason, or None if it was accepted."""
        block.rehash()
        return self.nodes[0].submitblock(ToHex(block))

    def check(self, name, mutate):
        try:
            block = self.base_block()
            mutate(block)
            block.solve()
            got = self.submit(block)
        except Exception as e:
            # A harness error is not a node behaviour; say which it is.
            self.observed[name] = "HARNESS ERROR: %s" % e
            self.log.info("  %-44s -> %s", name, self.observed[name])
            return
        self.observed[name] = got if got is not None else "ACCEPTED (no rejection)"
        self.log.info("  %-44s -> %s", name, self.observed[name])

    def take_tx(self):
        """A valid, signed, already-confirmed transaction to place in a block."""
        tx = self.pool[self.next_tx]
        self.next_tx += 1
        return tx

    # ---- mutations, one rule each ------------------------------------------

    def mutate_merkle_root(self, block):
        block.hashMerkleRoot += 1

    def mutate_duplicate_tx(self, block):
        # The same transaction twice, adjacent, which is what ComputeMerkleRoot's
        # `mutated` flag detects. A duplicate in NON-adjacent positions passes it
        # (F-43b), which is why the commitment format needs identifier
        # uniqueness as its own rule.
        tx = self.take_tx()
        block.vtx += [tx, tx]
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_no_coinbase(self, block):
        block.vtx = [self.take_tx()]
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_founder_payment(self, block):
        # Strip the founder output. CheckTransaction enforces this and it is the
        # ONLY place in the codebase that does, which is why F-45's fChecked
        # caching matters: the verdict depends on the height it was computed at.
        cb = block.vtx[0]
        cb.vout = cb.vout[:1]
        cb.calc_sha256()
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_coinbase_type(self, block):
        # A version-3 coinbase that is not a CbTx.
        cb = block.vtx[0]
        cb.nType = 0
        cb.vExtraPayload = b""
        cb.calc_sha256()
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_two_coinbases(self, block):
        second = create_coinbase(self.tip_height + 1)
        second.vout[0].scriptPubKey = CScript([OP_RETURN])
        second.calc_sha256()
        block.vtx.append(second)
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_tx_no_outputs(self, block):
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.take_tx().sha256, 0), b"", 0xffffffff))
        tx.vout = []
        tx.calc_sha256()
        block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_tx_negative_value(self, block):
        tx = self.take_tx()
        tx.vout[0].nValue = -1
        tx.calc_sha256()
        block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()

    def mutate_tx_duplicate_input(self, block):
        tx = self.take_tx()
        tx.vin.append(tx.vin[0])
        tx.calc_sha256()
        block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()


if __name__ == "__main__":
    CharacteriseAcceptTest().main()
