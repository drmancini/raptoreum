// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.6.2 (build-plan.md's 4.6 row, docs/findings.md's F-205 thread 2): the
// real, height-gated trigger for commitment-form P2P negotiation. See
// commitments_negotiation.h for the design and why IsCommitmentFormatActive
// is a placeholder pending 4.6.1's own real EUpdate bit.

#include <chain.h>
#include <commitments_negotiation.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(commitments_negotiation_tests, BasicTestingSetup)

// Tripwire: pins today's placeholder behaviour explicitly, so 4.6.1 landing
// the real bit forces a deliberate look at this test rather than a silent
// pass either way. False is the safe default for anything consensus-
// adjacent whose activation mechanism hasn't shipped yet.
BOOST_AUTO_TEST_CASE(commitment_format_is_never_active_before_4_6_1_lands) {
    BOOST_CHECK(!IsCommitmentFormatActive(nullptr));

    CBlockIndex indexDummy;
    indexDummy.nHeight = 5000000; // arbitrarily far in the future -- height must not matter yet
    BOOST_CHECK(!IsCommitmentFormatActive(&indexDummy));
}

// The actual trigger-swap logic: exhaustive over all four (real activation x
// manual override) combinations, fully mutation-testable today even though
// fRealActivation can't yet come from a genuine query (see
// commitments_negotiation.h's own doc comment on why this is split out from
// ShouldNegotiateCommitmentsNow).
BOOST_AUTO_TEST_CASE(combine_negotiation_trigger_is_an_or) {
    BOOST_CHECK(CombineNegotiationTrigger(/*fRealActivation=*/false, /*fManualOverride=*/false) == false);
    BOOST_CHECK(CombineNegotiationTrigger(/*fRealActivation=*/true, /*fManualOverride=*/false) == true);
    BOOST_CHECK(CombineNegotiationTrigger(/*fRealActivation=*/false, /*fManualOverride=*/true) == true);
    BOOST_CHECK(CombineNegotiationTrigger(/*fRealActivation=*/true, /*fManualOverride=*/true) == true);
}

// End-to-end wiring: today, with no real bit, this is equivalent to the
// manual flag alone (IsCommitmentFormatActive is hardcoded false) -- pinned
// explicitly so the flag-driven call sites (init.cpp's NODE_COMMITMENTS
// advertisement, net_processing.cpp's GETBODYRANGE fetch gates) keep working
// exactly as they did before this sub-step, for every existing test that
// still drives them via -commitmentblocks/-fetchbodyrange.
BOOST_AUTO_TEST_CASE(should_negotiate_now_matches_the_manual_flag_pending_4_6_1) {
    BOOST_CHECK(ShouldNegotiateCommitmentsNow(nullptr, /*fManualOverride=*/true) == true);
    BOOST_CHECK(ShouldNegotiateCommitmentsNow(nullptr, /*fManualOverride=*/false) == false);

    CBlockIndex indexDummy;
    indexDummy.nHeight = 5000000;
    BOOST_CHECK(ShouldNegotiateCommitmentsNow(&indexDummy, /*fManualOverride=*/true) == true);
    BOOST_CHECK(ShouldNegotiateCommitmentsNow(&indexDummy, /*fManualOverride=*/false) == false);
}

BOOST_AUTO_TEST_SUITE_END()
