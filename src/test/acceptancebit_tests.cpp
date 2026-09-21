// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 1.3.1 (Mike, 2026-09-19): the real per-block BLOCK_HAVE_BODIES bit, ported
// and hardened from probe/acceptance-layer (a2d5f68c9, 37a7c37a0) -- the
// commitment block is always written (BLOCK_HAVE_DATA stays honest), and
// BLOCK_HAVE_BODIES is set separately by ReceivedBlockBodies. F-100 corrected
// the plan draft that described a vacuous encoding where the bit collapsed
// onto BLOCK_HAVE_DATA; these tests exercise the real, distinct bit.

#include <algorithm>
#include <amount.h>
#include <bodystore.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <key.h>
#include <keystore.h>
#include <node/context.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <fs.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <streams.h>
#include <txmempool.h>
#include <util/system.h>
#include <validation.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

// F7 (test review precedent, txvalidation_tests.cpp/blockbudget_tests.cpp):
// a thrown BOOST_REQUIRE between inserting into g_perf_withhold_* and erasing
// would leave the flag set for every later test in the process.
struct PerfWithholdGuard {
    uint256 hash;

    explicit PerfWithholdGuard(const uint256 &hashIn) : hash(hashIn) {
        g_perf_withhold_hashes.insert(hash);
    }

    ~PerfWithholdGuard() { g_perf_withhold_hashes.erase(hash); }
};

BOOST_FIXTURE_TEST_SUITE(acceptancebit_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(have_bodies_is_true_by_default) {
    const CBlockIndex *tip = ::ChainActive().Tip();
    BOOST_REQUIRE(tip != nullptr);
    BOOST_CHECK(HaveBodies(tip));
    BOOST_CHECK(tip->nStatus & BLOCK_HAVE_BODIES);
}

BOOST_AUTO_TEST_CASE(have_bodies_is_false_for_a_null_index) {
    BOOST_CHECK(!HaveBodies(nullptr));
}

// The core mechanism: a withheld block is accepted with BLOCK_HAVE_DATA set
// but BLOCK_HAVE_BODIES clear (commitment-only); once the body is offered
// again -- unrequested, exactly F-36's trap -- it must be accepted and the
// bit set, not silently discarded.
BOOST_AUTO_TEST_CASE(unrequested_arrival_fills_a_commitment_only_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    {
        PerfWithholdGuard guard(hash);
        bool fNewBlock = false;
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
        BOOST_REQUIRE(fNewBlock);

        const CBlockIndex *pindex = LookupBlockIndex(hash);
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(pindex->nTx > 0);
        BOOST_CHECK(!HaveBodies(pindex));
        BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODIES));
    }
    // Guard destructor cleared the withhold -- PerfWithholdBodies(pindex) is
    // now false, but HaveBodies(pindex) stays false: it reads the PERSISTED
    // bit, not the current withhold-flag state, exactly the distinction
    // F-100 corrected the plan draft over.
    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(!HaveBodies(pindex));
    // Test review (2026-09-19): capture where the commitment record already
    // lives, so a re-offer that wrongly writes a SECOND copy (exactly what
    // ReceivedBlockBodies exists to avoid -- see the "already-stored,
    // still-withheld" comment in AcceptBlock) is caught, not just the bit.
    const int nFileBefore = pindex->nFile;
    const unsigned int nDataPosBefore = pindex->nDataPos;

    // Re-offer the identical block, UNREQUESTED (fForceProcessing = false).
    // Before the F-36 fix, AcceptBlock's "if (pindex->nTx != 0) return true"
    // would discard this -- indistinguishable from a pruned block -- and the
    // bit would never be set.
    bool fNewBlock2 = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    // AcceptBlock sets *fNewBlock unconditionally once past the requested/
    // work/height gates and CheckBlock, before it knows whether this is a
    // genuinely new block or bodies filling in an already-known one --
    // pre-existing behaviour, not something this fix changes. What matters
    // here is that we reached this line at all: before the F-36 fix this
    // call returned early with *fNewBlock left false.
    BOOST_CHECK(fNewBlock2);

    BOOST_CHECK(HaveBodies(pindex));
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODIES);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_DATA);

    // Test review (2026-09-19), HIGH: the mechanism's actual point -- a
    // commitment-only block whose bodies arrive must become connectable, not
    // just carry the right bits. Deleting ReceivedBlockBodies's candidate
    // re-arm walk (the "put back everything the arrival makes eligible
    // again" loop) would leave every assertion above passing while the tip
    // never advances.
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash() == hash);
    // And no second copy was written -- the record's own file position is
    // unchanged, not merely re-created identically.
    BOOST_CHECK_EQUAL(pindex->nFile, nFileBefore);
    BOOST_CHECK_EQUAL(pindex->nDataPos, nDataPosBefore);
}

// The fix must not touch pruning's own, deliberate "ignore a re-offered
// pruned block" behaviour (feature_characterise_pruned_arrival.py's pinned
// baseline): a block whose BLOCK_HAVE_DATA has been cleared (nTx != 0 stays,
// exactly what pruning leaves behind) must still be discarded when offered
// back unrequested.
BOOST_AUTO_TEST_CASE(genuinely_pruned_arrival_is_still_discarded) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nTx > 0);

    // Simulate pruning directly: clear HAVE_DATA (and HAVE_BODIES, which
    // PruneOneBlockFile clears alongside it) but leave nTx untouched -- the
    // exact index shape pruning leaves behind. CheckBlockIndex (called
    // unconditionally at the top of every AcceptBlock) asserts
    // !HAVE_DATA == (nTx==0) whenever fHavePruned is false, so this
    // simulation must also set the global fHavePruned flag the way real
    // pruning does, or the very next AcceptBlock call trips that assertion.
    struct FHavePrunedGuard {
        bool saved = fHavePruned;
        ~FHavePrunedGuard() { fHavePruned = saved; }
    } fHavePrunedGuard;
    fHavePruned = true;
    pindex->nStatus &= ~BLOCK_HAVE_DATA;
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    pindex->nStatus &= ~BLOCK_HAVE_UNDO; // PruneOneBlockFile clears this too; HAVE_UNDO implies HAVE_DATA

    bool fNewBlock2 = true;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    BOOST_CHECK(!fNewBlock2);

    // Must still be discarded: HAVE_DATA was not restored by the unrequested
    // arrival, matching the characterisation test's pinned pruning behaviour.
    BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_DATA));
    BOOST_CHECK(!HaveBodies(pindex));
}

