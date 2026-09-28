// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.6.1 (F-207): proves the real per-height gate this sub-step wired up --
// EUpdate::COMMITMENT_MODE (update.h) and CommitmentModeAtHeight
// (validation.h) -- actually distinguishes a block before a real
// activation height from a block at/after it, closing TODO(4.6). Two
// levels, deliberately not just one (mutation-check requirement, per this
// sub-step's own process discipline):
//
//  1. connectblock_relocated_rule_is_height_gated_not_globally_on: the SAME
//     technique commitmentblock_tests.cpp's own
//     connectblock_enforces_nonfinal_coinbase_when_relocation_is_active
//     already uses (a direct, fJustCheck=true ConnectBlock call against a
//     hand-built CBlockIndex at real-tip+1), run twice against the SAME
//     violating shape -- once below one real registered activation height,
//     once at it -- with one genuine, ordinary block mined in between to
//     advance the real tip itself (rather than fabricating a second
//     CBlockIndex hop off a single, unmoving tip: that alternative was
//     tried first and abandoned, since ConnectBlock's own evoDb/coins-view
//     "does this view match the previous block" assertions hold only
//     against the REAL, currently-committed chain state, not a synthetic
//     multi-hop pprev chain -- see the test body's own comment). This is
//     the direct "pre-activation block and post-activation block treated
//     differently" proof.
//
//  2. commitment_mode_at_height_reflects_each_historical_blocks_own_height:
//     drives CommitmentModeAtHeight itself -- the exact mechanism
//     ConnectBlock/AcceptBlock install -- against two REAL, already-
//     connected historical CBlockIndex pointers from one real mined chain,
//     with the activation height registered only AFTER both blocks already
//     exist. This is the closest a boost unit test gets to a
//     `-reindex-chainstate`-shaped scenario: it proves that replaying an
//     EARLIER block still reads that block's OWN height against the gate,
//     not whatever the chain's current tip happens to be. A literal
//     process-level `-reindex-chainstate` run (a real daemon restarted with
//     that flag against an existing datadir) is out of scope for this
//     binary -- there is no second process/re-read here -- and would
//     belong in test/functional as a python integration test instead; see
//     this sub-step's own report for why that was not additionally built.

#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/evodb.h>
#include <primitives/block.h>
#include <test/test_raptoreum.h>
#include <update/update.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

namespace commitmentmode_activation_test_helpers {

// A huge, never-reached-by-any-test-chain height, used to leave the bit
// registered but permanently inert once a test is done with it -- mirrors
// commitmentblock_tests.cpp/acceptancebit_tests.cpp's own identical
// constant, duplicated locally rather than shared (matching this codebase's
// own existing per-file CommitmentBudgetGuard convention).
static const int64_t COMMITMENT_MODE_TEST_NEVER_HEIGHT = 2000000000;

static Update MakeCommitmentModeUpdate(int64_t heightActivated) {
    return Update(EUpdate::COMMITMENT_MODE, "Commitment Mode (test)", 3, 1, 0, 1, 1, 0, false,
                 VoteThreshold(0, 0, 1), VoteThreshold(0, 0, 1), false, heightActivated);
}

// RAII: registers a real activation height against the live UpdateManager
// (Updates(), the same object CommitmentModeAtHeight/ConnectBlock/
// AcceptBlock all query) for the guard's lifetime, then resets it to
// permanently inert -- matching the RAII discipline F6 already established
// in this codebase (a thrown BOOST_REQUIRE must not leave state stuck on
// for later tests in the same process).
struct ActivationHeightGuard {
    explicit ActivationHeightGuard(int64_t heightActivated) {
        Updates().Add(MakeCommitmentModeUpdate(heightActivated));
    }
    ~ActivationHeightGuard() {
        Updates().Add(MakeCommitmentModeUpdate(COMMITMENT_MODE_TEST_NEVER_HEIGHT));
        g_commitmentBudgetActive = false;
    }
};

}  // namespace commitmentmode_activation_test_helpers

using namespace commitmentmode_activation_test_helpers;

