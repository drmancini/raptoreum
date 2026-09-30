// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Reproduces a real bug found on a live two-node devnet (not predicted from
// source): miner.cpp's own block-version computation used
// Updates().ComputeBlockVersion(pindexPrev) -- the PARENT -- while
// validation.cpp's "is this an unknown/expected version" check uses
// pindexNew, the new block itself. UpdateManager::State's own roundNumber
// math (update/update.cpp) uses blockIndex->nHeight directly with no
// adjustment, so ComputeBlockVersion(X) answers "what does X's OWN height
// expect" -- passing the parent evaluates one height too early. The two
// reference points agree almost always, EXCEPT exactly at the block where a
// deployment's state crosses from LockedIn into Active (ComputeBlockVersion
// stops including a bit once Active, per update.cpp's own
// State==Voting||LockedIn check) -- there, the miner (still looking at the
// LockedIn parent) sets the bit, and the validator (looking at the
// already-Active new block) no longer expects it, producing a real,
// log-visible "Warning: unknown new rules activated" on every node that
// ever crosses this exact boundary.
//
// The fix is UpdateManager::ComputeNextBlockVersion(pindexPrev, nHeight):
// it deliberately does NOT construct a CBlockIndex for the not-yet-existing
// block being mined. State() and the per-update GetVote() caches key their
// results by raw CBlockIndex* for the lifetime of the UpdateManager: a
// synthetic/stack CBlockIndex passed there would eventually have its
// address reused by an unrelated, real CBlockIndex once its stack frame
// was gone, silently aliasing a stale cached result onto the wrong block --
// found the hard way, when an earlier version of this fix (passing a
// lookahead CBlockIndex straight into ComputeBlockVersion) corrupted the
// real, process-lifetime UpdateManager singleton and cascaded into 594
// unrelated test failures. ComputeNextBlockVersion instead queries State()
// only on the real, persistent pindexPrev (cache-safe, same as every other
// caller) and corrects for the LockedIn->Active transition using the
// already-known FinalHeight field -- no synthetic CBlockIndex, no new
// vote-counting logic.

#include <chain.h>
#include <test/test_raptoreum.h>
#include <update/update.h>

#include <boost/test/unit_test.hpp>

#include <deque>

namespace updatemanager_versionbits_test_helpers {

// A standalone UpdateManager with its own tiny, isolated Update
// registration -- deliberately NOT touching chainparams.cpp or any real
// network's registered bits, so this test needs no real chain, no real
// mining, and cannot affect or be affected by any real network's own
// registrations. roundSize=2/votingPeriod=1/graceRounds=1 reaches
// Voting -> LockedIn -> Active in a handful of blocks instead of the real
// devnet's 100+.
constexpr int TEST_BIT = 5;
constexpr int64_t TEST_ROUND_SIZE = 2;
constexpr int64_t TEST_START_HEIGHT = 0;
constexpr int64_t TEST_VOTING_PERIOD = 1;
constexpr int64_t TEST_VOTING_MAX_ROUNDS = 5;
constexpr int64_t TEST_GRACE_ROUNDS = 1;

// UpdateManager holds a mutex, so it cannot be returned by value; callers
// construct their own UpdateManager um; and call this to register the
// test's Update onto it.
void RegisterTestUpdate(UpdateManager &um) {
    um.Add(Update(EUpdate::COMMITMENT_MODE, std::string("Test Update"), TEST_BIT, TEST_ROUND_SIZE,
                  TEST_START_HEIGHT, TEST_VOTING_PERIOD, TEST_VOTING_MAX_ROUNDS, TEST_GRACE_ROUNDS,
                  /*forcedUpdate=*/false, VoteThreshold(85, 85, 1), VoteThreshold(0, 0, 1)));
}

// A chain of bare CBlockIndex nodes linked via pprev, every one signaling
// TEST_BIT (100% miner signal -- resolves the vote as fast as the
// parameters allow). Stored in a deque so pprev pointers into it stay
// valid as more blocks are appended (unlike a vector, which can
// reallocate and invalidate every existing element's address).
struct FabricatedChain {
    std::deque<CBlockIndex> blocks;

    CBlockIndex *TipAtHeight(int64_t height) {
        for (auto &b : blocks)
            if (b.nHeight == height) return &b;
        return nullptr;
    }

    // Extends the chain up to and including targetHeight, starting from
    // genesis (height 0) if not already built.
    void ExtendTo(int64_t targetHeight) {
        int64_t nextHeight = blocks.empty() ? 0 : blocks.back().nHeight + 1;
        for (int64_t h = nextHeight; h <= targetHeight; ++h) {
            blocks.emplace_back();
            CBlockIndex &idx = blocks.back();
            idx.nHeight = h;
            idx.nVersion = VERSIONBITS_TOP_BITS | (uint32_t(1) << TEST_BIT);
            idx.pprev = (h == 0) ? nullptr : TipAtHeight(h - 1);
        }
    }
};

}  // namespace updatemanager_versionbits_test_helpers

