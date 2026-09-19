// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 1.3.1 (Mike, 2026-09-19): the real per-block BLOCK_HAVE_BODIES bit, ported
// and hardened from probe/acceptance-layer (a2d5f68c9, 37a7c37a0) -- the
// commitment block is always written (BLOCK_HAVE_DATA stays honest), and
// BLOCK_HAVE_BODIES is set separately by ReceivedBlockBodies. F-100 corrected
// the plan draft that described a vacuous encoding where the bit collapsed
// onto BLOCK_HAVE_DATA; these tests exercise the real, distinct bit.

#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <key.h>
#include <node/context.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
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

BOOST_AUTO_TEST_SUITE_END()
