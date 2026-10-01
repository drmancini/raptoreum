// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// MEDIUM-3 (Fable review, 2026-10-01, 5.4.4.2 round): evo/attestationbatch.h's
// own pure functions had NO direct tests at all; only ever exercised
// indirectly through CAttestationBatchHandler::GetAttestation
// (quorums_attestationbatch.cpp). These functions are pure (no chain, no
// quorum, no class state) and deserve direct coverage on their own terms.
// BasicTestingSetup, not TestingSetup -- neither function touches the
// chain, so a lighter fixture matches what is actually under test.
//
// 5.4.4.1-OPEN item (b) (F-247): BuildAttestationBatchProof/
// ExtractAttestationBatchRoot replaced a CPartialMerkleTree-backed proof
// with a hand-rolled sibling-hash branch, mirroring ComputeMerkleRoot's
// own pairing/odd-duplication algorithm (consensus/merkle.cpp) exactly.
// That mirroring is the one thing most worth getting wrong here, so both
// parities (even/odd leaf counts) and the single-leaf (zero-sibling)
// edge case each get their own dedicated test, not just one generic loop.

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

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_of_a_single_leaf_batch_has_no_siblings) {
    // The root IS the leaf (ComputeMerkleRoot's own single-element case,
    // consensus/merkle.cpp: the while(size>1) loop never runs) -- a proof
    // needs zero sibling hashes to prove it.
    uint256 leaf = InsecureRand256();
    CAttestationBatchProof proof = BuildAttestationBatchProof({leaf}, leaf);
    BOOST_CHECK(proof.siblingHashes.empty());
    BOOST_CHECK_EQUAL(proof.leafIndex, 0U);
    BOOST_CHECK_EQUAL(ExtractAttestationBatchRoot(leaf, proof).ToString(), leaf.ToString());
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_extracts_the_right_leaf_with_an_even_leaf_count) {
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256(), InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 expectedRoot = ComputeMerkleRoot(sorted);

    for (const uint256 &leaf: sorted) {
        CAttestationBatchProof proof = BuildAttestationBatchProof(sorted, leaf);
        BOOST_CHECK_EQUAL(ExtractAttestationBatchRoot(leaf, proof).ToString(), expectedRoot.ToString());
    }
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_extracts_the_right_leaf_with_an_odd_leaf_count) {
    // The odd-count path specifically: ComputeMerkleRoot duplicates the
    // last element before pairing at any level whose own count is odd
    // (consensus/merkle.cpp, the CVE-2012-2459 shape) -- BuildAttestationBatchProof
    // must mirror that duplication exactly, or a proof for the
    // self-paired node would recompute the wrong root. 5 leaves forces an
    // odd count at the FIRST level (5 -> pad to 6) and again one level up
    // (3 -> pad to 4), exercising the duplication twice over, at two
    // different tree heights, not just once.
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256(), InsecureRand256(),
                                    InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 expectedRoot = ComputeMerkleRoot(sorted);

    for (const uint256 &leaf: sorted) {
        CAttestationBatchProof proof = BuildAttestationBatchProof(sorted, leaf);
        BOOST_CHECK_EQUAL(ExtractAttestationBatchRoot(leaf, proof).ToString(), expectedRoot.ToString());
    }
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_matches_compute_canonical_batch_root) {
    // Ties BuildAttestationBatchProof/ExtractAttestationBatchRoot directly
    // to the function real callers actually compute the signed root with
    // (CAttestationBatchHandler::TrySignBatch), not just to ComputeMerkleRoot
    // one level down -- a cheap way to catch a sort-order mismatch between
    // the two call sites that per-leaf tests against ComputeMerkleRoot
    // directly would not.
    std::vector <uint256> leaves = {InsecureRand256(), InsecureRand256(), InsecureRand256()};
    uint256 expectedRoot = ComputeCanonicalBatchRoot(leaves);

    std::vector <uint256> sorted = leaves;
    std::sort(sorted.begin(), sorted.end());
    for (const uint256 &leaf: sorted) {
        CAttestationBatchProof proof = BuildAttestationBatchProof(sorted, leaf);
        BOOST_CHECK_EQUAL(ExtractAttestationBatchRoot(leaf, proof).ToString(), expectedRoot.ToString());
    }
}

BOOST_AUTO_TEST_CASE(build_attestation_batch_proof_returns_a_default_proof_if_the_leaf_is_absent) {
    std::vector <uint256> sorted = {InsecureRand256(), InsecureRand256(), InsecureRand256()};
    std::sort(sorted.begin(), sorted.end());
    uint256 absentLeaf = InsecureRand256(); // not one of the three above, overwhelmingly

    CAttestationBatchProof proof = BuildAttestationBatchProof(sorted, absentLeaf);
    BOOST_CHECK(proof.siblingHashes.empty());
    BOOST_CHECK_EQUAL(proof.leafIndex, 0U);
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

    CAttestationBatchProof wrongProof = BuildAttestationBatchProof(unsorted, myLeaf);
    uint256 wrongRoot = ExtractAttestationBatchRoot(myLeaf, wrongProof);
    BOOST_CHECK_NE(wrongRoot.ToString(), correctRoot.ToString());
}

BOOST_AUTO_TEST_SUITE_END()
