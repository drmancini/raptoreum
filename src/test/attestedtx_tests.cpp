// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 5.4.1/5.4.2 (build-plan.md, "attested transaction type"): CheckAttestedTx's
// own sanity-check core plus the quorum attestation itself. Value
// conservation is deliberately NOT re-tested here -- that is
// Consensus::CheckTxInputs' own job, already covered by that function's own
// existing test coverage, and evo/attestedtx.h's own doc comment explains
// why re-testing the identical rule a second time here would risk two
// copies silently diverging instead of proving anything new. What IS
// tested: the script-shape restriction, the unconfirmed-input restriction,
// the payload's own structural checks, and (as far as this environment
// allows without a live quorum) the attestation-verification call itself
// -- real, hand-built coins, not mocks, matching this project's own
// established "hunt a real record" preference wherever a real one is
// cheap to build.

#include <evo/attestedtx.h>

#include <bls/bls.h>
#include <coins.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

BOOST_AUTO_TEST_SUITE(attestedtx_tests)

namespace {
    CPubKey NewPubKey() {
        CKey key;
        key.MakeNewKey(true);
        return key.GetPubKey();
    }

    // Matches txvalidation_tests.cpp's own established P2PK scriptPubKey
    // shape (CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG).
    CScript P2PKScript(const CPubKey &pubkey) {
        return CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
    }

    // Matches sigopcount_tests.cpp's own established bare-multisig shape.
    CScript BareMultisigScript(const std::vector <CPubKey> &keys) {
        return GetScriptForMultisig(1, keys);
    }

    CMutableTransaction MakeAttestedSpend(const COutPoint &prevout) {
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_ATTESTED;
        tx.vin.resize(1);
        tx.vin[0].prevout = prevout;
        tx.vout.resize(1);
        tx.vout[0].nValue = 1;
        tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
        return tx;
    }

    // A well-formed-but-unverifiable v1 payload (default CBLSSignature is
    // invalid and safely fails VerifyInsecure, bls/bls.cpp -- confirmed
    // during the Fable review, not assumed) at a given height, for tests
    // that need to get past the structural/height checks without a live
    // quorum actually signing anything. nVersion pinned explicitly, not
    // left to the default: CAttestationPayload's own default (1) happens
    // to be v1 today, but this helper's callers all care specifically
    // about the v1 (direct-signature) path, so relying on the default
    // would silently start testing v2 instead the next time that default
    // ever changes (exactly the regression 5.4.4.1's own CURRENT_VERSION
    // bump caused here before being caught and fixed).
    void AttachPlausiblePayload(CMutableTransaction &tx, int32_t signHeight) {
        CAttestationPayload payload;
        payload.nVersion = 1;
        payload.nSignHeight = signHeight;
        SetTxPayload(tx, payload);
    }

    // 5.4.4.1 (build-plan.md, F-243/F-247): a well-formed v2 (batched)
    // payload -- a real CAttestationBatchProof (evo/attestationbatch.h)
    // proving this tx's own ComputeAttestedMessageHash is included in a
    // batch alongside `otherLeafCount` other, unrelated transactions,
    // built via the SAME BuildAttestationBatchProof production code uses
    // (CAttestationBatchHandler::GetAttestation), not hand-rolled.
    // Whether the batch's root has a recovered signature anywhere is NOT
    // this helper's concern -- it only builds a structurally correct
    // proof, matching AttachPlausiblePayload's own scope for v1.
    void AttachPlausibleBatchPayload(CMutableTransaction &tx, int32_t signHeight, size_t otherLeafCount = 2) {
        const uint256 myHash = ComputeAttestedMessageHash(CTransaction(tx));
        std::vector <uint256> leaves = {myHash};
        for (size_t i = 0; i < otherLeafCount; i++) {
            leaves.push_back(InsecureRand256());
        }
        std::sort(leaves.begin(), leaves.end());
        CAttestationPayload payload;
        payload.nVersion = 2;
        payload.nSignHeight = signHeight;
        payload.batchProof = BuildAttestationBatchProof(leaves, myHash);
        SetTxPayload(tx, payload);
    }
}

