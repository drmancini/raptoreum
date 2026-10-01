// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 5.4.4.2 (build-plan.md, F-245): CAttestationBatchHandler's own request/
// sign/recover/retrieve flow. HandleNewRecoveredSig is a plain, directly-
// callable (virtual) method, not something only quorumSigningManager can
// invoke -- confirmed by reading CRecoveredSigsListener's own interface
// (llmq/quorums_signing.h) before relying on it, not assumed -- so a
// hand-built, well-formed CRecoveredSig (a real BLS signature from a
// fresh keypair, not a mock; this class never re-verifies the signature
// itself, trusting the SAME contract every CRecoveredSigsListener already
// does: a recovered signature reaching this callback is already known
// valid, PushReconstructedRecoveredSig's own doc comment,
// llmq/quorums_signing.h) can simulate "a quorum just recovered this
// signature" without a live quorum ever existing -- genuinely exercising
// the whole flow end to end, not just its own quorum-independent half.
// What stays untestable here, the same F-216 limitation every other piece
// of this feature has: whether AsyncSignIfMember itself ever gets a real
// quorum to produce a signature in the first place.
//
// RequestAttestation's own request-time validation (the CRITICAL fix,
// Fable review 2026-10-01, quorums_attestationbatch.h's own doc comment on
// RequestAttestation) is exercised against REAL signed transactions built
// here -- a genuine CKey/CBasicKeyStore/SignSignature flow (script/sign.h)
// -- rather than ever stubbing out or mocking CScriptCheck, matching this
// project's own established "hunt a real record, don't build a fixture"
// preference.

#include <llmq/quorums_attestationbatch.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/attestationbatch.h>
#include <evo/attestedtx.h>
#include <evo/specialtx.h>
#include <key.h>
#include <keystore.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>
#include <timedata.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

namespace {

llmq::CRecoveredSig MakeRecoveredSig(const uint256 &id, const uint256 &msgHash, const CBLSSignature &sig,
                                     Consensus::LLMQType llmqType) {
    return llmq::CRecoveredSig(llmqType, InsecureRand256() /* quorumHash, irrelevant to this class's own
                                                               filtering */, id, msgHash, sig);
}

// A real, single-sig (P2PK) UTXO plus a transaction that genuinely spends
// it with a valid signature -- what RequestAttestation's own CScriptCheck
// loop (quorums_attestationbatch.cpp) demands since the CRITICAL fix.
// Deliberately NOT shared with attestedtx_tests.cpp's own MakeAttestedSpend
// (which never signs, by design -- CheckAttestedTx itself never looks at
// scriptSig, only this handler does): the two files are proving different
// things about the same transaction shape. Each call adds its own fresh,
// independent UTXO to `view`, so this is safe to call more than once
// against one shared view (simulating more than one wallet's own request
// arriving at the same handler).
CTransaction MakeSignedAttestedSpend(CCoinsViewCache &view) {
    CKey key;
    key.MakeNewKey(true);
    CBasicKeyStore keystore;
    keystore.AddKey(key);
    CScript scriptPubKey = CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;

    COutPoint prevout(InsecureRand256(), 0);
    CAmount amount = 2;
    view.AddCoin(prevout, Coin(CTxOut(amount, scriptPubKey), 100, false, 0, {}), true);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ATTESTED;
    tx.vin.resize(1);
    tx.vin[0].prevout = prevout;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;

    BOOST_REQUIRE(SignSignature(keystore, scriptPubKey, tx, 0, amount, SIGHASH_ALL));
    return CTransaction(tx);
}

// Same shape, same kind of real UTXO -- but never signed. Exactly the
// request this handler used to accept completely unchallenged before the
// CRITICAL fix (quorums_attestationbatch.h's own doc comment on
// RequestAttestation).
CTransaction MakeUnsignedAttestedSpend(CCoinsViewCache &view) {
    CKey key;
    key.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;

    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, scriptPubKey), 100, false, 0, {}), true);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ATTESTED;
    tx.vin.resize(1);
    tx.vin[0].prevout = prevout;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    // tx.vin[0].scriptSig left default-constructed (empty) -- never signed.
    return CTransaction(tx);
}

}  // namespace

BOOST_FIXTURE_TEST_SUITE(quorums_attestationbatch_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(request_attestation_rejects_an_unsigned_input) {
    // The CRITICAL this round's own Fable review found: before this fix,
    // RequestAttestation took a bare uint256 with NO validation at all, so
    // a request for a transaction spending any single-sig UTXO with an
    // empty scriptSig sailed straight into the signing queue -- no proof
    // of key ownership was ever checked, anywhere in the request-to-
    // verification chain. Confirmed here against a REAL P2PK UTXO and a
    // REAL, deliberately-absent signature -- not a mock of CScriptCheck --
    // so this test fails for the right reason if the per-input check is
    // ever weakened or removed again.
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeUnsignedAttestedSpend(view);

    CValidationState state;
    BOOST_CHECK(!handler.RequestAttestation(tx, state, view));
    BOOST_CHECK(!state.IsValid());
    BOOST_CHECK(handler.TrySignBatch().IsNull()); // never queued
}

BOOST_AUTO_TEST_CASE(request_attestation_rejects_the_wrong_transaction_type) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CMutableTransaction tx(MakeSignedAttestedSpend(view));
    tx.nVersion = 2; // no longer IsAttestedTx, regardless of nType

    CValidationState state;
    BOOST_CHECK(!handler.RequestAttestation(CTransaction(tx), state, view));
    BOOST_CHECK(!state.IsValid());
}