// 1.3.4 (F-35/F-42, Mike, 2026-09-19): the equal-work tie-break. Settled by
// construction when ReceivedBlockBodies was built (1.3.1) -- it deliberately
// does NOT re-stamp nSequenceId, on the reasoning that first-seen order has
// not changed when a body merely catches up to already-held commitments.
// This proves it: a block (B) whose COMMITMENTS arrive first, but whose BODY
// arrives second (after an equal-work rival (A) has arrived whole and become
// tip), must still WIN the tie once its body fills in.
//
// L-1 (F-107 correction, Fable adversarial review, 2026-09-19): this does
// NOT match unmodified Bitcoin's tie-break -- it's a genuine, disclosed
// behaviour change (see F-42). Unmodified Bitcoin only assigns nSequenceId
// once a block's FULL data arrives, so with this exact arrival order (B's
// commitments, then A whole, then B's body) A keeps the tip upstream; only a
// first-seen-whole-and-connectable block can ever win a tie there. This test
// proves the opposite side of F-42's open question: a commitment-first,
// body-second block CAN displace an already-connected equal-work rival,
// something F-42 explicitly says unmodified code never does. F-35's bug was
// the reverse of what decoupling now deliberately does: re-stamping on body
// arrival would have moved B's sequence id LATER than A's, making B lose a
// tie this design means for it to win.
BOOST_AUTO_TEST_CASE(equal_work_tiebreak_survives_bodies_arriving_second) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    // Two distinct children of the same tip -- equal chainwork by
    // construction (same nBits at the same height) -- built with different
    // coinbase scriptPubKeys so they are genuinely different blocks, not the
    // same bytes twice. NOTE: TestChainSetup::CreateBlock's CKey overload
    // ignores its argument and always uses the fixture's own coinbaseKey (a
    // pre-existing quirk in test_raptoreum.cpp, out of scope here) -- use
    // the CScript overload directly instead.
    CKey keyB;
    keyB.MakeNewKey(true);
    CScript scriptPubKeyB = CScript() << ToByteVector(keyB.GetPubKey()) << OP_CHECKSIG;
    CBlock blockB = CreateBlock({}, scriptPubKeyB);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE(blockA.GetHash() != blockB.GetHash());

    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);
    std::shared_ptr<const CBlock> shared_pblockA = std::make_shared<const CBlock>(blockA);
    uint256 hashA = blockA.GetHash();
    uint256 hashB = blockB.GetHash();

    // B's commitments arrive first, bodies withheld -- not yet connectable,
    // so it cannot become tip regardless of its (currently low) sequence id.
    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    const CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));

    // A arrives whole, second, and becomes tip -- the only connectable
    // candidate at this point.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockA, /*fForceProcessing=*/true, nullptr));
    const CBlockIndex *pindexA = LookupBlockIndex(hashA);
    BOOST_REQUIRE(pindexA != nullptr);
    BOOST_REQUIRE(::ChainActive().Tip()->GetBlockHash() == hashA);
    BOOST_REQUIRE(pindexA->nChainWork == pindexB->nChainWork);   // genuinely a tie
    BOOST_REQUIRE_LT(pindexB->nSequenceId, pindexA->nSequenceId);   // B really was seen first

    // B's body now arrives, unrequested -- ReceivedBlockBodies must NOT
    // re-stamp nSequenceId, so B's (earlier) id is unchanged and it wins the
    // tie against A, becoming the new tip.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/false, nullptr));
    BOOST_CHECK(HaveBodies(pindexB));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
}

// 1.3.3 (F-25j, Mike, 2026-09-19): out-of-order arrival is pre-existing
// Bitcoin Core behaviour -- ReceivedBlockTransactions parks a block in
// m_blocks_unlinked whenever ITS OWN parent is not yet HaveTxsDownloaded(),
// "regardless of bodies" (F-25j's own wording). Decoupling does not touch
// that guard, only what "downloaded" now records (commitments vs bodies).
// This proves the guard still holds with the commitment layer wired in: a
// fully-valid, fully-downloaded CHILD whose PARENT looks header-only must be
// parked, not connected -- and must drain correctly once the parent's
// commitments genuinely arrive.
//
// Building that PARENT/CHILD pair is not free: DIP3's CbTx merkle roots
// (CalcCbTxMerkleRootMNList/Quorums, src/evo/cbtx.cpp) require the
// deterministic masternode list to already be built for whatever pindexPrev
// the child extends. That list is built incrementally, only by really
// connecting a block -- there is no way to construct a DIP3-valid CHILD on
// top of a parent that was never really connected (a fake CChain::SetTip
// onto a header-only index, the upstream miner_tests.cpp trick for plain
// subsidy math, reaches "*** Found EvoDB inconsistency, you must reindex to
// continue" here, a fatal abort). So this builds the pair the only way DIP3
// allows -- by really connecting the parent first -- and then rolls the
// parent back to header-only via the real DisconnectTip (which correctly
// reverses its coin/evoDB effects, unlike hand-editing nStatus over live
// state), leaving only its own nTx/nChainTx bookkeeping to reset directly.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