BOOST_FIXTURE_TEST_CASE(attested_tx_accepts_a_single_sig_p2pk_input, TestingSetup) {
    // check_sigs=false skips only the quorum-signature verification itself
    // (Fable review, CONFIRMED MEDIUM, fixed: payload presence/version/
    // height are structural rules, checked unconditionally, matching
    // every sibling CheckXxxTx function's own real convention) -- so this
    // test still needs a well-formed payload and a real chain tip to
    // reach the check_sigs gate at all, even though the signature inside
    // the payload is never actually verified here. A real, quorum-
    // verifiable attestation cannot be constructed without a live quorum
    // (F-216's own documented problem) -- proving 5.4.1's own script-shape
    // rule in isolation is what check_sigs=false is for.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    AttachPlausiblePayload(tx, pindexPrev->nHeight);
    CValidationState state;
    BOOST_CHECK(CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, false));
    BOOST_CHECK(state.IsValid());
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_missing_attestation_payload, TestingSetup) {
    // check_sigs=true and no vExtraPayload at all -- must fail at
    // GetTxPayload, before ever reaching the height check, the quorum
    // lookup, or BLS verification, matching every sibling CheckXxxTx
    // function's own "bad payload" convention (CheckMintAssetTx et al.).
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), ::ChainActive().Tip(), state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-payload");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_an_implausible_payload_height, TestingSetup) {
    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: nSignHeight is
    // now carried explicitly rather than inferred from pindexPrev, and
    // this is the new check bounding it -- a height after the block being
    // validated is structurally impossible regardless of whether the
    // signature itself would ever verify.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    AttachPlausiblePayload(tx, pindexPrev->nHeight + 1000); // in the future relative to pindexPrev
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-payload-height");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_bare_multisig_input, TestingSetup) {
    // F-13/F-90's own established lesson (sigopcount_tests.cpp): a bare
    // multisig prevout is real, live script, not a hypothetical -- exactly
    // the shape build-plan.md's own 5.4 interim decision (a) excludes.
    // Rejected by the per-input loop, before any payload is even looked
    // at, so no payload is attached here.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    std::vector <CPubKey> keys{NewPubKey(), NewPubKey(), NewPubKey()};
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, BareMultisigScript(keys)), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), ::ChainActive().Tip(), state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-nonstandard-input");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_the_wrong_type, BasicTestingSetup) {
    // The very first check in the function, before anything chain-state-
    // dependent is touched -- BasicTestingSetup (no chainstate manager) is
    // fine here, matching the SIGABRT lesson below: this path never
    // reaches ChainstateActive() or anything downstream of it.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    tx.nType = TRANSACTION_NORMAL;
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-type");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_the_right_type_at_the_wrong_version, BasicTestingSetup) {
    // Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: IsAttestedTx
    // (evo/attestedtx.h) requires nVersion==3 as well as nType -- not
    // reachable via CheckSpecialTx's own dispatch today (its nVersion!=3
    // guard never calls this function at all), but defense in depth for the
    // same reason validation.cpp's three 5.4.3 sites all needed the fix:
    // two different files must not re-derive "is this an attested tx" with
    // two different predicates.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    tx.nVersion = 4;
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-type");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_no_inputs, BasicTestingSetup) {
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ATTESTED;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-no-inputs");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_an_unconfirmed_input, TestingSetup) {
    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: this is NOT an
    // "already caught elsewhere, unreachable" branch the way an earlier
    // version of this test's own comment claimed -- ATMP checks this
    // function's own inputs against CoinsTip() specifically (mempool-
    // blind), and ConnectBlock runs special-tx checks before its own
    // UpdateCoins -- so an attested transaction spending an unconfirmed
    // parent genuinely reaches this branch. Deliberately never added to
    // the view, exercising a real, named v1 restriction: attested inputs
    // must already be confirmed.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), ::ChainActive().Tip(), state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-unconfirmed-input");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_an_attestation_with_no_live_quorum, TestChain100Setup) {
    // Genuinely tried with BasicTestingSetup first and found a real
    // SIGABRT, not a hang -- `test_raptoreum: validation.cpp:99:
    // CChainState& ChainstateActive(): Assertion
    // 'g_chainman.m_active_chainstate' failed.`, confirmed via
    // --catch_system_errors=no rather than guessed from Boost.Test's own
    // signal report. That is BasicTestingSetup never initialising a
    // chainstate manager at all -- a test-fixture gap, not a production
    // one; ChainstateActive() is always valid by the time any real node
    // runs transaction validation.
    //
    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: the first fix
    // (switching to plain TestingSetup with pindexPrev=nullptr) still did
    // not reach the real quorum scan -- SelectQuorumForSigning's own
    // startBlockHeight = signHeight - SIGN_HEIGHT_OFFSET (8,
    // llmq/quorums_signing.h) guard returns early for any signHeight below
    // 8, before ever calling ScanQuorums, so a genesis-only chain (height
    // 0) cannot exercise it either. TestChain100Setup's own real,
    // 100-block tip, with nSignHeight pinned to that same height, clears
    // that guard and reaches the real scan -- confirmed by this test
    // passing for the right reason (no quorum registered in this fixture,
    // not a height-format rejection caught earlier).
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    BOOST_REQUIRE(pindexPrev->nHeight >= 8);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    AttachPlausiblePayload(tx, pindexPrev->nHeight);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-attestation");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_message_hash_is_independent_of_the_signature_it_carries, BasicTestingSetup) {
    // Fable review (2026-09-30), CONFIRMED HIGH, fixed: the attested
    // message must NOT be tx.GetHash() itself, since vExtraPayload (which
    // carries payload.sig) is hashed unconditionally by
    // CTransaction::Serialize for any non-normal nType -- making the
    // signed message depend on the very signature it authenticates.
    // No live quorum is obtainable in this environment (F-216), so no
    // call through CheckAttestedTx can ever distinguish the fixed
    // function from a reverted `tx.GetHash()` -- every reachable failure
    // in this suite happens at the quorum-lookup step, before the hash's
    // VALUE matters at all. This test instead holds the fix's one
    // load-bearing property directly, with no quorum involved: two
    // transactions identical except for payload.sig must produce the
    // SAME ComputeAttestedMessageHash (proving the fix), while still
    // producing DIFFERENT tx.GetHash() (proving vExtraPayload really
    // does flow into the raw hash, so the two functions are not
    // accidentally equal for some unrelated reason).
    CBLSSecretKey sk;
    sk.MakeNewKey();

    // nVersion pinned explicitly (5.4.4.1): this test's own payload.sig
    // field is only ever serialized under v1 -- relying on the default
    // caused exactly this test to silently start exercising v2 instead the
    // one time CAttestationPayload's own default briefly tracked
    // CURRENT_VERSION (mutation-tested, not hypothetical).
    CMutableTransaction txA = MakeAttestedSpend(COutPoint(InsecureRand256(), 0));
    CAttestationPayload payloadA;
    payloadA.nVersion = 1;
    payloadA.nSignHeight = 50;
    payloadA.sig = sk.Sign(InsecureRand256());
    SetTxPayload(txA, payloadA);

    CMutableTransaction txB = txA;
    CAttestationPayload payloadB;
    payloadB.nVersion = 1;
    payloadB.nSignHeight = 50;
    payloadB.sig = sk.Sign(InsecureRand256());
    BOOST_REQUIRE(payloadB.sig != payloadA.sig);
    SetTxPayload(txB, payloadB);

    const CTransaction ctxA(txA), ctxB(txB);
    BOOST_CHECK(ctxA.GetHash() != ctxB.GetHash());
    BOOST_CHECK_EQUAL(ComputeAttestedMessageHash(ctxA).ToString(), ComputeAttestedMessageHash(ctxB).ToString());
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_payload_height_older_than_the_plausible_window, BasicTestingSetup) {
    // The other side of IsPlausibleAttestationSignHeight's boundary
    // (attested_tx_rejects_an_implausible_payload_height above covers a
    // height in the future; this covers one so old it is outside
    // MAX_ATTESTATION_SIGN_HEIGHT_AGE) -- both boundaries are part of the
    // same MEDIUM fix and neither implies the other passes correctly. A
    // synthetic CBlockIndex (this test suite's own established idiom,
    // e.g. commitments_negotiation_tests.cpp) rather than a real fixture
    // tip -- a real chain tall enough for a non-negative-but-still-stale
    // nSignHeight to exist is not available cheaply, and this rejection
    // happens before check_sigs is ever consulted, so no chainstate
    // manager is touched either way; BasicTestingSetup is safe here for
    // the same reason it already is for attested_tx_rejects_the_wrong_type.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CBlockIndex indexPrev;
    indexPrev.nHeight = 1000;
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    AttachPlausiblePayload(tx, 1000 - 577); // non-negative, but one past MAX_ATTESTATION_SIGN_HEIGHT_AGE (576)
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), &indexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-payload-height");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_missing_payload_even_when_check_sigs_is_false, TestingSetup) {
    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: payload presence/
    // version/height are structural rules, not signature verification, so
    // check_sigs=false must not let a payload-less attested transaction
    // through -- every sibling CheckXxxTx function already holds this
    // property (CheckProRegTx et al. parse and validate their own payload
    // unconditionally). attested_tx_accepts_a_single_sig_p2pk_input above
    // also uses check_sigs=false but attaches a well-formed payload, so it
    // cannot catch a regression back to "check_sigs gates everything" --
    // this test's whole point is the absence of one.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout); // no payload attached
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), ::ChainActive().Tip(), state, view, false));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-payload");
}

