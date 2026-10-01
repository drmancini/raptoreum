// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// MEDIUM-3 (Fable review, 2026-10-01, 5.4.4.2 round): evo/attestationbatch.h's
// own pure functions -- ComputeCanonicalBatchRoot and BuildAttestationBatchProof
// -- had NO direct tests at all; only ever exercised indirectly through
// CAttestationBatchHandler::GetAttestation (quorums_attestationbatch.cpp),
// and even that indirect path lost its own only reachable "found" case once
// HIGH-1's quorum-binding check made every recovered-signature acceptance
// path in that class's own test suite unreachable without a live quorum
// (see test/quorums_attestationbatch_tests.cpp's own comments). These two
// functions are pure (no chain, no quorum, no class state) and deserve
// direct coverage on their own terms regardless. BasicTestingSetup, not
// TestingSetup -- neither function touches the chain, so a lighter fixture
// matches what is actually under test (the same "don't couple to more
// environment than the code needs" discipline attestedtx_tests.cpp's own
// check_sigs=false already follows for a different reason).

#include <evo/attestationbatch.h>

#include <consensus/merkle.h>
#include <random.h>
#include <test/test_raptoreum.h>

#include <algorithm>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(attestationbatch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(compute_canonical_batch_root_sorts_before_hashing) {
    uint256 a = InsecureRand256(), b = InsecureRand256(), c = InsecureRand256();
    uint256 rootForward = ComputeCanonicalBatchRoot({a, b, c});
    uint256 rootReversed = ComputeCanonicalBatchRoot({c, b, a});
    BOOST_CHECK_EQUAL(rootForward.ToString(), rootReversed.ToString());

    std::vector <uint256> sorted = {a, b, c};
    std::sort(sorted.begin(), sorted.end());
    BOOST_CHECK_EQUAL(rootForward.ToString(), ComputeMerkleRoot(sorted).ToString());
}

BOOST_AUTO_TEST_CASE(compute_canonical_batch_root_of_empty_is_null) {
    // CAttestationBatchHandler::TrySignBatch (llmq/quorums_attestationbatch.cpp)
    // relies on exactly this -- its own comment there explains why its
    // empty-queue check is about avoiding a wasted AsyncSignIfMember call,
    // not this return value, which is already null either way.
    BOOST_CHECK(ComputeCanonicalBatchRoot({}).IsNull());
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_extracts_the_right_leaf) {
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256(), InsecureRand256(),
                                    InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 expectedRoot = ComputeMerkleRoot(sorted);

    for (const uint256 &leaf: sorted) {
        CPartialMerkleTree proof = BuildAttestationBatchProof(sorted, leaf);
        std::vector <uint256> matched;
        std::vector<unsigned int> indices;
        uint256 extractedRoot = proof.ExtractMatches(matched, indices);
        BOOST_CHECK_EQUAL(extractedRoot.ToString(), expectedRoot.ToString());
        BOOST_REQUIRE_EQUAL(matched.size(), 1U);
        BOOST_CHECK_EQUAL(matched[0].ToString(), leaf.ToString());
    }
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_returns_an_empty_tree_if_the_leaf_is_absent) {
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 absentLeaf = InsecureRand256(); // not one of the three above, overwhelmingly

    CPartialMerkleTree proof = BuildAttestationBatchProof(sorted, absentLeaf);
    std::vector <uint256> matched;
    std::vector<unsigned int> indices;
    proof.ExtractMatches(matched, indices);
    BOOST_CHECK(matched.empty());
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_requires_the_shared_sort_order) {
    // The precondition BuildAttestationBatchProof's own doc comment
    // (evo/attestationbatch.h) states: `sortedLeaves` must already be
    // sorted the identical way ComputeCanonicalBatchRoot sorts internally.
    // Proven here, not assumed: building a proof against a list that is
    // NOT sorted that way extracts a DIFFERENT root than the canonical
    // one, confirming a caller that skips the shared sort step is not
    // merely stylistically wrong but builds a proof against the wrong
    // tree entirely.
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256(), InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 myLeaf = sorted[2];
    uint256 correctRoot = ComputeMerkleRoot(sorted);

    // First and last elements swapped -- guaranteed to differ from the
    // canonical sort order (random 256-bit values are never equal in
    // practice), not left to chance the way building an "unsorted" list
    // from scratch and hoping it is not accidentally already sorted
    // would be.
    std::vector <uint256> unsorted = sorted;
    std::swap(unsorted.front(), unsorted.back());

    CPartialMerkleTree wrongProof = BuildAttestationBatchProof(unsorted, myLeaf);
    std::vector <uint256> matched;
    std::vector<unsigned int> indices;
    uint256 wrongRoot = wrongProof.ExtractMatches(matched, indices);
    BOOST_CHECK_NE(wrongRoot.ToString(), correctRoot.ToString());
}

BOOST_AUTO_TEST_SUITE_END()