BOOST_AUTO_TEST_CASE(out_of_order_arrival_parks_a_child_until_its_parent_downloads) {
    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;

    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();
    CChain &active = ::ChainActive();

    CBlockIndex *pindexRealTip = active.Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    // Really connect the parent -- DIP3's deterministic MN list is now
    // genuinely cached for it, so a child built on top of it is DIP3-valid.
    CBlock parent = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hashParent = parent.GetHash();
    CBlockIndex *pindexParent = LookupBlockIndex(hashParent);
    BOOST_REQUIRE(pindexParent != nullptr);
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashParent.ToString());

    // Build the child on top of the (still really connected) parent tip --
    // ordinary construction, no fakery, so its own CbTx is genuinely correct.
    CBlock child = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(child.hashPrevBlock.ToString(), hashParent.ToString());

    // Roll the parent back off the active chain through the real mechanism,
    // which correctly reverses its coin/evoDB effects. DisconnectTip queues
    // every one of the block's txs (coinbase included) for mempool
    // resurrection -- deciding which ones actually get resurrected is the
    // real reorg caller's (UpdateMempoolForReorg's) job, done elsewhere, so
    // here it is enough to drain the queue directly: a coinbase can never be
    // mempool-eligible, and DisconnectedBlockTransactions's own destructor
    // asserts the queue is empty.
    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // setBlockIndexCandidates only keeps entries "as good as the current tip
    // or better" -- real tip pruned it out once the (strictly better) parent
    // overtook it, on the assumption (true for a real reorg, not here) that
    // ActivateBestChain would immediately follow within the same call and
    // reconnect something at least as good. CheckBlockIndex asserts the
    // active tip is always a member, so restore that invariant by hand.
    chainstate.setBlockIndexCandidates.insert(pindexRealTip);

    // DisconnectTip reverses the parent's EFFECTS but leaves its own
    // "this block's transactions were downloaded" bookkeeping untouched --
    // correctly so, since DisconnectTip models "no longer active", not
    // "never downloaded". Reset exactly that bookkeeping by hand (the same
    // kind of direct nStatus surgery genuinely_pruned_arrival_is_still_discarded
    // uses to simulate pruning) so the parent now looks exactly like a
    // genuine headers-first registration: known, ordered, but never given to
    // ReceivedBlockTransactions.
    chainstate.setBlockIndexCandidates.erase(pindexParent);
    pindexParent->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_BODIES | BLOCK_HAVE_UNDO);
    pindexParent->nStatus = (pindexParent->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_TREE;
    pindexParent->nTx = 0;
    pindexParent->nChainTx = 0;
    // nSequenceId is only ever assigned inside ReceivedBlockTransactions, so
    // a genuine header-only block always reads 0 -- CheckBlockIndex asserts
    // exactly this for every not-yet-linked block.
    pindexParent->nSequenceId = 0;
    BOOST_REQUIRE(!pindexParent->HaveTxsDownloaded());
    BOOST_REQUIRE(!(pindexParent->nStatus & BLOCK_HAVE_DATA));

    // The child arrives, fully formed. Its own parent is (as far as the
    // index can tell) still only header-known.
    std::shared_ptr<const CBlock> shared_pchild = std::make_shared<const CBlock>(child);
    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pchild, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    // The child is fully downloaded on its own -- HAVE_DATA is set -- but it
    // must NOT become the tip, because its own parent has never had
    // ReceivedBlockTransactions called on it. "Regardless of bodies"
    // (F-25j): the child carries a full, decoded body and it still cannot
    // connect.
    CBlockIndex *pindexChild = LookupBlockIndex(child.GetHash());
    BOOST_REQUIRE(pindexChild != nullptr);
    BOOST_CHECK(pindexChild->nStatus & BLOCK_HAVE_DATA);
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    // Direct evidence of the parked state itself (Fable review, 2026-09-19:
    // the mutation-tested crash above proves SOME invariant broke, not that
    // these specific facts are what the test is actually checking).
    BOOST_CHECK(!pindexChild->HaveTxsDownloaded());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexChild), 0U);
    auto rangeUnlinked = chainman.m_blockman.m_blocks_unlinked.equal_range(pindexParent);
    bool childFoundUnlinked = std::any_of(rangeUnlinked.first, rangeUnlinked.second,
                                          [&](const auto &kv) { return kv.second == pindexChild; });
    BOOST_CHECK(childFoundUnlinked);

    // Now the parent's commitments arrive, genuinely, through the normal
    // path. ReceivedBlockTransactions's own drain loop (validation.cpp) must
    // find the child in m_blocks_unlinked and connect both blocks in one go.
    std::shared_ptr<const CBlock> shared_pparent = std::make_shared<const CBlock>(parent);
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pparent, /*fForceProcessing=*/true, nullptr));

    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), child.GetHash().ToString());
    BOOST_CHECK_EQUAL(active.Height(), pindexRealTip->nHeight + 2);
}

