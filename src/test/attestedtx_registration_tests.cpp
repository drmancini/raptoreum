// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 5.4.3 (F-242): proves the chainparams.cpp registration this sub-step adds
// actually has the two properties it claims, mirroring
// commitmentmode_registration_tests.cpp's own proven shape exactly for
// EUpdate::ATTESTED_TX in place of EUpdate::COMMITMENT_MODE:
//
//  1. EUpdate::ATTESTED_TX is registered ONLY on devnet and regtest --
//     UpdateManager::GetUpdate returns nullptr for mainnet and testnet's own
//     CChainParams, and a real Update for devnet/regtest's.
//
//  2. On regtest, the registration cannot silently activate (structurally
//     unreachable startHeight); on devnet it is deliberately reachable
//     (F-228's own precedent for why a low devnet height is safe).

#include <chain.h>
#include <chainparams.h>
#include <test/test_raptoreum.h>
#include <update/update.h>

#include <boost/test/unit_test.hpp>

namespace attestedtx_registration_test_helpers {

CBlockIndex MakeIndexAtHeight(int nHeight) {
    CBlockIndex idx;
    idx.nHeight = nHeight;
    return idx;
}

}  // namespace attestedtx_registration_test_helpers

using namespace attestedtx_registration_test_helpers;

BOOST_FIXTURE_TEST_SUITE(attestedtx_registration_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(attested_tx_unregistered_on_mainnet_and_testnet) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testParams = CreateChainParams(CBaseChainParams::TESTNET);

    BOOST_CHECK(mainParams->Updates().GetUpdate(EUpdate::ATTESTED_TX) == nullptr);
    BOOST_CHECK(testParams->Updates().GetUpdate(EUpdate::ATTESTED_TX) == nullptr);

    CBlockIndex mainTip = MakeIndexAtHeight(1500000);
    CBlockIndex testTip = MakeIndexAtHeight(1500000);
    BOOST_CHECK(!mainParams->Updates().IsActive(EUpdate::ATTESTED_TX, &mainTip));
    BOOST_CHECK(!testParams->Updates().IsActive(EUpdate::ATTESTED_TX, &testTip));
}

BOOST_AUTO_TEST_CASE(attested_tx_registered_on_devnet_and_regtest) {
    const auto devParams = CreateChainParams(CBaseChainParams::DEVNET);
    const auto regParams = CreateChainParams(CBaseChainParams::REGTEST);

    const Update *devUpdate = devParams->Updates().GetUpdate(EUpdate::ATTESTED_TX);
    const Update *regUpdate = regParams->Updates().GetUpdate(EUpdate::ATTESTED_TX);
    BOOST_REQUIRE(devUpdate != nullptr);
    BOOST_REQUIRE(regUpdate != nullptr);

    BOOST_CHECK_EQUAL(devUpdate->Bit(), 4);
    BOOST_CHECK_EQUAL(regUpdate->Bit(), 4);
    BOOST_CHECK(!devUpdate->ForcedUpdate());
    BOOST_CHECK(!regUpdate->ForcedUpdate());
    BOOST_CHECK_EQUAL(devUpdate->MinerThreshold().ThresholdStart(), 85);
    BOOST_CHECK_EQUAL(devUpdate->NodeThreshold().ThresholdStart(), 0);
    BOOST_CHECK_EQUAL(regUpdate->MinerThreshold().ThresholdStart(), 85);
    BOOST_CHECK_EQUAL(regUpdate->NodeThreshold().ThresholdStart(), 0);
}

BOOST_AUTO_TEST_CASE(attested_tx_start_height_is_structurally_unreachable_on_regtest) {
    const auto regParams = CreateChainParams(CBaseChainParams::REGTEST);

    const Update *regUpdate = regParams->Updates().GetUpdate(EUpdate::ATTESTED_TX);
    BOOST_REQUIRE(regUpdate != nullptr);

    const int64_t tallTestHeight = 10000;
    const int64_t blocksRemaining = regUpdate->StartHeight() - tallTestHeight;
    BOOST_REQUIRE_GT(blocksRemaining, 10000000);

    CBlockIndex regTip = MakeIndexAtHeight(tallTestHeight);
    BOOST_CHECK(!regParams->Updates().IsActive(EUpdate::ATTESTED_TX, &regTip));

    StateInfo regState = regParams->Updates().State(EUpdate::ATTESTED_TX, &regTip);
    BOOST_CHECK(regState.State == EUpdateState::Defined);
}

BOOST_AUTO_TEST_CASE(attested_tx_start_height_is_reachable_on_devnet) {
    const auto devParams = CreateChainParams(CBaseChainParams::DEVNET);
    const Update *devUpdate = devParams->Updates().GetUpdate(EUpdate::ATTESTED_TX);
    BOOST_REQUIRE(devUpdate != nullptr);
    BOOST_CHECK_EQUAL(devUpdate->StartHeight(), 100);

    CBlockIndex devTipBefore = MakeIndexAtHeight(50);
    BOOST_CHECK(!devParams->Updates().IsActive(EUpdate::ATTESTED_TX, &devTipBefore));
    StateInfo devStateBefore = devParams->Updates().State(EUpdate::ATTESTED_TX, &devTipBefore);
    BOOST_CHECK(devStateBefore.State == EUpdateState::Defined);
}

BOOST_AUTO_TEST_SUITE_END()