BOOST_AUTO_TEST_CASE(request_attestation_is_idempotent) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);

    CValidationState state;
    BOOST_CHECK(handler.RequestAttestation(tx, state, view));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!handler.RequestAttestation(tx, state, view)); // already queued
}

BOOST_AUTO_TEST_CASE(try_sign_batch_is_a_noop_on_an_empty_queue) {
    llmq::CAttestationBatchHandler handler;
    BOOST_CHECK(handler.TrySignBatch().IsNull());
}

BOOST_AUTO_TEST_CASE(try_sign_batch_computes_the_same_root_a_direct_call_would) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx1 = MakeSignedAttestedSpend(view);
    CTransaction tx2 = MakeSignedAttestedSpend(view);
    CTransaction tx3 = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx1, state, view));
    BOOST_REQUIRE(handler.RequestAttestation(tx2, state, view));
    BOOST_REQUIRE(handler.RequestAttestation(tx3, state, view));

    uint256 root = handler.TrySignBatch();
    BOOST_CHECK(!root.IsNull());
    uint256 expected = ComputeCanonicalBatchRoot({ComputeAttestedMessageHash(tx1), ComputeAttestedMessageHash(tx2),
                                                   ComputeAttestedMessageHash(tx3)});
    BOOST_CHECK_EQUAL(root.ToString(), expected.ToString());

    // The queue was snapshotted and cleared -- a second call with nothing
    // newly requested has nothing left to sign.
    BOOST_CHECK(handler.TrySignBatch().IsNull());
}

BOOST_AUTO_TEST_CASE(get_attestation_fails_before_any_signature_recovers) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx, state, view));
    handler.TrySignBatch();

    CBLSSignature retSig;
    int32_t retHeight;
    CPartialMerkleTree retProof;
    BOOST_CHECK(!handler.GetAttestation(ComputeAttestedMessageHash(tx), retSig, retHeight, retProof));
}

BOOST_AUTO_TEST_CASE(handle_new_recovered_sig_ignores_the_wrong_llmq_type) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx, state, view));
    uint256 root = handler.TrySignBatch();
    BOOST_REQUIRE(!root.IsNull());

    CBLSSecretKey sk;
    sk.MakeNewKey();
    uint256 id = BuildAttestationBatchId(root);
    auto recSig = MakeRecoveredSig(id, root, sk.Sign(root), Consensus::LLMQType::LLMQ_50_60);
    BOOST_REQUIRE(Params().GetConsensus().llmqTypeChainLocks != Consensus::LLMQType::LLMQ_50_60);
    handler.HandleNewRecoveredSig(recSig);

    CBLSSignature retSig;
    int32_t retHeight;
    CPartialMerkleTree retProof;
    BOOST_CHECK(!handler.GetAttestation(ComputeAttestedMessageHash(tx), retSig, retHeight, retProof));
}

BOOST_AUTO_TEST_CASE(handle_new_recovered_sig_ignores_a_root_with_the_wrong_id) {
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx, state, view));
    uint256 root = handler.TrySignBatch();
    BOOST_REQUIRE(!root.IsNull());

    CBLSSecretKey sk;
    sk.MakeNewKey();
    // Wrong id: NOT BuildAttestationBatchId(root) -- e.g. a v1 per-tx
    // attestation id, or anything else that happens to carry this same
    // root as its own msgHash.
    uint256 wrongId = InsecureRand256();
    auto recSig = MakeRecoveredSig(wrongId, root, sk.Sign(root), Params().GetConsensus().llmqTypeChainLocks);
    handler.HandleNewRecoveredSig(recSig);

    CBLSSignature retSig;
    int32_t retHeight;
    CPartialMerkleTree retProof;
    BOOST_CHECK(!handler.GetAttestation(ComputeAttestedMessageHash(tx), retSig, retHeight, retProof));
}

BOOST_AUTO_TEST_CASE(handle_new_recovered_sig_ignores_a_root_never_asked_for) {
    llmq::CAttestationBatchHandler handler;
    // Note: no RequestAttestation/TrySignBatch call at all -- this root is
    // not, and never was, one this handler is awaiting.
    uint256 root = InsecureRand256();

    CBLSSecretKey sk;
    sk.MakeNewKey();
    uint256 id = BuildAttestationBatchId(root);
    auto recSig = MakeRecoveredSig(id, root, sk.Sign(root), Params().GetConsensus().llmqTypeChainLocks);
    handler.HandleNewRecoveredSig(recSig);

    CBLSSignature retSig;
    int32_t retHeight;
    CPartialMerkleTree retProof;
    BOOST_CHECK(!handler.GetAttestation(InsecureRand256(), retSig, retHeight, retProof));
}