// 1.3.3, take two (Fable adversarial review, 2026-09-19): the test above
// exercises ReceivedBlockTransactions's generic parent-guard, which is
// unmodified Bitcoin Core code -- it passes identically with
// g_commitmentBudgetActive off, so it proves nothing decoupling-specific.
// The real decoupling-era gap lives in FindMostWorkChain (validation.cpp,
// the "PERF" comment on fMissingData): a candidate whose OWN bodies are
// present can still have an ANCESTOR that is commitment-only, and
// FindMostWorkChain must recognise the whole candidate chain as unusable
// and re-park it in m_blocks_unlinked -- not silently drop it, and not let
// it become tip (ConnectTip cannot execute a block whose parent's UTXO
// effects were never applied). This is the shape F-25j actually named
// ("regardless of bodies") that the first test never built: a PARENT that
// is genuinely commitment-only, extended by a fully-bodied CHILD.
//
// Building a commitment-only PARENT with a DIP3-valid CHILD on top of it
// has the same constraint the first test's comment already documents:
// DIP3's deterministic MN list is only ever built by really connecting a
// block, so this builds the pair by really connecting the parent (getting
// a real list cached for it and a real child built on top), then
// downgrades the parent to commitment-only by hand -- clearing only
// BLOCK_HAVE_BODIES/BLOCK_HAVE_UNDO and the validity rung down to
// BLOCK_VALID_TRANSACTIONS (what a genuine commitment-only arrival grants,
// per D-16), while leaving nTx/nChainTx/nSequenceId untouched, since its
// commitments really were downloaded. The child is never submitted until
// after this downgrade, so it carries no stale index/candidate state of
// its own to account for.
BOOST_AUTO_TEST_CASE(commitment_only_ancestor_parks_a_fully_bodied_descendant) {
    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;

    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();
    CChain &active = ::ChainActive();

    CBlockIndex *pindexRealTip = active.Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    // Really connect the parent (DIP3 list genuinely cached for it), build
    // the child on top of it (also DIP3-valid), but do not submit the child
    // yet -- it must not acquire any index state before the parent is
    // downgraded below.
    CBlock parent = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hashParent = parent.GetHash();
    CBlockIndex *pindexParent = LookupBlockIndex(hashParent);
    BOOST_REQUIRE(pindexParent != nullptr);
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashParent.ToString());

    CBlock child = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(child.hashPrevBlock.ToString(), hashParent.ToString());

    // Roll the parent back off the active chain through the real mechanism.
    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    chainstate.setBlockIndexCandidates.insert(pindexRealTip);

    // Downgrade the parent to genuinely commitment-only: bodies withdrawn,
    // rung lowered to what a real commitment-only arrival grants, tx-level
    // bookkeeping (nTx/nChainTx/nSequenceId) untouched since commitments
    // really were downloaded for it.
    chainstate.setBlockIndexCandidates.erase(pindexParent);
    pindexParent->nStatus &= ~(BLOCK_HAVE_BODIES | BLOCK_HAVE_UNDO);
    pindexParent->nStatus = (pindexParent->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_TRANSACTIONS;
    BOOST_REQUIRE(pindexParent->nStatus & BLOCK_HAVE_DATA);
    BOOST_REQUIRE(pindexParent->HaveTxsDownloaded());
    BOOST_REQUIRE(!HaveBodies(pindexParent));

    // The fully-bodied child arrives. Its own data is complete, but its
    // parent's bodies are not -- FindMostWorkChain must refuse to connect
    // it and must park it, not lose it.
    std::shared_ptr<const CBlock> shared_pchild = std::make_shared<const CBlock>(child);
    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pchild, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    CBlockIndex *pindexChild = LookupBlockIndex(child.GetHash());
    BOOST_REQUIRE(pindexChild != nullptr);
    BOOST_CHECK(HaveBodies(pindexChild));
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexChild), 0U);
    auto rangeUnlinked = chainman.m_blockman.m_blocks_unlinked.equal_range(pindexParent);
    bool childFoundUnlinked = std::any_of(rangeUnlinked.first, rangeUnlinked.second,
                                          [&](const auto &kv) { return kv.second == pindexChild; });
    BOOST_CHECK(childFoundUnlinked);

    // The parent's body now arrives, genuinely. Its own ReceivedBlockBodies
    // re-arm walk (1.3.1) must find the child in m_blocks_unlinked and
    // connect both blocks.
    std::shared_ptr<const CBlock> shared_pparent = std::make_shared<const CBlock>(parent);
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pparent, /*fForceProcessing=*/false, nullptr));

    BOOST_CHECK(HaveBodies(pindexParent));
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), child.GetHash().ToString());
    BOOST_CHECK_EQUAL(active.Height(), pindexRealTip->nHeight + 2);
}

// 1.3.5 (F-110, Mike, 2026-09-20): VerifyDB/reindex/startup re-pointing at the
// real bit. Investigation (Fable adversarial review) found most of the
// original plan already built and correct, but the mechanism is not what the
// plan assumed: ConnectTip's own !HaveBodies(pindexNew) guard (the "PERF"
// comment near SetBodiesMissing) and init.cpp:1193's BodiesMissing() handling
// are BOTH dead code today -- FindMostWorkChain's own fMissingData ancestor
// walk (F-109's mechanism) already excludes any bodies-missing candidate
// BEFORE ConnectTip is ever called on it, so ConnectTip's guard never fires
// in practice. This test pins that fact directly, so if FindMostWorkChain's
// filter is ever weakened, this assertion (not a crash) is the tripwire that
// says the OTHER guard has become load-bearing.
BOOST_AUTO_TEST_CASE(startup_activation_is_non_fatal_past_a_commitment_only_best_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    CBlock blockB = CreateBlock({}, coinbaseKey);
    uint256 hashB = blockB.GetHash();
    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // FindMostWorkChain already excluded B from setBlockIndexCandidates
    // during the ProcessNewBlock call above, so calling ActivateBestChain
    // again here would trivially find nothing to do -- not what a real
    // restart faces. LoadBlockIndex (validation.cpp) re-inserts every
    // BLOCK_VALID_TRANSACTIONS block with HaveTxsDownloaded() as a
    // candidate regardless of bodies (a commitment-only block satisfies
    // both), so at actual startup B genuinely IS a candidate again and
    // FindMostWorkChain's filter is what has to exclude it a second time.
    // Reproduce that real precondition directly (Fable review, 2026-09-20).
    chainstate.setBlockIndexCandidates.insert(pindexB);

    // The exact call shape init.cpp:1190-1208 makes at startup -- one loop,
    // used for both -reindex and -reindex-chainstate, no special-casing.
    CValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state, chainparams, nullptr));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!state.BodiesMissing());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

