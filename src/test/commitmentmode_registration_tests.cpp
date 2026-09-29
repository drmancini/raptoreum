// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.6.3 (F-214): proves the chainparams.cpp registration this sub-step adds
// actually has the two properties it claims:
//
//  1. EUpdate::COMMITMENT_MODE is registered ONLY on devnet and regtest --
//     UpdateManager::GetUpdate returns nullptr for mainnet and testnet's own
//     CChainParams, and a real Update for devnet/regtest's.
//
//  2. On the two networks where it IS registered, the registration cannot
//     silently activate: startHeight (100,000,000) is chosen so that
//     UpdateManager::State's own roundNumber<-1 early return (update/
//     update.cpp) reports EUpdateState::Defined -- never Voting, LockedIn,
//     or Active -- for any block at a realistic test-chain height, and
//     IsActive() is false. This is checked directly against a
//     realistically-tall (but still far short of startHeight) fabricated
//     chain tip, not merely asserted from the constant.
//
// Mirrors this codebase's own established pattern for constructing
// CChainParams instances directly in a unit test (test/pow_tests.cpp's
// CreateChainParams(CBaseChainParams::MAIN)/CreateChainParams(DEVNET) calls)
// rather than going through the global SelectParams()/Params() singleton,
// so all four networks can be compared in one test binary without
// reinitializing global state between them.

#include <chain.h>
#include <chainparams.h>
#include <test/test_raptoreum.h>
#include <update/update.h>

#include <boost/test/unit_test.hpp>

namespace commitmentmode_registration_test_helpers {

// A single fabricated CBlockIndex at a given height, no pprev chain needed:
// UpdateManager::State's roundNumber<-1 early return (update/update.cpp)
// fires from blockIndex->nHeight alone, before any GetAncestor() walk, so a
// bare-height index is sufficient to exercise it -- confirmed by reading
// the State() function itself, not assumed.
CBlockIndex MakeIndexAtHeight(int nHeight) {
    CBlockIndex idx;
    idx.nHeight = nHeight;
    return idx;
}

}  // namespace commitmentmode_registration_test_helpers

using namespace commitmentmode_registration_test_helpers;

BOOST_FIXTURE_TEST_SUITE(commitmentmode_registration_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(commitment_mode_unregistered_on_mainnet_and_testnet) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testParams = CreateChainParams(CBaseChainParams::TESTNET);

    BOOST_CHECK(mainParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE) == nullptr);
    BOOST_CHECK(testParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE) == nullptr);

    // With no Update registered, State() returns Unknown (update/update.cpp,
    // UpdateManager::State's own `if (!update ...) return {Unknown, ...}`
    // guard) and IsActive() is false at any height, including a
    // realistic-looking mainnet height.
    CBlockIndex mainTip = MakeIndexAtHeight(1500000);
    CBlockIndex testTip = MakeIndexAtHeight(1500000);
    BOOST_CHECK(!mainParams->Updates().IsActive(EUpdate::COMMITMENT_MODE, &mainTip));
    BOOST_CHECK(!testParams->Updates().IsActive(EUpdate::COMMITMENT_MODE, &testTip));
}

BOOST_AUTO_TEST_CASE(commitment_mode_registered_on_devnet_and_regtest) {
    const auto devParams = CreateChainParams(CBaseChainParams::DEVNET);
    const auto regParams = CreateChainParams(CBaseChainParams::REGTEST);

    const Update *devUpdate = devParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE);
    const Update *regUpdate = regParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE);
    BOOST_REQUIRE(devUpdate != nullptr);
    BOOST_REQUIRE(regUpdate != nullptr);

    // F-213 point 5's parameter set: reused bit, ROUND_VOTING-shaped
    // thresholds, forcedUpdate=false.
    BOOST_CHECK_EQUAL(devUpdate->Bit(), 3);
    BOOST_CHECK_EQUAL(regUpdate->Bit(), 3);
    BOOST_CHECK(!devUpdate->ForcedUpdate());
    BOOST_CHECK(!regUpdate->ForcedUpdate());
    BOOST_CHECK_EQUAL(devUpdate->MinerThreshold().ThresholdStart(), 85);
    BOOST_CHECK_EQUAL(devUpdate->MinerThreshold().ThresholdMin(), 85);
    BOOST_CHECK_EQUAL(devUpdate->NodeThreshold().ThresholdStart(), 0);
    BOOST_CHECK_EQUAL(devUpdate->NodeThreshold().ThresholdMin(), 0);
    BOOST_CHECK_EQUAL(regUpdate->MinerThreshold().ThresholdStart(), 85);
    BOOST_CHECK_EQUAL(regUpdate->NodeThreshold().ThresholdStart(), 0);
}

