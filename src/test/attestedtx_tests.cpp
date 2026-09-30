// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 5.4.1 (build-plan.md, "attested transaction type"): CheckAttestedTx's own
// sanity-check core. Value conservation and double-spend correctness are
// deliberately NOT re-tested here -- both are Consensus::CheckTxInputs' own
// job, already covered by that function's own existing test coverage, and
// evo/attestedtx.h's own doc comment explains why re-testing the identical
// rule a second time here would risk two copies silently diverging instead
// of proving anything new. What IS new, and what this file tests: the
// script-shape restriction (build-plan.md's own 5.4 row, interim decision
// (a)) -- a real, hand-built coin, not a mock, matching this project's own
// established "hunt a real record" preference wherever a real one is cheap
// to build.

#include <evo/attestedtx.h>

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

#include <boost/test/unit_test.hpp>

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
}

BOOST_FIXTURE_TEST_CASE(attested_tx_accepts_a_single_sig_p2pk_input, BasicTestingSetup) {
    // check_sigs=false: this test is specifically about 5.4.1's own
    // script-shape restriction, not 5.4.2's attestation check -- a real,
    // quorum-verifiable attestation cannot be constructed without a live
    // quorum (F-216's own documented problem), so proving THIS rule in
    // isolation needs the same skip every sibling CheckXxxTx function's
    // own check_sigs parameter already provides for exactly this reason.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(CheckAttestedTx(CTransaction(tx), nullptr, state, view, false));
    BOOST_CHECK(state.IsValid());
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_missing_attestation_payload, BasicTestingSetup) {
    // 5.4.2: check_sigs=true and no vExtraPayload at all -- must fail at
    // GetTxPayload, before ever reaching quorum lookup or BLS
    // verification, matching every sibling CheckXxxTx function's own
    // "bad payload" convention (CheckMintAssetTx et al.).
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-payload");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_bare_multisig_input, BasicTestingSetup) {
    // F-13/F-90's own established lesson (sigopcount_tests.cpp): a bare
    // multisig prevout is real, live script, not a hypothetical -- exactly
    // the shape build-plan.md's own 5.4 interim decision (a) excludes.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    std::vector <CPubKey> keys{NewPubKey(), NewPubKey(), NewPubKey()};
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, BareMultisigScript(keys)), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-nonstandard-input");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_the_wrong_type, BasicTestingSetup) {
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

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_a_missing_input, BasicTestingSetup) {
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    // Deliberately never added to the view -- exercises the
    // Consensus::CheckTxInputs-should-have-already-caught-this fail-closed
    // branch, not a normal, expected path.
    COutPoint prevout(InsecureRand256(), 0);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-missing-input");
}

BOOST_FIXTURE_TEST_CASE(attested_tx_rejects_an_attestation_with_no_live_quorum, TestingSetup) {
    // Genuinely tried with BasicTestingSetup first and found a real
    // SIGABRT, not a hang -- `test_raptoreum: validation.cpp:99:
    // CChainState& ChainstateActive(): Assertion
    // 'g_chainman.m_active_chainstate' failed.`, confirmed via
    // --catch_system_errors=no rather than guessed from Boost.Test's own
    // signal report. That is BasicTestingSetup never initialising a
    // chainstate manager at all -- a test-fixture gap, not a production
    // one; ChainstateActive() is always valid by the time any real node
    // runs transaction validation. TestingSetup (this fixture) does
    // initialise one, and with it this reaches
    // llmq::CSigningManager::VerifyRecoveredSig's real quorum lookup and
    // returns false cleanly -- no quorum exists in this lightweight
    // fixture either (no live 3-of-3, F-216's own documented problem,
    // same as 5.3/F-238), but SelectQuorumForSigning's own "no quorum"
    // path is safe, confirmed live rather than assumed. This is the
    // closest this environment can get to testing 5.4.2's real
    // verification call without a live quorum actually signing anything.
    CCoinsView coinsDummy;
    CCoinsViewCache view(&coinsDummy);
    COutPoint prevout(InsecureRand256(), 0);
    view.AddCoin(prevout, Coin(CTxOut(2, P2PKScript(NewPubKey())), 100, false, 0, {}), true);

    CMutableTransaction tx = MakeAttestedSpend(prevout);
    CAttestationPayload payload;
    SetTxPayload(tx, payload);
    CValidationState state;
    BOOST_CHECK(!CheckAttestedTx(CTransaction(tx), nullptr, state, view, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-attested-tx-attestation");
}

BOOST_AUTO_TEST_SUITE_END()
