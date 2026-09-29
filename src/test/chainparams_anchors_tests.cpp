// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 3.6 (F-222): proves chainparams.cpp's mainnet defaultAssumeValid/
// nMinimumChainWork actually carry the values this fix intends, at the
// CChainParams level -- not just that uint256S("ox...") parses to zero in
// the abstract (uint256_tests.cpp covers that mechanism directly). Before
// the fix, defaultAssumeValid's "ox" typo made this exact test's first
// check fail (mainnet's defaultAssumeValid was the null hash); genuine RED
// confirmed by reverting src/chainparams.cpp alone and rebuilding before
// restoring it.
//
// Mirrors this codebase's own established pattern (test/pow_tests.cpp,
// test/commitmentmode_registration_tests.cpp) of constructing CChainParams
// directly via CreateChainParams() rather than the global SelectParams()/
// Params() singleton.

#include <chainparams.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(chainparams_anchors_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_default_assume_valid_is_not_the_zero_hash) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);

    // This is the bug the row exists to close: pre-fix, the "ox" typo made
    // SetHex silently produce the zero hash, and defaultAssumeValid.IsNull()
    // being true here means the whole feature (init.cpp's -assumevalid
    // default, validation.cpp's script-check skip) was structurally
    // switched off on mainnet with no error, warning, or test failure.
    BOOST_CHECK(!mainParams->GetConsensus().defaultAssumeValid.IsNull());
}

BOOST_AUTO_TEST_CASE(mainnet_default_assume_valid_matches_the_live_verified_block) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);

    // Block 1,340,136 on real mainnet (raptoreum-cli getblockhash 1340136 /
    // getblock, cross-checked against getblockheader, on a fully-synced
    // node), chosen as tip-100,000 at audit time (2026-09-29).
    const uint256 expected = uint256S("0x8ef694133cd8981a14f4042d256cdce823cb5686dadfcda5ad843b97090cfccf");
    BOOST_CHECK_EQUAL(mainParams->GetConsensus().defaultAssumeValid.GetHex(), expected.GetHex());
}

BOOST_AUTO_TEST_CASE(mainnet_minimum_chain_work_matches_the_live_verified_block) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);

    // Same block's real cumulative chainwork (raptoreum-cli getblock's
    // "chainwork" field), so the two constants stay pointed at the same
    // real anchor rather than drifting apart.
    const uint256 expected = uint256S("00000000000000000000000000000000000000000000000000198d0e6ffa12df");
    BOOST_CHECK_EQUAL(mainParams->GetConsensus().nMinimumChainWork.GetHex(), expected.GetHex());
}

BOOST_AUTO_TEST_CASE(mainnet_minimum_chain_work_exceeds_the_stale_anchor_it_replaced) {
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);

    // The old, stale anchor's chainwork (block 421457, real mainnet value --
    // this was correct and un-typo'd; only its staleness was the problem).
    // Guards against a future accidental downgrade past the constant's own
    // point.
    const arith_uint256 oldWork = UintToArith256(uint256S(
            "000000000000000000000000000000000000000000000000000eead474ccbc59"));
    BOOST_CHECK(UintToArith256(mainParams->GetConsensus().nMinimumChainWork) > oldWork);
}

BOOST_AUTO_TEST_CASE(testnet_devnet_regtest_anchors_are_untouched_by_this_fix) {
    // This fix is scoped to mainnet only (3.6's own row, and F-222). The
    // other three networks' anchors are deliberately-disabled placeholders
    // (uint256S("0x0")-family values) unrelated to the mainnet typo, and
    // must stay exactly as they were.
    const auto testParams = CreateChainParams(CBaseChainParams::TESTNET);
    const auto devParams = CreateChainParams(CBaseChainParams::DEVNET);
    const auto regParams = CreateChainParams(CBaseChainParams::REGTEST);

    BOOST_CHECK(testParams->GetConsensus().defaultAssumeValid.IsNull());
    BOOST_CHECK(devParams->GetConsensus().defaultAssumeValid.IsNull());
    BOOST_CHECK(regParams->GetConsensus().defaultAssumeValid.IsNull());
    BOOST_CHECK(testParams->GetConsensus().nMinimumChainWork.IsNull());
    BOOST_CHECK(devParams->GetConsensus().nMinimumChainWork.IsNull());
    BOOST_CHECK(regParams->GetConsensus().nMinimumChainWork.IsNull());
}

BOOST_AUTO_TEST_CASE(mainnet_checkpoint_394273_is_unchanged_and_still_the_last_entry) {
    // 3.6/F-222 deliberately left the checkpoint map untouched (see the
    // in-source comment above checkpointData in chainparams.cpp for why).
    // This pins that "left alone" decision: still 4 entries, still topping
    // out at 394273, with the real, live-verified hash -- so a future change
    // to this map is a deliberate edit, not silent drift.
    const auto mainParams = CreateChainParams(CBaseChainParams::MAIN);
    const MapCheckpoints &checkpoints = mainParams->Checkpoints().mapCheckpoints;

    BOOST_CHECK_EQUAL(checkpoints.size(), 4u);
    const int topHeight = checkpoints.rbegin()->first;
    BOOST_CHECK_EQUAL(topHeight, 394273);

    const uint256 expected = uint256S("0x0dc274a28864a01a9539e60afdbc38fcdb0f000fbc52553cd31651c97557dc04");
    BOOST_CHECK_EQUAL(checkpoints.rbegin()->second.GetHex(), expected.GetHex());
}

BOOST_AUTO_TEST_SUITE_END()
