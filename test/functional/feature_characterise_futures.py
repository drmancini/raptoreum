#!/usr/bin/env python3
"""Pin what the node accepts and rejects for TRANSACTION_FUTURE, and at which stage.

Build-plan item 0.2's full-scope reopening (F-216). feature_futures.py already
covers the maturity/lockTime spending rule in depth (validateFutureCoin,
consensus/tx_verify.cpp) and the SPORK_22 fee gate's happy path -- read it
first, it is the reference for this whole feature and this file does not
repeat its coverage. This file's job is narrower and different in kind,
matching feature_characterise_accept.py's own technique: pin the
ACCEPT/REJECT boundary of the FUTURE payload's own FIELDS -- what makes
CheckFutureTx (evo/providertx.cpp:82-104) itself reject a structurally
malformed future, as opposed to what makes a mature/immature SPEND of one
succeed or fail (feature_futures.py's own subject).

CFutureTx (evo/providertx.h:204-226) carries no signature at all -- unlike
every asset-op payload, there is no owner key to verify -- so every row here
can use a technique feature_characterise_assets.py's asset rows could not
for their signature-gated types: take a genuine, node-produced future
transaction (sendtoaddress's own future={...} parameter,
rpc/rawtransaction_util.cpp:144-168) and patch exactly one field of its
vExtraPayload in place (test_framework.specialtx_bytes.PayloadCursor), then
resubmit inside a hand-built block. CheckSpecialTx runs before script/
signature verification on both the mempool and block-connect paths, so the
patch invalidating the outer transaction's own signature is never reached --
CheckFutureTx's own verdict fires first, which is exactly the row under test.

Per docs/transaction-decoupling.md 2.4's classification, every FUTURE rule is
body-dependent (needs vExtraPayload, present only with the full transaction) --
matching 2.4's own general "special transactions ... already runs at
connect" framing, since CheckSpecialTx only dispatches once a whole
transaction, not a bare identifier, is in hand.

**checkSpecialTxFee's masking.** checkSpecialTxFee (consensus/tx_verify.cpp:
32-50) is the SAME shared function for every special-tx type, called from
Consensus::CheckTxInputs (validation.cpp:858, mempool; :2524, connect) BEFORE
CheckSpecialTx's own dispatch (validation.cpp:953) -- a fee mismatch there
produces the generic "bad-txns-wrong-future-fee-or-not-enable" for EITHER a
future OR an asset (the literal string names only "future", despite covering
both types, F-216 notes this as a naming quirk). For assets, this gate is
unavoidable: an inactive SPORK_22 makes createasset itself refuse ("Under
maintenance"), so every asset row needed the spork armed and fee=100 threaded
through by hand. Futures are different: getFutureFees() (future/fee.cpp:
14-19) returns 0 outright while SPORK_22 is inactive -- its DEFAULT regtest
state (feature_futures.py confirms: 4070908800, a year-2099 style off-switch)
-- and checkSpecialTxFee's own condition (`futureEnabled && fFeeVerify &&
...`) short-circuits to true (no rejection) whenever the spork is off, since
`futureEnabled` is false. So every field-boundary row above runs with
SPORK_22 at its default OFF state and fee=0, and the masking gate never
engages at all -- and check_fee_masking (below) demonstrates the masking
itself as its own, deliberately isolated row, built via createrawtransaction/
signrawtransactionwithwallet (which never broadcast) specifically to avoid
leaving a spork-dependent, soon-to-be-stale transaction sitting in the
mempool -- see that function's own docstring for the real node crash
(CTxMemPool::check's assert(fCheckResult), txmempool.cpp:1155) building this
row surfaced the first way, and F-216 for the full writeup.

**Not characterisable on regtest**: `future-not-enabled` (CheckFutureTx's own
gate, evo/providertx.cpp:84-86) needs IsFutureActive(Tip()) to read false.
consensus.nFutureForkBlock is 1 on regtest (chainparams.cpp) -- the future
soft fork is active from the first block after genesis, so there is no
reachable height at which to observe this row, the same class of gap
feature_characterise_accept.py's own header-PoW row notes.

Run against an UNMODIFIED tree (F-46b, F-58): the perf rig branch never
touches evo/providertx.cpp or consensus/tx_verify.cpp's future-specific code,
so this is expected identical there, but the discipline applies regardless.
"""
import os
import re
from io import BytesIO

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTransaction, FromHex
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import wait_until
from test_framework.specialtx_bytes import PayloadCursor