using namespace updatemanager_versionbits_test_helpers;

BOOST_FIXTURE_TEST_SUITE(updatemanager_versionbits_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(miner_lookahead_version_matches_validator_expectation_at_locked_in_to_active_boundary) {
    UpdateManager um;
    RegisterTestUpdate(um);
    FabricatedChain chain;

    // Round 0 is heights [0,1] (partial, per GetVote's own
    // "height < startHeight+roundSize" guard); round 1 is [2,3], the first
    // round GetVote actually counts. With 100% signal and
    // votingPeriod=1/minerThreshold 85%, the vote resolves (LockedIn) at
    // the FIRST counted round boundary: height 2. Confirmed by walking the
    // real state forward rather than assumed.
    int64_t probeHeight = 2;
    EUpdateState stateAtProbe = EUpdateState::Defined;
    while (stateAtProbe != EUpdateState::LockedIn && probeHeight < 200) {
        chain.ExtendTo(probeHeight);
        stateAtProbe = um.State(EUpdate::COMMITMENT_MODE, chain.TipAtHeight(probeHeight)).State;
        if (stateAtProbe != EUpdateState::LockedIn) probeHeight += TEST_ROUND_SIZE;
    }
    BOOST_REQUIRE_EQUAL((int) stateAtProbe, (int) EUpdateState::LockedIn);
    int64_t lockedInHeight = probeHeight;

    // FinalHeight (update/update.h's own StateInfo doc: "Active height when
    // state in (LockedIn, Active)") tells us exactly where Active begins --
    // read directly from the real State() result, not computed by hand
    // from the grace-round formula, so this test does not silently rely on
    // its own understanding of that math being right.
    int64_t activeHeight = um.State(EUpdate::COMMITMENT_MODE, chain.TipAtHeight(lockedInHeight)).FinalHeight;
    BOOST_REQUIRE_GT(activeHeight, lockedInHeight);
    chain.ExtendTo(activeHeight);

    CBlockIndex *pindexParent = chain.TipAtHeight(activeHeight - 1);
    CBlockIndex *pindexActive = chain.TipAtHeight(activeHeight);
    BOOST_REQUIRE(pindexParent != nullptr);
    BOOST_REQUIRE(pindexActive != nullptr);

    BOOST_CHECK(um.State(EUpdate::COMMITMENT_MODE, pindexParent).State == EUpdateState::LockedIn);
    BOOST_CHECK(um.State(EUpdate::COMMITMENT_MODE, pindexActive).State == EUpdateState::Active);

    // Precondition, not the regression guard: confirms this scenario
    // genuinely reaches the disagreement this test exists to catch, and
    // will remain true forever regardless of any fix -- ComputeBlockVersion
    // on the raw parent (still LockedIn) and on the raw child (already
    // Active) are SUPPOSED to differ; that difference is exactly the root
    // cause, not itself a bug to resolve. If this ever stops holding, the
    // fabricated chain above no longer reaches the boundary it's meant to,
    // and the real regression check below would be testing nothing.
    uint32_t rawParentVersion = um.ComputeBlockVersion(pindexParent);
    uint32_t rawActiveVersion = um.ComputeBlockVersion(pindexActive);
    bool rawParentSetsIt = (rawParentVersion & (uint32_t(1) << TEST_BIT)) != 0;
    bool rawActiveExpectsIt = (rawActiveVersion & (uint32_t(1) << TEST_BIT)) != 0;
    BOOST_REQUIRE_MESSAGE(rawParentSetsIt && !rawActiveExpectsIt,
        "test setup did not reach a genuine LockedIn->Active boundary -- "
        "raw ComputeBlockVersion(parent) vs (active child) must disagree "
        "here, or this test isn't exercising the scenario it claims to");

    // The actual regression guard, tied to miner.cpp's real fix: what
    // miner.cpp now calls (ComputeNextBlockVersion(pindexParent,
    // activeHeight), querying State() only on the real, persistent parent)
    // must agree with validation.cpp's own real expectation for that same
    // future block. Before the fix, miner.cpp used rawParentVersion above
    // and this assertion would have failed the same way "Warning: unknown
    // new rules activated" fired on the real devnet.
    uint32_t fixedMinerWouldSet = um.ComputeNextBlockVersion(pindexParent, activeHeight);
    BOOST_CHECK_EQUAL(fixedMinerWouldSet, rawActiveVersion);
}

BOOST_AUTO_TEST_SUITE_END()