// The bodies-gap guard in VerifyDB (validation.cpp, "Same shape as the
// pruning case") is only load-bearing when the block's bytes are genuinely
// unreadable -- under today's storage format (F-110) a block's full bytes are
// always written to disk regardless of BLOCK_HAVE_BODIES, so clearing the bit
// alone leaves ReadBlockFromDisk succeeding and the guard proves nothing.
// g_perf_withhold_hashes is what actually makes ReadBlockFromDisk's
// CBlockIndex overload refuse the read (precedent: commitmentblock_tests.cpp's
// materialising-read test) -- combining both is what reproduces "commitments
// held, bodies missing" the way F-25's original bug needed to be caught.
BOOST_AUTO_TEST_CASE(verifydb_stops_at_a_bodies_gap_instead_of_reporting_corruption) {
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindexG = pindexTip->GetAncestor(pindexTip->nHeight - 2);
    BOOST_REQUIRE(pindexG != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexG));

    PerfWithholdGuard guard(pindexG->GetBlockHash());
    CBlock unreadable;
    BOOST_REQUIRE(!ReadBlockFromDisk(unreadable, pindexG, chainparams.GetConsensus()));  // sanity: withhold really blocks the read

    pindexG->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexG));

    // The guard (validation.cpp's !HaveBodies check, ahead of the doomed
    // read) stops the walk gracefully at both check level 3 (the default
    // path) and level 4 (the "try reconnecting" path) -- level 4 never
    // revisits G itself, only bodied blocks above it (F-110), but prove it
    // rather than assume it.
    BOOST_CHECK(CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/3, /*nCheckDepth=*/10));
    BOOST_CHECK(CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/4, /*nCheckDepth=*/10));

    // Control: restore the bit while the bytes are STILL unreadable -- this
    // is F-25's original bug shape (the index claims bodies are held; they
    // are not). Without the guard catching it earlier, VerifyDB attempts the
    // read, it fails, and the caller (init.cpp) reports "Corrupted block
    // database detected" -- exactly the false diagnosis 1.3.5 exists to
    // prevent for a genuine gap, reproduced here to prove the guard (not the
    // bytes) is what saves the true case above.
    pindexG->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_CHECK(!CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/3, /*nCheckDepth=*/10));

    // Restore real state before the withhold guard lifts.
    BOOST_REQUIRE(HaveBodies(pindexG));
}

// The -reindex/-loadblock limb: LoadExternalBlockFile's M-2 fix
// ((pindex->nStatus & BLOCK_HAVE_DATA) == 0 || !HaveBodies(pindex), instead of
// HAVE_DATA alone) has zero test coverage despite being named in 1.3.5's own
// title. Under today's storage format a blk*.dat entry is always a complete
// block (F-110) -- offering one for an index entry that is merely
// commitment-only is exactly how -reindex heals it back to fully-bodied,
// which is what this proves.
BOOST_AUTO_TEST_CASE(loadblock_fills_a_commitment_only_block_and_startup_connects_it) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    CBlock blockB = CreateBlock({}, coinbaseKey);
    uint256 hashB = blockB.GetHash();
    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));
    int nFileBefore = pindexB->nFile;
    unsigned int nDataPosBefore = pindexB->nDataPos;

    // Build a scratch blk*.dat-format file for B, the same framing
    // WriteBlockToDisk uses (message-start bytes, serialized size, the block).
    fs::path scratchPath = GetDataDir() / "scratch_blk_for_test.dat";
    {
        CAutoFile fileout(fsbridge::fopen(scratchPath, "wb+"), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!fileout.IsNull());
        unsigned int nSize = GetSerializeSize(fileout, blockB);
        fileout << chainparams.MessageStart() << nSize;
        fileout << blockB;
    }
    FILE *fileIn = fsbridge::fopen(scratchPath, "rb");
    BOOST_REQUIRE(fileIn != nullptr);
    LoadExternalBlockFile(chainparams, fileIn);  // takes ownership, closes fileIn

    BOOST_CHECK(HaveBodies(pindexB));
    // No second copy written -- the existing commitment record's own file
    // position is reused, not duplicated (same discipline as F-101's
    // unrequested-arrival test).
    BOOST_CHECK_EQUAL(pindexB->nFile, nFileBefore);
    BOOST_CHECK_EQUAL(pindexB->nDataPos, nDataPosBefore);

    // LoadExternalBlockFile only self-activates for genesis; B is not
    // genesis, so the startup sequence's own separate ActivateBestChain call
    // (init.cpp:1192) is what actually connects it.
    CValidationState state;
    BOOST_REQUIRE(::ChainstateActive().ActivateBestChain(state, chainparams, nullptr));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
    BOOST_CHECK_EQUAL(::ChainActive().Height(), pindexRealTip->nHeight + 1);
}

