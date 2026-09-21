// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 3.1 (build-plan.md): DIP8's "signing attempts" process. TrySignChainTip's
// original one-shot-per-height guard permanently blocked ChainLock
// convergence once ANY attempt was made at a height, even if it never
// produced a recovered signature -- exactly the condition transaction-
// decoupling.md's §3A.7 makes attacker-schedulable under decoupling
// (identifiers arrive instantly; body-release timing is the attacker's
// choice, so a quorum split can be manufactured on demand rather than raced
// for). These tests exercise the pure decision logic
// (quorums_chainlocks.h) that replaces the single check -- driving the real
// CChainLocksHandler needs a live smartnode, a BLS quorum and a signed
// recovered signature, which this tree's own precedent
// (acceptancebit_tests.cpp's a_commitment_only_block_still_heals_after_
// its_sibling_is_marked_conflicting comment) already declined as
// disproportionate scaffolding -- so the actual retry/finalization decision
// is extracted as free functions, on ShouldNegotiateCommitments's own
// pattern (protocol.h), specifically so it can be tested without any of
// that.

#include <llmq/quorums_chainlocks.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;

BOOST_AUTO_TEST_SUITE(chainlocks_tests)

// --- GetChainLockAttemptNumber -----------------------------------------
//
// Every quorum member must compute the SAME attempt number for their
// signature shares to combine into a threshold signature -- these tests pin
// the time-slot arithmetic exactly, since an off-by-one here would silently
// desync the quorum instead of raising an error anywhere.

BOOST_AUTO_TEST_CASE(attempt_number_is_stable_within_one_interval) {
    BOOST_CHECK_EQUAL(GetChainLockAttemptNumber(0), GetChainLockAttemptNumber(CLSIG_ATTEMPT_INTERVAL - 1));
}

BOOST_AUTO_TEST_CASE(attempt_number_advances_at_the_interval_boundary) {
    int32_t before = GetChainLockAttemptNumber(CLSIG_ATTEMPT_INTERVAL - 1);
    int32_t after = GetChainLockAttemptNumber(CLSIG_ATTEMPT_INTERVAL);
    BOOST_CHECK_EQUAL(after, before + 1);
}

BOOST_AUTO_TEST_CASE(attempt_number_advances_again_a_full_interval_later) {
    int32_t first = GetChainLockAttemptNumber(1000 * CLSIG_ATTEMPT_INTERVAL);
    int32_t second = GetChainLockAttemptNumber(1001 * CLSIG_ATTEMPT_INTERVAL);
    BOOST_CHECK_EQUAL(second, first + 1);
}

// --- GetChainLockAttemptRequestId ---------------------------------------

BOOST_AUTO_TEST_CASE(attempt_request_id_is_deterministic) {
    uint256 a = GetChainLockAttemptRequestId(500, 3);
    uint256 b = GetChainLockAttemptRequestId(500, 3);
    BOOST_CHECK(a == b);
}

BOOST_AUTO_TEST_CASE(attempt_request_id_differs_by_height) {
    uint256 a = GetChainLockAttemptRequestId(500, 3);
    uint256 b = GetChainLockAttemptRequestId(501, 3);
    BOOST_CHECK(a != b);
}

BOOST_AUTO_TEST_CASE(attempt_request_id_differs_by_attempt_number) {
    uint256 a = GetChainLockAttemptRequestId(500, 3);
    uint256 b = GetChainLockAttemptRequestId(500, 4);
    BOOST_CHECK(a != b);
}

// The entire reason two prefixes exist (quorums_chainlocks.h's own doc
// comment on CLSIG_ATTEMPT_REQUESTID_PREFIX): CChainLockSig's wire format
// carries no attempt number, and ProcessNewChainLock's verification
// recomputes a request id from height alone -- an attempt's own recovered
// signature must never collide with the finalization round's request id, at
// ANY attempt number, or a peer could be tricked into treating an
// unconverged attempt as the final, broadcastable CLSIG.
BOOST_AUTO_TEST_CASE(attempt_request_id_never_collides_with_the_finalization_request_id) {
    uint256 finalizationId = ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, 500));
    for (int32_t attempt = 0; attempt < 5; attempt++) {
        BOOST_CHECK(GetChainLockAttemptRequestId(500, attempt) != finalizationId);
    }
}

// --- DecideChainLockSignAction -------------------------------------------

BOOST_AUTO_TEST_CASE(starts_first_attempt_from_the_initial_state) {
    // The handler's own real defaults: bestChainLock height -1, lastSignedHeight
    // -1, lastSignedAttempt -1, not finalizing.
    BOOST_CHECK(DecideChainLockSignAction(/*nTipHeight=*/100, /*nAttemptNum=*/7,
                                          /*nBestChainLockHeight=*/-1,
                                          /*nLastSignedHeight=*/-1, /*nLastSignedAttempt=*/-1,
                                          /*fLastSignedIsFinalization=*/false)
                == ChainLockSignAction::kStartAttempt);
}