// 5.4.4.1 (build-plan.md, F-243/F-244): the v2 (batched) payload's own
// tests. Fable review (2026-10-01), CONFIRMED CRITICAL: v2 originally
// carried no signature at all, looking one up in quorumSigningManager's
// own store instead -- fixed to carry the batch's own signature directly
// and verify via VerifyRecoveredSig exactly like v1 (CAttestationPayload's
// own `sig` doc comment, evo/attestedtx.h, has the full finding). This
// means every structural-rejection test below now needs TestingSetup, not
// BasicTestingSetup: a mutation that weakens an earlier check no longer
// falls through into a store lookup (which could null-pointer-crash
// under BasicTestingSetup, a real finding from this same review) but into
// the SAME VerifyRecoveredSig call v1 uses, which needs ChainstateActive()
// -- confirmed, not assumed, by F-241's own mutant 2 hitting exactly this
// once already. None of these need a live quorum except the last, same as
// v1 -- a well-formed, correctly-matching proof whose batch signature
// does not verify is fully testable; the "genuinely signed by a real
// quorum" branch is the SAME kind of irreducible gap v1's own
// VerifyRecoveredSig call always had (F-216).

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_v2_payload_with_a_default_constructed_proof, TestingSetup) {
    // 5.4.4.1-OPEN item (b) (F-247): with CAttestationBatchProof
    // (evo/attestationbatch.h) there is no distinct "malformed proof"
    // rejection anymore -- a default-constructed proof (zero siblings)
    // simply makes ExtractAttestationBatchRoot return the leaf itself
    // unchanged (the single-leaf-batch case), which then fails the
    // ordinary attestation check below it like any other wrong root does
    // (no live quorum ever signs this transaction's own bare hash as a
    // "batch" of one, F-216). One rejection path, not two -- this test's
    // own reject reason reflects that, not the old, now-nonexistent
    // "bad-attested-tx-batch-proof".
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CAttestationPayload payload;
    payload.nVersion = 2;
    payload.nSignHeight = pindexPrev->nHeight;
    // batchProof left default-constructed.
    SetTxPayload(tx, payload);

    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-batch-attestation");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_v2_payload_whose_proof_is_for_a_different_transaction, TestingSetup) {
    // A real, well-formed proof -- but built for a leaf that is NOT this
    // transaction's own ComputeAttestedMessageHash, e.g. a proof copied
    // from a different transaction's own payload. ExtractAttestationBatchRoot
    // hashes THIS transaction's own leaf up through siblings that were
    // never computed against it, so the resulting root is simply wrong --
    // rejected the same ordinary way any wrong root is, not a distinct
    // "wrong leaf" check the old CPartialMerkleTree-based design needed.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    std::vector <uint256> leaves = {InsecureRand256(), InsecureRand256(), InsecureRand256()};
    std::sort(leaves.begin(), leaves.end());
    CAttestationPayload payload;
    payload.nVersion = 2;
    payload.nSignHeight = pindexPrev->nHeight;
    payload.batchProof = BuildAttestationBatchProof(leaves, leaves[1]); // a proof for someone else's leaf
    SetTxPayload(tx, payload);

    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-batch-attestation");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_v2_payload_whose_signature_does_not_verify, TestChain100Setup) {
    // TestChain100Setup, not TestingSetup: v2 now reaches the SAME
    // VerifyRecoveredSig->SelectQuorumForSigning call v1's own no-live-
    // quorum test does, which needs signHeight - SIGN_HEIGHT_OFFSET (8,
    // llmq/quorums_signing.h) >= 0 to reach the real quorum scan rather
    // than an early-return guard -- confirmed by this test's own runtime
    // (matching v1's own ~4.6s, not v1's sibling structural tests' ~10ms).
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    const CBlockIndex *pindexPrev = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexPrev != nullptr);
    BOOST_REQUIRE(pindexPrev->nHeight >= 8);
    CMutableTransaction tx = MakeAttestedSpend(prevout);
    AttachPlausibleBatchPayload(tx, pindexPrev->nHeight);

    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), pindexPrev, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-batch-attestation");
}

BOOST_AUTO_TEST_CASE(attested_tx_payload_default_version_is_one_not_current_version) {
    // Fable review (2026-10-01), LOW: hardening every OTHER test to pin
    // nVersion explicitly (the 5.4.4.1 CURRENT_VERSION-bump regression,
    // this file's own earlier fix) removed the only canary that would
    // catch a future "tidy-up" reverting the deliberate {1} default
    // (evo/attestedtx.h's own comment there explains why) back to
    // {CURRENT_VERSION} -- every sibling payload type uses CURRENT_VERSION
    // as both, making that an easy, plausible slip.
    BOOST_CHECK_EQUAL(CAttestationPayload().nVersion, 1);
}

BOOST_AUTO_TEST_SUITE_END()