// 1.3.7 (Mike, 2026-09-20): InvalidateBlock / MarkConflictingBlock /
// EnforceBestChainLock -- audit, not full hardening (build-plan's own
// framing: confirm safe under the new state, leave full graceful
// degradation to 4.1). Scoped with a Fable review before writing anything,
// given this reaches into the LLMQ ChainLocks subsystem this session
// hadn't otherwise touched.
//
// Two findings from that review, both negative results worth recording
// rather than fixing:
//
// (1) The "rebuild setBlockIndexCandidates" loop InvalidateBlock and
// MarkConflictingBlock share checks HaveTxsDownloaded() but not
// HaveBodies() (validation.cpp), so it CAN insert a commitment-only block
// into the candidate set. This is not new risk -- ReceivedBlockTransactions
// and LoadBlockIndex already insert commitment-only blocks as candidates
// on arrival and at restart with the identical check (the
// startup_activation_... test above already relies on that).
// FindMostWorkChain (F-109) is the one, already-tested filter, and nothing
// else reads the set unsafely: no RPC, no net_processing.cpp reference: no
// consumer of setBlockIndexCandidates outside FindMostWorkChain,
// PruneBlockIndexCandidates, ResetBlockFailureFlags and PreciousBlock, all
// of which use the identical insertion check.
//
// (2) EnforceBestChainLock's (llmq/quorums_chainlocks.cpp) backward pprev
// walk terminates safely at genesis (always on ChainActive, so the loop's
// own !ChainActive().Contains(pindex) condition stops it before a null
// pprev is ever dereferenced), and its only assert(false) path
// (MarkConflictingBlock returning false) requires DisconnectTip to fail,
// reachable only for a block that m_chain.Contains() -- which a
// commitment-only block can never be (ConnectTip refuses to connect one,
// F-109). The row's own named "ReadBlockFromDisk nullptr fallback reaching
// an IS-conflict-detection caller" is TrySignChainTip's GetBlockTxs walk
// (same file) -- rooted at ::ChainActive().Tip() and never walking more
// than 5 blocks back, so by the same invariant (the active chain is never
// commitment-only) it can never reach one either. Safe by construction;
// this comment is that written conclusion, not a test, since there is
// nothing reachable to construct a test around.
//
// What IS worth testing directly, below: that InvalidateBlock and
// MarkConflictingBlock genuinely don't crash or corrupt CheckBlockIndex's
// invariants with a commitment-only sibling in play, on either side of the
// call -- and that marking a full sibling conflicting (MarkConflictingBlock's
// own shape, the exact operation EnforceBestChainLock performs on a losing
// sibling) doesn't prevent a commitment-only rival from later healing in.
//
// Shared shape: T is the real tip; A is a full child of T that connects and
// becomes tip; B is an equal-work commitment-only child of T that never
// connects (same construction as equal_work_tiebreak_survives_bodies_
// arriving_second above). MarkConflictingBlock's own docs annotate
// UpdateMempoolForReorg's requirement on ::mempool.cs, unlike
// InvalidateBlock which takes it internally -- a pre-existing annotation
// gap (MarkConflictingBlock never takes the lock itself) that costs
// nothing to build around here, so every call below is under LOCK2.
// CreateBlock is a TestChainSetup fixture member, unreachable from a free
// function -- callers build blockA/blockB themselves (CreateBlock({},
// coinbaseKey) and CreateBlock({}, an independent scriptPubKey) so they are
// genuinely distinct, equal-work children of the real tip) and this helper
// only drives the shared process-and-assert sequence.
static void ProcessEqualWorkSiblingsAWinsBWithheld(ChainstateManager &chainman, const CChainParams &chainparams,
                                                   const CBlock &blockA, const CBlock &blockB, uint256 &hashA,
                                                   uint256 &hashB, std::shared_ptr<const CBlock> &shared_pblockB) {
    BOOST_REQUIRE(blockA.GetHash() != blockB.GetHash());

    shared_pblockB = std::make_shared<const CBlock>(blockB);
    std::shared_ptr<const CBlock> shared_pblockA = std::make_shared<const CBlock>(blockA);
    hashA = blockA.GetHash();
    hashB = blockB.GetHash();

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    BOOST_REQUIRE(!HaveBodies(LookupBlockIndex(hashB)));

    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockA, /*fForceProcessing=*/true, nullptr));
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());
}

static CBlock CreateSiblingBlock(TestChainSetup &fixture) {
    CKey key;
    key.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
    return fixture.CreateBlock({}, scriptPubKey);
}

BOOST_AUTO_TEST_CASE(invalidateblock_is_safe_with_a_commitment_only_sibling) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, chainparams, LookupBlockIndex(hashA)));
    // The rebuild loop's own known gap (finding (1) above): B, still
    // commitment-only, is reinstated as a candidate anyway. Recorded, not
    // asserted against -- it is what the comment above says is harmless.
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 1U);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // FindMostWorkChain must still refuse to select B despite it being a
    // (wrongly, but harmlessly) reinstated candidate.
    CValidationState state2;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state2, chainparams, nullptr));
    BOOST_CHECK(!state2.BodiesMissing());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

BOOST_AUTO_TEST_CASE(markconflictingblock_is_safe_with_a_commitment_only_sibling) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexA = LookupBlockIndex(hashA);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.MarkConflictingBlock(state, chainparams, pindexA));
    BOOST_CHECK(pindexA->nStatus & BLOCK_CONFLICT_CHAINLOCK);
    BOOST_CHECK(!(pindexB->nStatus & BLOCK_CONFLICT_CHAINLOCK));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    CValidationState state2;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state2, chainparams, nullptr));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

// The build-plan row's own literal wording: these two functions invoked
// directly ON a commitment-only block, off the active chain entirely --
// the pure flag-setting path (pindex_was_in_chain stays false, so neither
// function ever reaches DisconnectTip at all).
BOOST_AUTO_TEST_CASE(invalidate_and_markconflicting_are_safe_called_directly_on_a_commitment_only_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(!::ChainActive().Contains(pindexB));

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, chainparams, pindexB));
    BOOST_CHECK(pindexB->nStatus & BLOCK_FAILED_VALID);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());

    // Reset and repeat for MarkConflictingBlock on a second, freshly-built
    // commitment-only sibling C, so the two calls don't interfere.
    ::ResetBlockFailureFlags(pindexB);

    CKey keyC;
    keyC.MakeNewKey(true);
    CScript scriptPubKeyC = CScript() << ToByteVector(keyC.GetPubKey()) << OP_CHECKSIG;
    CBlock blockC = CreateBlock({}, scriptPubKeyC);
    std::shared_ptr<const CBlock> shared_pblockC = std::make_shared<const CBlock>(blockC);
    uint256 hashC = blockC.GetHash();
    BOOST_REQUIRE(hashC != hashA && hashC != hashB);
    {
        PerfWithholdGuard guard(hashC);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockC, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexC = LookupBlockIndex(hashC);
    BOOST_REQUIRE(!HaveBodies(pindexC));

    CValidationState state2;
    BOOST_REQUIRE(chainstate.MarkConflictingBlock(state2, chainparams, pindexC));
    BOOST_CHECK(pindexC->nStatus & BLOCK_CONFLICT_CHAINLOCK);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());
}