BOOST_AUTO_TEST_CASE(does_nothing_once_a_chainlock_is_at_least_as_good) {
    BOOST_CHECK(DecideChainLockSignAction(100, 7, /*nBestChainLockHeight=*/100, -1, -1, false)
                == ChainLockSignAction::kNone);
    BOOST_CHECK(DecideChainLockSignAction(100, 7, /*nBestChainLockHeight=*/101, -1, -1, false)
                == ChainLockSignAction::kNone);
}

BOOST_AUTO_TEST_CASE(does_not_restart_the_same_attempt_slot_at_the_same_height) {
    BOOST_CHECK(DecideChainLockSignAction(100, 7, -1, /*nLastSignedHeight=*/100, /*nLastSignedAttempt=*/7, false)
                == ChainLockSignAction::kNone);
}

// The actual fix: the original code's `pindex->nHeight == lastSignedHeight`
// check alone would have returned kNone here forever, regardless of attempt
// number, once any attempt had ever been made at this height. A quorum
// split manufactured by withholding bodies (§3A.7) would then never
// converge even after propagation naturally caught up everyone's local
// tips -- this is the exact bug F-136 exists to close.
BOOST_AUTO_TEST_CASE(starts_a_new_attempt_when_the_slot_advances_at_the_same_height) {
    BOOST_CHECK(DecideChainLockSignAction(100, /*nAttemptNum=*/8, -1, /*nLastSignedHeight=*/100,
                                          /*nLastSignedAttempt=*/7, false)
                == ChainLockSignAction::kStartAttempt);
}

BOOST_AUTO_TEST_CASE(starts_a_new_attempt_when_the_tip_height_advances) {
    BOOST_CHECK(DecideChainLockSignAction(/*nTipHeight=*/101, /*nAttemptNum=*/7, -1,
                                          /*nLastSignedHeight=*/100, /*nLastSignedAttempt=*/7, false)
                == ChainLockSignAction::kStartAttempt);
}

// Guards the narrow race identified while designing this: if the attempt
// slot rolls over WHILE a finalization round is still outstanding at this
// height, a naive re-check of just (height, attemptNum) would restart a
// fresh attempt and abandon the in-flight finalization -- silently losing a
// CLSIG that might have converged moments later, since HandleNewRecoveredSig
// only recognises a recovered signature matching the CURRENT
// lastSignedRequestId.
BOOST_AUTO_TEST_CASE(never_restarts_an_attempt_while_finalization_is_outstanding) {
    BOOST_CHECK(DecideChainLockSignAction(100, /*nAttemptNum=*/99, -1, /*nLastSignedHeight=*/100,
                                          /*nLastSignedAttempt=*/7, /*fLastSignedIsFinalization=*/true)
                == ChainLockSignAction::kNone);
}

// --- DecideRecoveredSigOutcome --------------------------------------------

BOOST_AUTO_TEST_CASE(ignores_a_recovered_sig_that_does_not_match_what_we_signed) {
    uint256 signedId = GetChainLockAttemptRequestId(100, 7);
    uint256 signedMsgHash = uint256S("0x01");
    uint256 otherId = GetChainLockAttemptRequestId(100, 8);

    BOOST_CHECK(DecideRecoveredSigOutcome(otherId, signedMsgHash, signedId, signedMsgHash,
                                          /*nBestChainLockHeight=*/-1, /*nLastSignedHeight=*/100, false)
                == RecoveredSigOutcome::kIgnore);
    BOOST_CHECK(DecideRecoveredSigOutcome(signedId, uint256S("0x02"), signedId, signedMsgHash,
                                          -1, 100, false)
                == RecoveredSigOutcome::kIgnore);
}

BOOST_AUTO_TEST_CASE(ignores_a_recovered_sig_once_a_better_chainlock_already_exists) {
    uint256 signedId = GetChainLockAttemptRequestId(100, 7);
    uint256 signedMsgHash = uint256S("0x01");

    BOOST_CHECK(DecideRecoveredSigOutcome(signedId, signedMsgHash, signedId, signedMsgHash,
                                          /*nBestChainLockHeight=*/100, /*nLastSignedHeight=*/100, false)
                == RecoveredSigOutcome::kIgnore);
}

BOOST_AUTO_TEST_CASE(starts_finalization_when_an_attempt_converges) {
    uint256 signedId = GetChainLockAttemptRequestId(100, 7);
    uint256 signedMsgHash = uint256S("0x01");

    BOOST_CHECK(DecideRecoveredSigOutcome(signedId, signedMsgHash, signedId, signedMsgHash,
                                          -1, 100, /*fLastSignedIsFinalization=*/false)
                == RecoveredSigOutcome::kStartFinalization);
}

BOOST_AUTO_TEST_CASE(builds_the_chainlock_when_finalization_converges) {
    uint256 finalizationId = ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, 100));
    uint256 signedMsgHash = uint256S("0x01");

    BOOST_CHECK(DecideRecoveredSigOutcome(finalizationId, signedMsgHash, finalizationId, signedMsgHash,
                                          -1, 100, /*fLastSignedIsFinalization=*/true)
                == RecoveredSigOutcome::kBuildChainLock);
}

BOOST_AUTO_TEST_SUITE_END()