BOOST_FIXTURE_TEST_SUITE(commitmentmode_activation_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(connectblock_relocated_rule_is_height_gated_not_globally_on) {
    // Identical violating shape to commitmentblock_tests.cpp's own
    // connectblock_enforces_nonfinal_coinbase_when_relocation_is_active: a
    // coinbase with a future absolute nLockTime and a non-final nSequence,
    // rejected as bad-txns-nonfinal by F-44's relocated ConnectBlock check
    // -- but ONLY once EUpdate::COMMITMENT_MODE is genuinely active at the
    // height being connected. IsFinalTx compares nLockTime (999999) against
    // pindex->nHeight directly, so the "still future" verdict holds
    // regardless of which of the two heights below is used -- the only
    // variable under test is whether the relocated check runs AT ALL.
    //
    // Both direct-ConnectBlock calls below use the EXACT same
    // pindexPrev->nHeight + 1 shape as commitmentblock_tests.cpp's own
    // precedent -- deliberately, not incidentally. Two things this test
    // tried first and had to abandon, both discovered by actually running
    // it rather than assumed away: (1) a single hand-built CBlockIndex at
    // an arbitrary large height (originally 100000) desyncs the block's
    // coinbase (built by the real BlockAssembler against the real, nearby
    // chain tip) from regtest's own real, unrelated height-gated consensus
    // rules -- founder payment starts at height 500 (chainparams.cpp) and
    // fired bad-cb-founder-payment-not-found before the intended relocated
    // check was ever reached; (2) chaining two fabricated CBlockIndex
    // levels off the real tip to get a genuine one-height gap between two
    // sub-scenarios in a single ConnectBlock pair breaks OTHER real
    // invariants ConnectBlock enforces beyond just nHeight/pprev
    // self-consistency -- evoDb's own VerifyBestBlock and the coins view's
    // GetBestBlock both compare against the REAL, currently-committed chain
    // state, which a synthetic second-level pprev can't satisfy no matter
    // how its height/hash fields are set. The fix that actually works:
    // keep every direct ConnectBlock call at real-tip+1 (matching the
    // precedent exactly, so every invariant holds), and advance the REAL
    // chain by one genuine, ordinary block between the "below" and "at"
    // sub-scenarios instead of fabricating a second hop.
    auto makeNonFinalCoinbaseBlock = [&]() {
        CBlock block = CreateBlock({}, coinbaseKey);
        CMutableTransaction mutCoinbase(*block.vtx[0]);
        mutCoinbase.nLockTime = 999999;
        mutCoinbase.vin[0].nSequence = 1;
        block.vtx[0] = MakeTransactionRef(mutCoinbase);
        return block;
    };

    const int64_t activationHeight = ::ChainActive().Height() + 2;
    ActivationHeightGuard guard(activationHeight);

    LOCK(cs_main);

    // Below activation: CommitmentModeAtHeight must pin the flag OFF for
    // this whole ConnectBlock call, so the relocated rule never runs.
    {
        CBlock block = makeNonFinalCoinbaseBlock();
        CBlockIndex *pindexPrev = ::ChainActive().Tip();
        uint256 block_hash(block.GetHash());
        CBlockIndex indexDummy(block);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        indexDummy.phashBlock = &block_hash;
        BOOST_REQUIRE(indexDummy.nHeight < activationHeight);

        CCoinsViewCache viewNew(&::ChainstateActive().CoinsTip());
        auto dbTx = evoDb->BeginTransaction();  // rolled back when dbTx goes out of scope
        CValidationState state;
        bool ok = ::ChainstateActive().ConnectBlock(block, state, &indexDummy, viewNew, Params(),
                                                    passetsCache.get(), /*fJustCheck=*/true);
        BOOST_CHECK(ok);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "");
    }

    // Advance the REAL chain by exactly one ordinary (non-violating) block,
    // so the "at activation" scenario below can use the identical,
    // precedent-matching real-tip+1 pattern against a genuinely new,
    // fully-consistent tip -- not a fabricated one.
    CreateAndProcessBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(::ChainActive().Height(), activationHeight - 1);

    // At activation: the same violating shape (rebuilt against the now-
    // advanced real tip, since CreateBlock()'s coinbase must match it),
    // now rejected -- proving the difference is the height, not some other
    // confound between the two sub-scenarios (same violating shape, same
    // real-tip+1 construction, only the real tip itself has moved).
    {
        CBlock block = makeNonFinalCoinbaseBlock();
        CBlockIndex *pindexPrev = ::ChainActive().Tip();
        uint256 block_hash(block.GetHash());
        CBlockIndex indexDummy(block);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        indexDummy.phashBlock = &block_hash;
        BOOST_REQUIRE_EQUAL(indexDummy.nHeight, activationHeight);

        CCoinsViewCache viewNew(&::ChainstateActive().CoinsTip());
        auto dbTx = evoDb->BeginTransaction();
        CValidationState state;
        bool ok = ::ChainstateActive().ConnectBlock(block, state, &indexDummy, viewNew, Params(),
                                                    passetsCache.get(), /*fJustCheck=*/true);
        BOOST_CHECK(!ok);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }
}

BOOST_AUTO_TEST_CASE(commitment_mode_at_height_reflects_each_historical_blocks_own_height) {
    // Mine past TestChain100Setup's own height 100 with NO activation
    // registered at all yet -- these blocks are indexed exactly like any
    // ordinary regtest history a `-reindex-chainstate` would later replay.
    for (int i = 0; i < 10; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
    }
    BOOST_REQUIRE_EQUAL(::ChainActive().Height(), 110);

    CBlockIndex *pindexEarly = ::ChainActive()[105];   // pre-activation, real, already connected
    CBlockIndex *pindexLate = ::ChainActive()[110];    // post-activation, real, already connected
    BOOST_REQUIRE(pindexEarly != nullptr);
    BOOST_REQUIRE(pindexLate != nullptr);

    // Register the activation height ONLY NOW, after both blocks already
    // exist in the index -- exactly the situation a `-reindex-chainstate`
    // replay (or a node simply catching up to a later-decided activation
    // height) faces: history already exists; the gate must still tell
    // early history and late history apart correctly by height, not by
    // when the bit itself was registered.
    ActivationHeightGuard guard(108);

    LOCK(cs_main);

    // CommitmentModeAtHeight is the EXACT mechanism ConnectBlock/AcceptBlock
    // install (validation.cpp) -- driving it directly here, against real
    // historical pindexes, proves the production code path, not a
    // reimplementation of it.
    {
        CommitmentModeAtHeight atEarly(pindexEarly);
        BOOST_CHECK(!g_commitmentBudgetActive);
    }
    {
        CommitmentModeAtHeight atLate(pindexLate);
        BOOST_CHECK(g_commitmentBudgetActive);
    }

    // And directly through the same Updates().IsActive() call
    // CommitmentModeAtHeight itself makes, for good measure.
    BOOST_CHECK(!Updates().IsActive(EUpdate::COMMITMENT_MODE, pindexEarly));
    BOOST_CHECK(Updates().IsActive(EUpdate::COMMITMENT_MODE, pindexLate));
}

BOOST_AUTO_TEST_SUITE_END()