// EnforceBestChainLock's own operation on a losing sibling, reproduced
// directly (driving the real CChainLocksHandler needs an enabled spork, a
// BLS quorum and a signed recovered signature -- disproportionate scaffolding
// for what is, per the Fable review, entirely expressible through
// MarkConflictingBlock's own behaviour: EnforceBestChainLock's loop calls
// exactly this, on exactly a losing sibling of its chain-locked target).
// The commitment-only side of the story: A (full, the loser here) gets
// marked conflicting exactly as EnforceBestChainLock would when a ChainLock
// commits to a DIFFERENT chain than the one A extends -- and B (commitment-
// only) must still be able to heal in afterward once its real body arrives,
// proving MarkConflictingBlock's bookkeeping doesn't leave B permanently
// stranded alongside its now-conflicting rival.
BOOST_AUTO_TEST_CASE(a_commitment_only_block_still_heals_after_its_sibling_is_marked_conflicting) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexA = LookupBlockIndex(hashA);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState state;
        BOOST_REQUIRE(chainstate.MarkConflictingBlock(state, chainparams, pindexA));
    }
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    // Same known, harmless gap as invalidateblock_is_safe_...: the rebuild
    // loop reinstates B (still commitment-only) regardless of bodies.
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 1U);

    // B's real body now arrives, unrequested -- must still heal in, not be
    // stranded by its now-conflicting rival's bookkeeping.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/false, nullptr));
    BOOST_CHECK(HaveBodies(pindexB));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
    // A must never come back, having been marked conflicting.
    BOOST_CHECK(pindexA->nStatus & BLOCK_CONFLICT_CHAINLOCK);
}

// M-5 (full-arc adversarial review, F-118): the pre-1.3 BLOCK_HAVE_BODIES
// migration in LoadBlockIndexDB (validation.cpp) had zero test coverage.
// Reproduces the on-disk shape a pre-1.3 datadir actually has -- HAVE_DATA
// set, HAVE_BODIES clear, exactly what every entry looked like before this
// bit existed -- by clearing HAVE_BODIES directly on already-connected
// entries (their bytes are genuinely on disk, matching a real pre-1.3 node)
// rather than via g_perf_withhold_hashes (which also makes ReadBlockFromDisk
// refuse the read -- the wrong shape here; a pre-1.3 block was never
// withheld, it just predates the bit).
BOOST_AUTO_TEST_CASE(bodiesmigrated_migration_restores_have_bodies_on_pre_1_3_entries) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindex1 = pindexTip->GetAncestor(pindexTip->nHeight - 5);
    CBlockIndex *pindex2 = pindexTip->GetAncestor(pindexTip->nHeight - 2);
    BOOST_REQUIRE(pindex1 != nullptr);
    BOOST_REQUIRE(pindex2 != nullptr);
    BOOST_REQUIRE(pindex1->nStatus & BLOCK_HAVE_DATA);
    BOOST_REQUIRE(pindex2->nStatus & BLOCK_HAVE_DATA);

    {
        LOCK(cs_main);
        pindex1->nStatus &= ~BLOCK_HAVE_BODIES;
        pindex2->nStatus &= ~BLOCK_HAVE_BODIES;
        BOOST_REQUIRE(pblocktree->WriteFlag("bodiesmigrated", false));
    }
    BOOST_REQUIRE(!HaveBodies(pindex1));
    BOOST_REQUIRE(!HaveBodies(pindex2));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    BOOST_CHECK(HaveBodies(pindex1));
    BOOST_CHECK(HaveBodies(pindex2));
    bool fBodiesMigrated = false;
    pblocktree->ReadFlag("bodiesmigrated", fBodiesMigrated);
    BOOST_CHECK(fBodiesMigrated);
}

// The downgrade/upgrade corruption risk the migration's own comment names:
// once "bodiesmigrated" is true, a GENUINE commitment-only entry (Phase 2)
// must never be swept up by a later, redundant migration run. Simulated here
// by clearing HAVE_BODIES on a fresh entry AFTER the flag is already set --
// the shape a real commitment-only block has the moment Phase 2 exists.
BOOST_AUTO_TEST_CASE(bodiesmigrated_migration_does_not_touch_a_later_genuine_commitment_only_entry) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindexGenuine = pindexTip->GetAncestor(pindexTip->nHeight - 3);
    BOOST_REQUIRE(pindexGenuine != nullptr);
    BOOST_REQUIRE(pindexGenuine->nStatus & BLOCK_HAVE_DATA);

    {
        // First pass: an empty/true-fresh datadir migrates nothing (nothing
        // is HAVE_DATA-without-HAVE_BODIES yet) but still sets the flag,
        // matching what a real first boot after upgrading to this binary does.
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }
    bool fBodiesMigrated = false;
    pblocktree->ReadFlag("bodiesmigrated", fBodiesMigrated);
    BOOST_REQUIRE(fBodiesMigrated);

    {
        LOCK(cs_main);
        pindexGenuine->nStatus &= ~BLOCK_HAVE_BODIES;
    }
    BOOST_REQUIRE(!HaveBodies(pindexGenuine));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    // Must still be genuinely bodies-missing -- the flag being set is what
    // stops the second pass from wrongly re-stamping it.
    BOOST_CHECK(!HaveBodies(pindexGenuine));
}

// 2.1.4: the body store's own write side, wired into AcceptBlock. Builds a
// real spend of a mature coinbase so the block carries a genuine non-coinbase
// body, not just a coinbase-only block (which would leave GetBodyRecordSerializedSize's
// count-only path untested here).
static CMutableTransaction MakeSpendOfCoinbase(const CTransactionRef &coinbase, const CKey &coinbaseKeyIn) {
    CBasicKeyStore keystore;
    keystore.AddKey(coinbaseKeyIn);
    CMutableTransaction spendTx;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(coinbase->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = GetScriptForDestination(coinbaseKeyIn.GetPubKey().GetID());
    BOOST_REQUIRE(SignSignature(keystore, *coinbase, spendTx, 0, SIGHASH_ALL));
    return spendTx;
}

// The core mechanism: an accepted block's non-coinbase transactions must
// actually be readable back from the body store, at the exact position the
// index now records -- not just "some bit got set".
BOOST_AUTO_TEST_CASE(accepted_block_writes_its_bodies_to_the_body_store) {
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);

    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);

    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE(!pindex->GetBodyPos().IsNull());

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pindex->GetBodyPos(), bodiesOut));
    // Index 0 is vtx[1] -- the coinbase is never stored here (bodystore.h's
    // own indexing convention).
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    for (size_t i = 0; i < bodiesOut.size(); i++) {
        BOOST_CHECK(bodiesOut[i]->GetHash() == block.vtx[i + 1]->GetHash());
    }
}

