#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Live-fire transaction-decoupling.md 3A.7's manufactured quorum split, and
F-136/F-137's own DIP8 signing-attempts retry that was built to survive it --
neither has ever run outside a pure-function unit test or a plain (non-
decoupled) chain reorg.

3A.7's own narrative: "Mine H. Deliver its bodies to a subset of the quorum;
withhold from the rest. The subset connects H and signs it. The remainder are
still on the honest sibling H' and sign that ... No ChainLock at that
height." Decoupling's own contribution is that a commitment (tiny) reaches
everyone instantly while a body (the thing that actually makes a block
connectable and therefore signable -- TrySignChainTip signs
::ChainActive().Tip() only) arrives on the attacker's own schedule, not a
race the defender can win by being fast.

feature_llmq_chainlocks.py already isolates a node, mines competing tips on
both sides of a real network partition, and confirms the honest side gets
chainlocked while the isolated side's tip is later marked conflicting -- but
it isolates node0, the plain (non-smartnode) controller, never one of the
mn_count=3 real quorum members, and it never sets a single decoupling flag
(confirmed by grep before writing this test). So the actual signers are
never split by that test, and even if they were, block recovery on
reconnect would go through the ordinary whole-block/compact-block path, not
GETBODYRANGE.

This test partitions the three real smartnodes themselves, not the
controller. It does NOT also re-prove GETBODYRANGE recovery specifically --
that would need -perfwithholdheight, a startup-only flag naming an exact
height nothing here knows in advance (the height depends on how many blocks
four real DKG sessions take), and the natural alternative -- a genuine
network partition, then reconnect -- turned out empirically NOT to route
through GETBODYRANGE at all: net_processing.cpp:1329 gates that recovery
path on the block already carrying BLOCK_HAVE_DATA (a prior commitment-only
accept), which a node that simply missed an ordinary block announcement
never enters -- confirmed live, in the debug.log, before this comment was
written: the reconnecting controller took the ordinary headers -> cmpctblock
-> getblocktxn/blocktxn path instead, exactly as a 1-block-behind peer
should. Decoupling's own recovery mechanism is proven separately (F-231,
feature_decoupled_assets_futures.py) and is the right, cheap thing for this
one-block case to use -- GETBODYRANGE exists for genuinely commitment-only
blocks, not routine catch-up. This test's own scope is narrower and,
importantly, still real: F-136/F-137's retry mechanism, live-fired by a
genuine multi-signer split, is the thing that has never been exercised
outside a pure-function unit test, and that gap is what this closes.

**Not tautological, on purpose -- this took two attempts to get right.** The
first draft split mn1/mn3 onto competing single-block tips, then let mn1's
side pull ahead by mining 2 MORE blocks before reconnecting, so the height
that finally got chainlocked was H+2, not H. TrySignChainTip signs
::ChainActive().Tip() only (quorums_chainlocks.cpp:262-265) -- H+2 is a
height NOBODY had ever attempted before, so reaching it needed no retry
logic at all: two members making their own first-ever attempt on a brand
new height is exactly as sufficient as two members retrying an old one,
since DIP8's own one-shot-per-height guard (the bug F-136 fixed) only ever
blocks a REPEATED attempt at the SAME height. That draft would have passed
identically with the fix reverted. Caught before shipping, not after.

The fix: never extend either branch. mn1 mines ONE block (H) alone, attempts
to sign it alone, fails (1 of 3 -- unremarkable either way). Mocktime then
advances past CLSIG_ATTEMPT_LOOKBACK's own tolerance (F-137, so no late
share from the failed round can still combine), and ONLY mn1 reconnects (to
the controller, not to mn2 directly) and is asked to sign H again in a
provably fresh attempt slot. mn2, still on the controller the whole time,
adopts H as its own first-ever candidate the moment it arrives (no reorg
question -- mn2 held nothing at that height before) and makes ITS OWN first
attempt in the same fresh slot. Two votes for H: one a genuine retry (mn1),
one genuinely fresh (mn2) -- 2 of 3, threshold reached, at the ORIGINAL
contested height, not a taller descendant. Discriminating check: with
F-136/F-137 reverted, mn1's retry is permanently blocked by the old
one-shot-per-height guard, mn2's lone fresh vote never exceeds 1 of 3, and H
can never be chainlocked -- this test would hang on wait_for_chainlocked_
block and fail on timeout, not silently pass.

