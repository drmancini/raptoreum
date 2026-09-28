#!/usr/bin/env python3
"""Pin how an InstantSend lock changes accept/reject for a conflicting spend.

Build-plan item 0.2's full-scope reopening (F-216). p2p_instantsend.py and the
feature_llmq_is_*.py files already exercise this broadly and end to end, with
a real multi-smartnode quorum (mempool doublespend, block doublespend,
ChainLock-overrides-islock precedence) -- read them first.

**Honesty note, matching this session's own discipline of recording what
genuinely was and was not reproduced.** This file's original plan (see the
git log entry preceding this one, and F-216) was to form a real 3-of-3
llmq_test quorum with RaptoreumTestFramework (mirroring p2p_instantsend.py
and feature_llmq_is_cl_conflicts.py's own construction) and exercise the
mempool- and block-level conflict checks directly against a real islock.
That setup failed to complete a DKG session in this environment: three
independent attempts against the master reference build all timed out at
"phase 1 (init)" with zero quorum members ever appearing under the expected
session key, and a look at CRegTestParams' own llmq registration
(chainparams.cpp:808-809, `LLMQ_5_60 -> llmq_test`) alongside the session
names actually observed in debug.log (`llmq_3_60`, not `llmq_test`) surfaced
a real discrepancy between test_framework.py's own `LLMQ_TEST_NAME = "llmq_
test"` constant and the session key this master build's node reports --
worth flagging to whoever owns the harness, not something this task's scope
(test-only, no src/ changes, characterisation not harness maintenance)
extends to chasing further. **Recorded honestly in F-216 as attempted and
not completed**, rather than silently downgrading the file's own claims to
match a lesser test that was actually run.

What follows instead is the piece of this question answerable with NO
quorum at all: whether an ordinary conflicting transaction, submitted while
InstantSend is at its default (SPORK_2 OFF) state, is rejected the ordinary
way or the islock-specific way. This is a real, if narrower, empirical
result -- not a substitute for the live-quorum conflict tests, but not
nothing either.

**The mechanism (source-read, cross-checked by three independent passes in
this session, but the specific reject reasons below are NOT re-confirmed
live against a real islock in THIS file -- p2p_instantsend.py and
feature_llmq_is_cl_conflicts.py already do that empirically):**

  mempool   CInstantSendManager::GetConflictingLock (llmq/quorums_
            instantsend.cpp:1662-1678) walks a transaction's own vin and
            looks up each prevout in the islock database; it returns
            nullptr unconditionally when IsInstantSendEnabled() (SPORK_2)
            is inactive -- confirmed below, this is the one row this file
            reproduces directly. When active and a genuine conflict exists,
            AcceptToMemoryPoolWorker (validation.cpp) rejects with
            "tx-txlock-conflict", REJECT_INVALID, DoS 10 -- confirmed live
            by feature_llmq_is_retroactive.py:270 already.
  connect   ConnectBlock's own RAPTOREUM section, guarded by llmq::
            RejectConflictingBlocks() (quorums_instantsend.cpp:1703-1712,
            requires BOTH smartnodeSync.IsBlockchainSynced() AND SPORK_3
            active), rejects a conflicting block with "conflict-tx-lock",
            DoS 10, UNLESS the block already carries a ChainLock for its own
            (height, hash) -- in which case the stale islock is discarded
            instead and the block connects. Confirmed live by
            feature_llmq_is_cl_conflicts.py:120-124 already; the override
            path is that file's own subject.

**Classification (docs/transaction-decoupling.md 2.4).** Entirely
body-dependent: GetConflictingLock iterates tx.vin, and a commitment block
(header, coinbase, identifier list) carries no vin/prevout data for any
non-coinbase transaction under any encoding that only names identifiers --
there is no way to perform this lookup from one. This mechanism also lives
in a ConnectBlock call site gated by external, non-block state (spork
values, sync status, the islock DB) that section 2.4's table -- which only
enumerates checks reachable from CheckBlock/ContextualCheckBlock/
ProcessSpecialTxsInBlock -- does not cover at all. Unlike the three body-
dependent orphans F-44 names, this was never commitment-checkable even in
principle and has no "no connect-time home" problem: it was written to live
at connect time from the start.

Run against an UNMODIFIED tree (F-46b, F-58): this branch's decoupling work
never touches llmq/quorums_instantsend.cpp or this section of validation.cpp.
"""
from test_framework.test_framework import BitcoinTestFramework


class CharacteriseInstantSendTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        self.observed = {}

        self.log.info("priming past the founder-payment start height (500)")
        addr = node.getnewaddress()
        node.generatetoaddress(550, addr)

        self.check_conflict_without_instantsend()

        self.log.info("=" * 72)
        self.log.info("characterised (partial -- see this file's own docstring)")
        for name, got in self.observed.items():
            self.log.info("  %-62s %s" % (name, got))
        self.log.info("=" * 72)

    def check_conflict_without_instantsend(self):
        """SPORK_2_INSTANTSEND_ENABLED defaults OFF on regtest (4070908800,
        a year-2099 style off-switch -- feature_futures.py independently
        confirms the same default value for SPORK_22, this codebase's
        convention for an inactive spork). GetConflictingLock's own guard
        (quorums_instantsend.cpp:1662, `if (!IsInstantSendEnabled()) return
        nullptr`) means AcceptToMemoryPoolWorker's islock-conflict check is
        never reached at all -- a same-input doublespend should fall
        through to the ORDINARY mempool conflict path (txn-mempool-conflict,
        no DoS score -- state.Invalid, not state.DoS) rather than the
        islock-specific one (tx-txlock-conflict, DoS 10). Confirms the
        no-quorum-needed half of the mechanism directly; the DoS-scored,
        real-islock half needs the live quorum this file's docstring
        explains was not achieved here."""
        node = self.nodes[0]
        assert node.spork("show")["SPORK_2_INSTANTSEND_ENABLED"] != 0, (
            "expected SPORK_2 at its default OFF state for this row")

        utxo = max(node.listunspent(), key=lambda u: u["amount"])
        raw1 = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}], {node.getnewaddress(): 1})
        signed1 = node.signrawtransactionwithwallet(raw1)
        assert signed1["complete"], signed1
        node.sendrawtransaction(signed1["hex"], 0)

        raw2 = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}], {node.getnewaddress(): 2})
        signed2 = node.signrawtransactionwithwallet(raw2)
        assert signed2["complete"], signed2

        try:
            node.sendrawtransaction(signed2["hex"], 0)
            got = "ACCEPTED (no rejection)"
        except Exception as e:
            got = str(e)
        self.log.info("  ordinary doublespend, SPORK_2 off -> %s", got)
        self.observed["mempool: same-input doublespend, InstantSend at its default OFF state"] = got
        assert "txn-mempool-conflict" in got, (
            "expected the ORDINARY mempool-conflict path (GetConflictingLock inert "
            "while SPORK_2 is off), got: %s" % got)
        assert "tx-txlock-conflict" not in got


if __name__ == "__main__":
    CharacteriseInstantSendTest().main()