// Mirrors unrequested_arrival_fills_a_commitment_only_block above, for
// BLOCK_HAVE_BODY_RECORD: a withheld block's real bytes are written to the
// body store on the FIRST accept (SaveBodyToDisk is unconditional, matching
// SaveBlockToDisk -- F-131's "must not special-case a withheld accept" write
// path), but the STATUS bit lags until the bodies "arrive" (a later
// unrequested re-offer), exactly the way BLOCK_HAVE_BODIES itself already
// works. No second write happens on arrival -- nBodyFile/nBodyPos are
// unchanged, only ReceivedBlockBodies's status bit changes.
BOOST_AUTO_TEST_CASE(withheld_bodies_are_written_but_the_record_bit_lags_until_arrival) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    int nFileAfterFirstAccept = -1;
    unsigned int nPosAfterFirstAccept = 0;
    {
        PerfWithholdGuard guard(hash);
        bool fNewBlock = false;
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
        BOOST_REQUIRE(fNewBlock);

        const CBlockIndex *pindex = LookupBlockIndex(hash);
        BOOST_REQUIRE(pindex != nullptr);
        // GetBodyPos() is gated on the status bit (mirrors GetBlockPos()'s own
        // convention, proven in bodystore_tests.cpp) so it is still null here
        // -- read the raw fields directly to see that the real bytes were
        // written anyway, only the index's CLAIM to hold them is withheld,
        // mirroring BLOCK_HAVE_BODIES exactly.
        BOOST_CHECK(pindex->GetBodyPos().IsNull());
        BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODY_RECORD));
        BOOST_CHECK(pindex->nBodyFile >= 0);
        nFileAfterFirstAccept = pindex->nBodyFile;
        nPosAfterFirstAccept = pindex->nBodyPos;
    }

    bool fNewBlock2 = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    BOOST_CHECK(fNewBlock2);

    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE(!pindex->GetBodyPos().IsNull());
    // No second copy: the position is unchanged from the withheld accept.
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nFile, nFileAfterFirstAccept);
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nPos, nPosAfterFirstAccept);

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pindex->GetBodyPos(), bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    BOOST_CHECK(bodiesOut[0]->GetHash() == block.vtx[1]->GetHash());
}

// F-133's second recorded constraint, proven rather than merely argued: the
// write side (AcceptBlock -> SaveBodyToDisk -> FindBodyPos) and the load side
// (LoadBodyFileInfo, now wired into LoadBlockIndexDB) must actually agree
// after a real restart, not just each work in isolation the way
// bodystore_tests.cpp's own restart test already proved with hand-fed data.
// This drives the SAME real LoadBlockIndexDB call
// bodiesmigrated_migration_restores_have_bodies_on_pre_1_3_entries above
// uses, so it is a real reload, not a simulation of one.
BOOST_AUTO_TEST_CASE(body_store_bookkeeping_survives_a_real_loadblockindexdb_reload) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    FlatFilePos bodyPos = pindex->GetBodyPos();

    // The real flush pipeline's body-file half: GetDirtyBodyFileInfo folded
    // into WriteBatchSync (F-132/F-133), for an entry FindBodyPos actually
    // marked dirty during a real AcceptBlock -- not a hand-built
    // CBodyFileInfo the way bodystore_tests.cpp's own tests use. (Calling the
    // full CChainState::FlushStateToDisk directly from this fixture is not
    // this test's job and is unsafe here -- it assumes a shutdown sequence
    // this fixture does not run, unrelated to the body store.)
    std::vector<std::pair<int, CBodyFileInfo>> vBodyFiles;
    int nLastBodyFileOut = -1;
    GetDirtyBodyFileInfo(vBodyFiles, nLastBodyFileOut);
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, vBodyFiles, nLastBodyFileOut));

    // Simulate a restart of bodystore.cpp's own in-memory bookkeeping (the
    // bytes on disk and the CBlockIndex entries are untouched -- only the
    // process-global FindBodyPos state resets, the same thing a real process
    // restart would do).
    TestOnlyResetBodyFileState();
    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    // FindBodyPos must continue from the persisted position, not silently
    // restart at file 0 and overwrite the block this test just accepted.
    // Computed independent of the reload (from the block's own content, not
    // from any post-reload state) so a mutant that drops LoadBodyFileInfo's
    // wiring can't pass by having both sides of the comparison collapse to
    // the same (wrong) zero together.
    uint64_t expectedRecordSize = GetBodyRecordSerializedSize(
            std::vector<CTransactionRef>(block.vtx.begin() + 1, block.vtx.end()));
    unsigned int expectedFileSizeAfterThisBlock = bodyPos.nPos + (unsigned int) expectedRecordSize;
    BOOST_REQUIRE_GT(expectedFileSizeAfterThisBlock, 0U);

    unsigned int nSizeAfterReload = TestOnlyGetBodyFileSize(bodyPos.nFile);
    BOOST_CHECK_EQUAL(nSizeAfterReload, expectedFileSizeAfterThisBlock);

    FlatFilePos posAfter;
    BOOST_REQUIRE(FindBodyPos(posAfter, 1));
    BOOST_CHECK_EQUAL(posAfter.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posAfter.nPos, expectedFileSizeAfterThisBlock);

    // And the accepted block's own bytes are still correct through the
    // reloaded state -- the reload only affects in-memory bookkeeping.
    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(bodyPos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    BOOST_CHECK(bodiesOut[0]->GetHash() == block.vtx[1]->GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