mn3 mines its own competing tip (H') while isolated too, matching 3A.7's
"the remainder are still on the honest sibling" -- but deliberately plays no
part in the discriminating proof above (equal work against H means mn3
never voluntarily reorgs onto it, so its vote is structurally unavailable to
H regardless of retry logic). Reconnecting it and observing H' marked
conflicting was tried and dropped too (own comment, below, at the point it
would have run) -- a second mechanism (ChainLock enforcement overriding an
equal-work tie-break) this test does not need to also exercise.

**Mutation-tested, not just argued.** DecideChainLockSignAction's own fix
(quorums_chainlocks.h) was temporarily reverted to the exact bug it
replaced -- nTipHeight == nLastSignedHeight returning kNone unconditionally,
dropping the nAttemptNum re-check entirely -- rebuilt, and run: genuine RED,
hanging at the post-reconnect wait with the log's last line still "mn1 must
independently retry signing the SAME H it already failed on once", killed
by an external 90s timeout with no CLSIG ever formed. Reverted the mutation,
rebuilt, reran: clean GREEN, identical to the run before the mutation.
"""
from test_framework.test_framework import RaptoreumTestFramework
from test_framework.util import assert_equal, connect_nodes, isolate_node, reconnect_isolated_node, wait_until

CLSIG_ATTEMPT_INTERVAL = 30  # llmq/quorums_chainlocks.h
CLSIG_ATTEMPT_LOOKBACK = 2   # llmq/quorums_chainlocks.h

DECOUPLING_FLAGS = ["-fetchbodyrange=1", "-servebodyrange=1", "-commitmentblocks=1"]


class LLMQChainLocksDecoupledTest(RaptoreumTestFramework):
    def set_test_params(self):
        # 1 controller + 3 smartnodes, matching feature_llmq_chainlocks.py's
        # own "3, not 5" precedent (with 5, only 3 take part in any one DKG
        # session and the ones under test are often not among them). Every
        # node runs with decoupling flags live -- confirmed (module doc
        # above) not to change this specific test's own 1-block recovery
        # path, but proving that much still matters: it shows the flags
        # coexist correctly with chainlocks/DKG rather than being untested
        # in that combination, which the reference test never sets at all.
        self.set_raptoreum_test_params(4, 3, extra_args=[list(DECOUPLING_FLAGS) for _ in range(4)],
                                       fast_dip3_enforcement=True)

    def run_test(self):
        controller = self.nodes[0]
        mn1, mn2, mn3 = (m.node for m in self.mninfo)

        for i in range(len(self.nodes)):
            if i != 1:
                connect_nodes(self.nodes[i], 1)

        self.wait_for_dip8_activation()
        self.sync_blocks(self.nodes, timeout=60 * 5)

        controller.spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("mining 4 quorums (feature_llmq_chainlocks.py's own count)")
        for _ in range(4):
            self.mine_quorum()

        self.log.info("sanity: a single ordinary block still gets chainlocked with decoupling flags live")
        controller.generate(1)
        self.wait_for_chainlocked_block_all_nodes(controller.getbestblockhash())

        self.log.info("splitting the THREE REAL SIGNERS, not the controller: mn1 alone on H, "
                      "mn3 alone on a competing H', mn2 stranded with the controller on neither")
        parent_tip = controller.getbestblockhash()
        # Smartnode operator nodes run wallet-disabled (collateral/addresses
        # live on the controller's own wallet, per prepare_smartnode) --
        # fetch mining addresses from the controller before isolating anyone;
        # generatetoaddress needs no wallet on the mining node itself.
        addr_h = controller.getnewaddress()
        addr_hprime = controller.getnewaddress()
        isolate_node(mn1)
        isolate_node(mn3)
        # mn2 is still connected to the controller; neither has been asked to
        # extend the chain yet, so both remain honestly on parent_tip.

        height_h = mn1.generatetoaddress(1, addr_h)[-1]
        height_hprime = mn3.generatetoaddress(1, addr_hprime)[-1]
        assert height_h != height_hprime, "the two isolated members must mine genuinely competing tips"

        self.log.info("giving each isolated member's own scheduler a real window to attempt-and-fail "
                      "signing its own local (unreachable-threshold) candidate")
        self.bump_mocktime(5, nodes=[mn1])
        self.bump_mocktime(5, nodes=[mn3])

        for node, branch in ((mn1, height_h), (mn2, parent_tip), (mn3, height_hprime)):
            assert_equal(node.getbestblockhash(), branch)
        self.log.info("confirmed: three real signers on three different views, and no candidate reaches "
                      "threshold=2 -- this IS transaction-decoupling.md 3A.7's manufactured split, live")

        # mn2/controller are still on parent_tip, which the earlier sanity
        # check already legitimately chainlocked before this split began --
        # only the two NEW, competing candidates must fail to reach it now.
        for node, branch in ((mn1, height_h), (mn3, height_hprime)):
            assert not node.getblock(branch).get("chainlock", False), \
                "no candidate may be chainlocked while genuinely split below threshold"

        self.log.info("advancing mocktime past CLSIG_ATTEMPT_LOOKBACK's own tolerance (%ds) before "
                      "reconnecting -- H's own failed round-1 share is now expired, so any convergence "
                      "below can only be a FRESH DIP8 attempt at the SAME height H" %
                      (CLSIG_ATTEMPT_INTERVAL * (CLSIG_ATTEMPT_LOOKBACK + 1)))
        self.bump_mocktime(CLSIG_ATTEMPT_INTERVAL * (CLSIG_ATTEMPT_LOOKBACK + 1))

        self.log.info("reconnecting ONLY mn1 (still holding H, unextended) to the controller -- mn2 "
                      "adopts H as its own first-ever candidate at this height (no reorg question), "
                      "and mn1 must independently retry signing the SAME H it already failed on once")
        reconnect_isolated_node(mn1, 0)
        # The initial mesh was wired hub-and-spoke through mn1
        # (connect_nodes(self.nodes[i], 1) for every i != 1) -- isolating
        # mn1 may have left the controller with no other path to mn2. Wire
        # it explicitly rather than trust whatever topology organic peer
        # discovery happened to add since setup; mn2's own vote is
        # load-bearing for the threshold-of-2 proof below, so its
        # connectivity is not left to chance.
        connect_nodes(mn2, 0)
        wait_until(lambda: controller.getbestblockhash() == height_h, timeout=30)
        wait_until(lambda: mn2.getbestblockhash() == height_h, timeout=30)

        # NOT wait_for_chainlocked_block_all_nodes: that checks self.nodes in
        # full, including mn3, still isolated at this point on its own H' --
        # mn3.getblock(height_h) would throw "not found" forever (mn3
        # genuinely doesn't have this block yet), so that call would just
        # poll to timeout regardless of whether signing itself succeeded.
        # Only the three nodes that actually hold H belong in this check.
        for node in (controller, mn1, mn2):
            self.wait_for_chainlocked_block(node, height_h, timeout=30)
        self.log.info("confirmed: H itself -- the exact height the split round already failed to lock, "
                      "not a taller descendant -- is now chainlocked, via mn1's own retried signature and "
                      "mn2's own fresh one combining to reach threshold. Discriminating: with F-136/F-137 "
                      "reverted, mn1's retry is the old code's permanently-blocked one-shot attempt, mn2 "
                      "alone never exceeds 1 of 3, and this line would never have been reached")

        # mn3's own reconnect-and-observe-H'-marked-conflicting was tried
        # here and dropped: mn3 mined H' itself, giving it equal work to H,
        # and Bitcoin Core's own tie-break never voluntarily reorgs onto a
        # merely-equal alternative -- getting mn3 to adopt H needs either
        # extra work on H's branch (which feature_llmq_chainlocks.py's own
        # "isolate node0, mine on both parts" section already covers, with
        # no smartnode ever split) or ChainLock enforcement overriding the
        # tie-break, a separate mechanism (EnforceBestChainLock) and its own
        # timing this test does not need to also exercise. mn3's vote was
        # deliberately structured to be irrelevant to the discriminating
        # proof above; leaving it stranded on H' does not weaken that proof.


if __name__ == "__main__":
    LLMQChainLocksDecoupledTest().main()