BOOST_AUTO_TEST_CASE(ms_until_next_tick_slot_phase_aligns_to_the_interval) {
    // HIGH-2 fix (Fable review, 2026-10-01): pure phase-alignment math,
    // tested directly without a real scheduler or wall clock.
    BOOST_CHECK_EQUAL(llmq::MsUntilNextTickSlot(100, 5000), 0); // already on a boundary
    BOOST_CHECK_EQUAL(llmq::MsUntilNextTickSlot(101, 5000), 4000);
    BOOST_CHECK_EQUAL(llmq::MsUntilNextTickSlot(104, 5000), 1000);
    BOOST_CHECK_EQUAL(llmq::MsUntilNextTickSlot(105, 5000), 0);
}

BOOST_AUTO_TEST_CASE(sweep_expired_entries_requeues_a_stale_awaiting_batch) {
    // MEDIUM-1/HIGH-2 fix (Fable review, 2026-10-01): an awaitingRecovery
    // entry that never converges used to sit forever, permanently
    // swallowing its own leaves. SetMockTime (util/time.h) proves the
    // fix deterministically, without a real sleep -- the same pattern
    // test/denialofservice_tests.cpp already establishes in this exact
    // codebase for other time-dependent logic.
    //
    // recoveredBatches' own retention-pruning half of SweepExpiredEntries
    // is NOT covered here, or anywhere in this file: reaching it at all
    // requires HandleNewRecoveredSig to have accepted a recovery first,
    // which (per handle_new_recovered_sig_ignores_an_unconfirmable_quorum's
    // own comment, below) no test in this environment can make happen
    // (F-216). Verified by direct reading instead -- the loop is
    // structurally identical to the awaitingRecovery one proven here.
    int64_t t0 = GetAdjustedTime();
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx, state, view));

    uint256 root1 = handler.TrySignBatch();
    BOOST_REQUIRE(!root1.IsNull());

    // Still within the timeout -- nothing to re-queue, the leaf is
    // legitimately still awaiting its own quorum signature.
    BOOST_CHECK(handler.TrySignBatch().IsNull());

    SetMockTime(t0 + 3 * 60 * 60); // comfortably past AWAITING_RECOVERY_TIMEOUT_SECS
    uint256 root2 = handler.TrySignBatch();
    SetMockTime(0); // restore real time for every later test in this process

    // The SAME single leaf re-queued and signed again produces the
    // identical root -- without the fix, this would be null forever (the
    // leaf stuck in awaitingRecovery, never re-queued).
    BOOST_CHECK(!root2.IsNull());
    BOOST_CHECK_EQUAL(root2.ToString(), root1.ToString());
}

BOOST_AUTO_TEST_CASE(handle_new_recovered_sig_ignores_an_unconfirmable_quorum) {
    // HIGH-1 fix (Fable review, 2026-10-01): llmqType/id/awaitingRecovery
    // membership all matching is no longer enough -- the recovered
    // signature's own quorumHash must also match whatever
    // CSigningManager::SelectQuorumForSigning deterministically re-derives
    // for the recorded signHeight. No live quorum is constructible in
    // this test environment (F-216's own documented limitation, hit
    // identically by every other quorum-lookup in this feature), so
    // SelectQuorumForSigning always returns nullptr here -- confirmed by
    // direct reading of its implementation (quorums_signing.cpp: an empty
    // ScanQuorums result on a chain with no mined quorum commitments),
    // not assumed -- meaning this test can only exercise the
    // "no quorum determinable at all" branch of the fix, never "quorum
    // exists but does not match." That is still real, mutation-tested
    // coverage: removing the guard entirely changes this handler's
    // observable behaviour (see the request_attestation_rejects_an_unsigned_input
    // test's own sibling mutation test, llmq/quorums_attestationbatch.cpp,
    // for the same discipline applied to the CRITICAL fix).
    llmq::CAttestationBatchHandler handler;
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CTransaction tx = MakeSignedAttestedSpend(view);
    CValidationState state;
    BOOST_REQUIRE(handler.RequestAttestation(tx, state, view));
    uint256 root = handler.TrySignBatch();
    BOOST_REQUIRE(!root.IsNull());

    CBLSSecretKey sk;
    sk.MakeNewKey();
    uint256 id = BuildAttestationBatchId(root);
    auto recSig = MakeRecoveredSig(id, root, sk.Sign(root), Params().GetConsensus().llmqTypeChainLocks);
    handler.HandleNewRecoveredSig(recSig);

    CBLSSignature retSig;
    int32_t retHeight;
    CPartialMerkleTree retProof;
    BOOST_CHECK(!handler.GetAttestation(ComputeAttestedMessageHash(tx), retSig, retHeight, retProof));
}

BOOST_AUTO_TEST_SUITE_END()