BOOST_AUTO_TEST_CASE(commitment_mode_start_height_is_structurally_unreachable_on_regtest) {
    const auto regParams = CreateChainParams(CBaseChainParams::REGTEST);

    const Update *regUpdate = regParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE);
    BOOST_REQUIRE(regUpdate != nullptr);

    // A height no real regtest test chain has ever reached (10,000 is already
    // generous -- TestChain100Setup-based fixtures top out around a few
    // hundred blocks) is still tens of millions of rounds short of
    // startHeight. At mainnet's real 2-minute spacing (consensus.
    // nPowTargetSpacing, chainparams.cpp's own CMainParams), the remaining
    // distance is centuries, not blocks a test -- or a live network -- could
    // ever plausibly reach by accident. Regtest keeps F-214's original
    // unreachable placeholder; only devnet was moved to a real height
    // (F-228), see commitment_mode_start_height_is_reachable_on_devnet below.
    const int64_t tallTestHeight = 10000;
    const int64_t blocksRemaining = regUpdate->StartHeight() - tallTestHeight;
    BOOST_REQUIRE_GT(blocksRemaining, 10000000); // tens of millions of blocks away
    const int64_t yearsAtMainnetSpacing = (blocksRemaining * 2) / (60 * 24 * 365); // 2 min/block
    BOOST_CHECK_GT(yearsAtMainnetSpacing, 100); // absurd on any realistic timescale

    CBlockIndex regTip = MakeIndexAtHeight(tallTestHeight);
    BOOST_CHECK(!regParams->Updates().IsActive(EUpdate::COMMITMENT_MODE, &regTip));

    StateInfo regState = regParams->Updates().State(EUpdate::COMMITMENT_MODE, &regTip);
    BOOST_CHECK(regState.State == EUpdateState::Defined);
}

BOOST_AUTO_TEST_CASE(commitment_mode_start_height_is_reachable_on_devnet) {
    // F-228: devnet's startHeight moved from F-214's unreachable placeholder
    // to 100, mirroring ROUND_VOTING's own real, working devnet activation
    // height directly above it in chainparams.cpp -- devnet carries no real
    // value and needs none of F-213 point 4's mainnet coverage precondition,
    // so a low, genuinely reachable height is a safe, deliberate choice for
    // exercising the mechanism end to end on a live network.
    const auto devParams = CreateChainParams(CBaseChainParams::DEVNET);
    const Update *devUpdate = devParams->Updates().GetUpdate(EUpdate::COMMITMENT_MODE);
    BOOST_REQUIRE(devUpdate != nullptr);
    BOOST_CHECK_EQUAL(devUpdate->StartHeight(), 100);

    // Before startHeight, still Defined/inactive -- same shape as before the
    // height was reachable, just close instead of astronomically far.
    CBlockIndex devTipBefore = MakeIndexAtHeight(50);
    BOOST_CHECK(!devParams->Updates().IsActive(EUpdate::COMMITMENT_MODE, &devTipBefore));
    StateInfo devStateBefore = devParams->Updates().State(EUpdate::COMMITMENT_MODE, &devTipBefore);
    BOOST_CHECK(devStateBefore.State == EUpdateState::Defined);
}

BOOST_AUTO_TEST_SUITE_END()