TRANSACTION_FUTURE = 7


class CharacteriseFuturesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Needed only for check_fee_masking's own row, which arms it briefly;
        # every other row runs with the spork at its default OFF state.
        self.extra_args = [["-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA"]]

    def run_test(self):
        node = self.nodes[0]
        self.observed = {}

        self.log.info("priming past the founder-payment start height (500)")
        self.owner = node.getnewaddress()
        node.generatetoaddress(550, self.owner)

        self.bootstrap_p2p()

        self.check("version 0", field="nVersion", value=(2, b"\x00\x00"))
        self.check("version above CURRENT_VERSION", field="nVersion", value=(2, b"\x63\x00"))
        self.check("truncated payload", field="truncate", value=None, expect_accept=False)
        self.check("inputsHash tampered", field="inputsHash", value=(32, bytes(32)))
        self.check("lockOutputIndex out of range -- CheckFutureTx has no bounds check for it",
                    field="lockOutputIndex", value=(2, b"\xff\xff"), expect_accept=True)

        self.log.info("=" * 72)
        self.log.info("characterised future-op rejection reasons")
        for name, got in self.observed.items():
            self.log.info("  %-62s %s" % (name, got))
        self.log.info("=" * 72)

        self.check_fee_masking()

        broken = [n for n, g in self.observed.items() if g.startswith("HARNESS")]
        assert not broken, "harness could not express: %s" % ", ".join(broken)

    # ---- harness --------------------------------------------------------

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)

    def reconnect_p2p(self):
        self.nodes[0].disconnect_p2ps()
        network_thread_join()
        self.bootstrap_p2p()

    def clear_dkg_window(self):
        """Mine forward until the NEXT height is outside every real DKG
        mining window (feature_characterise_assets.py's own helper, reused
        verbatim -- see its docstring for why these four windows and not
        test_framework.blocktools.REGTEST_LLMQS's two). A hand-built
        coinbase carries no quorum commitments, so a block built inside a
        window is rejected bad-qc-missing regardless of what else it
        carries -- found empirically, the same trap F-63 already names."""
        node = self.nodes[0]
        windows = ((30, 10, 18), (360, 20, 28), (720, 20, 48), (24, 10, 18))

        def next_height_is_clear():
            h = node.getblockcount() + 1
            return all(not (start <= h % interval <= end) for interval, start, end in windows)

        while not next_height_is_clear():
            node.generatetoaddress(1, self.owner)

    def base_block(self, extra_txns=()):
        node = self.nodes[0]
        self.clear_dkg_window()
        tip = int(node.getbestblockhash(), 16)
        height = node.getblockcount() + 1
        t = node.getblock(node.getbestblockhash())["time"] + 1
        block = create_block(tip, create_coinbase(height), t)
        for tx in extra_txns:
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block

    def submit_p2p(self, block):
        """Returns the reject reason, or None if the block was accepted.

        Falls back to grepping debug.log when the p2p reject message itself
        never arrives before send_blocks_and_test's own wait_for_getdata
        times out -- found empirically for several rows here (not for every
        row: DoS severity/ban timing appears to race the reject message on
        some rejections but not others). The block's own rejection is always
        logged by ProcessNewBlock's caller (init.cpp/validation.cpp's error()
        calls print the DoS state's reject reason), so this recovers the
        real answer instead of reporting a placeholder."""
        block.rehash()
        node = self.nodes[0]
        p2p = node.p2p
        with mininode_lock:
            p2p.reject_reason_received = None
        before = node.getbestblockhash()
        log_path = os.path.join(node.datadir, "regtest", "debug.log")
        with open(log_path, encoding="utf-8", errors="replace") as f:
            f.seek(0, 2)
            log_start = f.tell()
        try:
            p2p.send_blocks_and_test([block], node, success=False, request_block=True, timeout=10)
        except AssertionError:
            if node.getbestblockhash() != before:
                return None
        with mininode_lock:
            reason = p2p.reject_reason_received
        if reason:
            return reason.decode() if isinstance(reason, bytes) else reason
        with open(log_path, encoding="utf-8", errors="replace") as f:
            f.seek(log_start)
            tail = f.read()
        m = re.search(r"%s.*" % block.hash, tail)
        if m:
            return "rejected (debug.log): %s" % m.group(0)[:200]
        return "rejected, no reason given"

    def fresh_future_tx(self):
        """A genuine, wallet-signed TRANSACTION_FUTURE, via sendtoaddress's
        own future={...} parameter (matching feature_futures.py's own
        send_future). Its payload's own fee field must equal getFutureFees()
        at call time or checkSpecialTxFee rejects it before it ever reaches
        the mempool -- 0 whenever SPORK_22 sits at its default OFF state,
        which every caller except check_fee_masking relies on.

        Left UNCONFIRMED and UNMINED on purpose: the mutated copy this feeds
        into (see check()) spends the SAME input, so confirming the original
        first would leave the mutated copy's own input already spent --
        found empirically (every row failing bad-txns-inputs-missingorspent
        instead of its own rule, the identical trap
        feature_characterise_assets.py's own characterise_bad_transfer_value
        hit and fixed the same way: clear the DKG window BEFORE broadcasting,
        so nothing downstream needs to mine a real block and sweep this one
        up before the mutated copy gets its turn).

        lockunspent's own locked output: sendtoaddress and its future={...}
        both send to a fresh node.getnewaddress() -- a WALLET-owned address,
        single-node/single-wallet test -- so the maturity-locked output
        itself sits in this same wallet and is, in principle, selectable by
        a LATER sendtoaddress call's own automatic coin selection. Found
        empirically: with several rows' locked outputs accumulating
        unconfirmed and immature, a later row's sendtoaddress occasionally
        picked one as a funding input and failed outright --
        'CommitTransaction(): Transaction can not be broadcasted
        immediately, bad-txns-premature-spend-of-future' -- proving
        validateFutureCoin's maturity gate is real (which feature_futures.py
        already establishes deliberately) but derailing an unrelated row
        here. Locking the output immediately keeps it out of every
        subsequent call's coin selection."""
        node = self.nodes[0]
        self.clear_dkg_window()
        address = node.getnewaddress()
        txid = node.sendtoaddress(
            address=address, amount=100,
            future={"future_maturity": 5, "future_locktime": -1})
        assert txid in node.getrawmempool()
        verbose = node.getrawtransaction(txid, 1)
        node.lockunspent(False, [{"txid": txid, "vout": verbose["futureTx"]["lockOutputIndex"]}])
        return FromHex(CTransaction(), verbose["hex"])

    def check(self, name, field, value, expect_accept=False):
        try:
            self.reconnect_p2p()
            tx = self.fresh_future_tx()
            payload = bytes(tx.vExtraPayload)

            c = PayloadCursor(payload)
            s_ver = c.skip_fixed(2)
            s_maturity = c.skip_fixed(4)
            s_locktime = c.skip_fixed(4)
            s_lockoutidx = c.skip_fixed(2)
            s_fee = c.skip_fixed(2)
            s_updatable = c.skip_fixed(1)
            s_exchain = c.skip_fixed(2)
            s_extscript = c.skip_compact_bytes()
            s_exttxid = c.skip_fixed(32)
            s_extconf = c.skip_fixed(2)
            s_inputshash = c.skip_fixed(32)
            assert c.pos == len(payload), "unparsed trailer in a genuine payload -- offsets are wrong"

            spans = {
                "nVersion": s_ver, "maturity": s_maturity, "lockTime": s_locktime,
                "lockOutputIndex": s_lockoutidx, "fee": s_fee, "updatableByDestination": s_updatable,
                "exChainType": s_exchain, "externalTxid": s_exttxid,
                "externalConfirmations": s_extconf, "inputsHash": s_inputshash,
            }

            if field == "truncate":
                new_payload = payload[:-8]
            elif field in spans:
                start, end = spans[field]
                width, raw_bytes = value
                assert width == end - start, "%s: fixed-width field, got %d bytes for a %d-byte span" % (
                    field, width, end - start)
                new_payload = payload[:start] + raw_bytes + payload[end:]
            else:
                raise AssertionError("unknown field %r" % field)

            out = CTransaction()
            out.deserialize(BytesIO(tx.serialize()))
            out.vExtraPayload = new_payload
            out.rehash()

            block = self.base_block(extra_txns=[out])
            got = self.submit_p2p(block)
            # Confirm (or let expire naturally via a real block) whatever the
            # ORIGINAL tx's fate is before the next row leaves a second one
            # unconfirmed alongside it. Found empirically: CTxMemPool::check's
            # own consistency assertion (txmempool.cpp:1155,
            # assert(fCheckResult) in CheckInputsAndUpdateCoins) aborted the
            # node outright when several of this file's own never-confirmed
            # future transactions were left sitting in the mempool together
            # across more than one clear_dkg_window() mining call -- a real
            # crash, reproduced, but plausibly a harness-shaped scenario
            # (multiple simultaneously-stale mempool entries this file's own
            # technique creates) rather than one a normal user or miner would
            # hit; recorded in F-216, not chased further (no src/ changes
            # permitted in this task, and this file's job is characterisation
            # of consensus rules, not mempool internals).
            self.nodes[0].generatetoaddress(1, self.owner)
        except Exception as e:
            self.observed[name] = "HARNESS ERROR: %s" % e
            self.log.info("  %-62s -> %s", name, self.observed[name])
            return

        result = got if got is not None else "ACCEPTED (no rejection)"
        self.observed[name] = result
        self.log.info("  %-62s -> %s", name, result)
        if expect_accept:
            # NOT a claim that the whole block is accepted: patching ANY
            # field invalidates the outer transaction's own ECDSA signature
            # (the sighash covers vExtraPayload in full), so a row
            # CheckFutureTx itself does not reject still normally fails
            # later, at script verification (block-validation-failed) --
            # found empirically, this row's first version wrongly asserted
            # outright acceptance and failed here. What this DOES prove: the
            # rejection reason is NOT one of CheckFutureTx's own strings,
            # which is the actual claim -- the field passed through that
            # function untouched.
            future_specific = ("bad-future-type", "bad-future-payload", "bad-future-version",
                               "bad-protx-inputs-hash")
            assert result not in future_specific, (
                "%s: expected CheckFutureTx to pass this through, but it rejected with %s" % (name, result))
        else:
            assert not result.startswith("ACCEPTED"), "%s: expected a rejection, node accepted it" % name

    # ---- the fee-masking question ----------------------------------------

    def check_fee_masking(self):
        """With SPORK_22 armed (a non-zero fee required), a future payload
        whose own fee field is 0 (the default, matching the spork's OFF
        state) is rejected before ever reaching CheckFutureTx -- confirming
        checkSpecialTxFee's masking is real for futures too, not just for
        assets (feature_characterise_assets.py's own finding).

        **A real node crash found building this row, worth its own record.**
        The first version used fresh_future_tx() (sendtoaddress, which
        broadcasts for real) to build the stale fee=0 transaction, THEN armed
        the spork. That leaves a transaction sitting in the mempool that was
        valid when it entered but is retroactively invalid the instant the
        spork changes -- nothing evicts it, since a spork flip is not a
        chain-tip event. The next block submission that fails ConnectBlock
        (this row's own, or unrelated) triggers ActivateBestChain's mempool
        re-sync, which calls CTxMemPool::check() -- and check()'s own
        assert(fCheckResult) in CheckInputsAndUpdateCoins (txmempool.cpp:
        1155) aborts the node outright when it finds that stale entry.
        Reproduced twice, always at the identical point ('Checking mempool
        with N transactions...' immediately followed by 'Posix Signal:
        Aborted' in debug.log). This is plausibly a real, if narrow, gap: a
        spork-dependent consensus rule (checkSpecialTxFee) creates mempool
        entries whose validity is not monotonic in chain height alone, and
        nothing re-validates the mempool against a spork change the way it
        does against a new tip. Recorded in F-216, not fixed (no src/
        changes permitted in this task, and the actual crash trigger --
        -checkmempool -- is a debug/test-only code path, not overwhelming
        evidence of mainnet impact by itself).

        Worked around here by building via createrawtransaction (also reads
        getFutureFees() at construction time, rawtransaction_util.cpp:164,
        so the same before/after spork ordering applies) plus
        signrawtransactionwithwallet -- NEITHER broadcasts, so nothing ever
        sits in the mempool at all before this row's own single, controlled
        block submission."""
        node = self.nodes[0]
        self.reconnect_p2p()
        # get_block_subsidy (blocktools.py): 4 RTM/block below height 720,
        # 5000 above -- this file primes to 550, so ordinary coinbase UTXOs
        # are themselves only ~4 RTM at this point, SMALLER than the 100 RTM
        # future-locked outputs this function needs to steer clear of below.
        # Mine past 720 PLUS COINBASE_MATURITY (100, consensus/consensus.h)
        # so a clearly-larger, clearly-MATURE (not just mined) UTXO exists to
        # pick from -- found empirically, 725 alone left every 5000 RTM
        # reward still 100 confirmations short of spendable.
        if node.getblockcount() < 850:
            node.generatetoaddress(850 - node.getblockcount(), self.owner)
        self.clear_dkg_window()
        # unlock: every earlier fresh_future_tx() call locked its own
        # future-locked output (see that method's own docstring); the
        # largest unlocked UTXO alone might be tiny by now, which would
        # otherwise make future_amount=100 exceed the input's own value
        # (bad-txns-in-belowout, no change output computed for it below) --
        # found empirically.
        node.lockunspent(True)
        # amount > 1000: excludes every 100-RTM future-locked output this
        # file's own earlier rows created (listunspent lists them as
        # spendable regardless of maturity -- the wallet only discovers a
        # future is still immature at actual broadcast time, per
        # fresh_future_tx's own docstring) -- found empirically, an
        # unfiltered max() picked one and failed
        # bad-txns-premature-spend-of-future instead of the masking rule
        # this row means to test. A coinbase reward at this chain height is
        # 5000 RTM (get_block_subsidy, blocktools.py), comfortably clear.
        utxo = max((u for u in node.listunspent() if u["amount"] > 1000), key=lambda u: u["amount"])
        dest = node.getnewaddress()
        # Built while the spork is still OFF, so getFutureFees() computes 0
        # into the payload -- arming the spork AFTER, not before, is
        # deliberate: computed at construction time, so building this with
        # the spork already armed would just produce a correctly-fee'd
        # payload and prove nothing (found empirically, the first version of
        # this row got that ordering wrong via fresh_future_tx's own
        # internal spork read and always observed ACCEPTED).
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {dest: {"future_maturity": 5, "future_locktime": -1, "future_amount": 100}})
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"], signed
        tx = FromHex(CTransaction(), signed["hex"])

        try:
            node.spork("SPORK_22_SPECIAL_TX_FEE", 3)  # low byte -> future fee 3 RTM
            wait_until(lambda: node.spork("show")["SPORK_22_SPECIAL_TX_FEE"] == 3, timeout=30)
            block = self.base_block(extra_txns=[tx])
            got = self.submit_p2p(block)
        finally:
            node.spork("SPORK_22_SPECIAL_TX_FEE", 4070908800)
            wait_until(lambda: node.spork("show")["SPORK_22_SPECIAL_TX_FEE"] == 4070908800, timeout=30)

        result = got if got is not None else "ACCEPTED (no rejection)"
        self.log.info("=" * 72)
        self.log.info("fee-masking: a fee=0 future, submitted after arming SPORK_22 -> %s", result)
        self.log.info("  checkSpecialTxFee (consensus/tx_verify.cpp) runs in Consensus::CheckTxInputs,")
        self.log.info("  BEFORE CheckSpecialTx's own dispatch -- this is expected to be")
        self.log.info("  bad-txns-wrong-future-fee-or-not-enable, the SAME generic string")
        self.log.info("  feature_characterise_assets.py's own masking row hits for a mismatched")
        self.log.info("  asset fee, despite the string literally naming only 'future'.")
        self.log.info("=" * 72)
        self.observed["fee=0 future submitted while SPORK_22 requires 3 RTM (masking)"] = result
        assert result == "bad-txns-wrong-future-fee-or-not-enable", (
            "expected checkSpecialTxFee's masking string, got %s" % result)


if __name__ == "__main__":
    CharacteriseFuturesTest().main()
