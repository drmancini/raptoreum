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
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <key.h>
#include <node/context.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <txmempool.h>
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
// tip), must still WIN the tie once its body fills in -- exactly what B
// would have won had it arrived whole first, matching unmodified Bitcoin's
// own first-seen tie-break (CBlockIndexWorkComparator, lower nSequenceId
// wins). F-35's bug was the opposite: re-stamping on body arrival would move
// B's sequence id LATER than A's, making B lose a tie it should have won.
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

BOOST_AUTO_TEST_SUITE_END()
