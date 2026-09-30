// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>
#include <txmempool.h>
#include <amount.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <key.h>
#include <keystore.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>
#include <update/update.h>

#include <boost/test/unit_test.hpp>


// F6 (1.2 review, 2026-09-19): a thrown BOOST_REQUIRE between setting
// g_commitmentBudgetActive = true and resetting it would leave the global on
// for every later test in the process. RAII guarantees the reset regardless.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

// Fable review (2026-10-01), F-242's CRITICAL: forces EUpdate::ATTESTED_TX
// active without a real, multi-round voting chain -- mirrors
// commitmentmode_activation_tests.cpp's own ActivationHeightGuard technique
// (Update's heightActivated constructor parameter bypasses the whole voting
// simulation, update/update.cpp's own `if (update->HeightActivated() >= 0)`
// early return) and attestedtx_activation_tests.cpp's own local copy of it --
// duplicated here too, matching this codebase's own established per-file
// convention for small test-only helpers rather than sharing one.
namespace {
static const int64_t ATTESTED_TX_TEST_NEVER_HEIGHT = 2000000000;

Update MakeAttestedTxTestUpdate(int64_t heightActivated) {
    return Update(EUpdate::ATTESTED_TX, "Attested Transaction (test)", 4, 1, 0, 1, 1, 0, false,
                 VoteThreshold(0, 0, 1), VoteThreshold(0, 0, 1), false, heightActivated);
}

struct AttestedTxActiveGuard {
    explicit AttestedTxActiveGuard(int64_t heightActivated) {
        Updates().Add(MakeAttestedTxTestUpdate(heightActivated));
    }
    ~AttestedTxActiveGuard() {
        Updates().Add(MakeAttestedTxTestUpdate(ATTESTED_TX_TEST_NEVER_HEIGHT));
    }
};

// devnet/testnet/regtest's own real default (-acceptnonstdtxn effectively on,
// init.cpp) -- fRequireStandard defaults to true in this test binary (never
// touched by init.cpp's own AppInit path), which would let IsStandardTx's
// unrelated policy-level version check (policy/policy.cpp) mask whether the
// CONSENSUS-level fix below actually works, for the mempool-acceptance test
// specifically. ConnectBlock has no IsStandardTx-equivalent, so this guard is
// not needed for the ConnectBlock-side test.
struct RequireStandardGuard {
    bool previousValue;
    RequireStandardGuard() : previousValue(fRequireStandard) { fRequireStandard = false; }
    ~RequireStandardGuard() { fRequireStandard = previousValue; }
};
}  // namespace

BOOST_AUTO_TEST_SUITE(txvalidation_tests)

/**
 * Ensure that the mempool won't accept coinbase transactions.
 */
BOOST_FIXTURE_TEST_CASE(tx_mempool_reject_coinbase, TestChain100Setup
)
{
CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
CMutableTransaction coinbaseTx;

coinbaseTx.
nVersion = 1;
coinbaseTx.vin.resize(1);
coinbaseTx.vout.resize(1);
coinbaseTx.vin[0].
scriptSig = CScript() << OP_11 << OP_EQUAL;
coinbaseTx.vout[0].
nValue = 1 * CENT;
coinbaseTx.vout[0].
scriptPubKey = scriptPubKey;

assert(CTransaction(coinbaseTx)
.

IsCoinBase()

);

CValidationState state;

LOCK(cs_main);

unsigned int initialPoolSize = m_node.mempool->size();

BOOST_CHECK_EQUAL(
false,
AcceptToMemoryPool(*m_node
.mempool, state,
MakeTransactionRef(coinbaseTx),
        nullptr /* pfMissingInputs */,
true /* bypass_limits */,
0 /* nAbsurdFee */));

// Check that the transaction hasn't been added to mempool.
BOOST_CHECK_EQUAL(m_node
.mempool->

size(), initialPoolSize

);

// Check that the validation state reflects the unsuccessful attempt.
BOOST_CHECK(state
.

IsInvalid()

);
BOOST_CHECK_EQUAL(state
.

GetRejectReason(),

"coinbase");

int nDoS;
BOOST_CHECK_EQUAL(state
.
IsInvalid(nDoS),
true);
BOOST_CHECK_EQUAL(nDoS,
100);
}

// 1.2 (D-17, D-18, F-86, F-89): a bare (non-P2SH) multisig spend is invisible
// to GetLegacySigOpCount and GetP2SHSigOpCount alike -- the real CHECKMULTISIG
// lives in the prevout's own scriptPubKey. GetAccurateSigOpCount closes that
// (src/consensus/tx_verify.cpp), but the miner reads its block-assembly sigop
// budget from the MEMPOOL ENTRY's stored count (BlockAssembler::TestPackage's
// packageSigOps check, miner.cpp -- C3, named by symbol since this line-number
// citation had already drifted once), set once at ATMP time -- so wiring has
// to happen exactly here, or the accurate counter exists but never reaches
// the path that matters. g_commitmentBudgetActive
// (test-only until 4.6 has a real activation bit) switches which counter ATMP
// stores; this proves the switch actually reaches a real mempool entry.
BOOST_FIXTURE_TEST_CASE(mempool_entry_uses_accurate_sigops_when_budget_active, TestChain100Setup) {
    CBasicKeyStore keystore;
    std::vector<CPubKey> keys;
    for (int i = 0; i < 3; i++) {
        CKey k;
        k.MakeNewKey(true);
        keystore.AddKey(k);
        keys.push_back(k.GetPubKey());
    }
    CScript bareMultisig = GetScriptForMultisig(1, keys);   // 1-of-3, standard to create, NOT P2SH

    // Fund the multisig output from a mature coinbase and mine it, so it has
    // a real Coin the mempool's ATMP can look up.
    keystore.AddKey(coinbaseKey);
    CMutableTransaction fundTx;
    fundTx.vin.resize(1);
    fundTx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    fundTx.vout.resize(1);
    fundTx.vout[0].nValue = 11 * CENT;
    fundTx.vout[0].scriptPubKey = bareMultisig;
    BOOST_REQUIRE(SignSignature(keystore, *m_coinbase_txns[0], fundTx, 0, SIGHASH_ALL));
    CreateAndProcessBlock({fundTx}, coinbaseKey);

    // Spend the bare multisig output, properly signed -- standard and valid
    // either way, so acceptance itself does not depend on g_commitmentBudgetActive.
    CMutableTransaction spendTx;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(fundTx.GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    BOOST_REQUIRE(SignSignature(keystore, CTransaction(fundTx), spendTx, 0, SIGHASH_ALL));

    LOCK(cs_main);
    CommitmentBudgetGuard guard;

    g_commitmentBudgetActive = false;
    CValidationState stateLegacy;
    BOOST_REQUIRE(AcceptToMemoryPool(*m_node.mempool, stateLegacy, MakeTransactionRef(spendTx),
                                     nullptr, true, 0));
    auto legacyIter = m_node.mempool->GetIter(spendTx.GetHash());
    BOOST_REQUIRE(legacyIter);
    unsigned int legacyCount = (*legacyIter)->GetSigOpCount();
    m_node.mempool->removeRecursive(CTransaction(spendTx), MemPoolRemovalReason::MANUAL);

    g_commitmentBudgetActive = true;
    CValidationState stateAccurate;
    BOOST_REQUIRE(AcceptToMemoryPool(*m_node.mempool, stateAccurate, MakeTransactionRef(spendTx),
                                     nullptr, true, 0));
    auto accurateIter = m_node.mempool->GetIter(spendTx.GetHash());
    BOOST_REQUIRE(accurateIter);
    unsigned int accurateCount = (*accurateIter)->GetSigOpCount();
    m_node.mempool->removeRecursive(CTransaction(spendTx), MemPoolRemovalReason::MANUAL);
    g_commitmentBudgetActive = false;

    // Absolute values, not just the delta (1.2 test review, 2026-09-19): a
    // delta-only check passes under any additive error shared by both
    // branches. spendTx's scriptSig is OP_0 <sig> (0 sigops of its own), its
    // output is P2PKH (1 CHECKSIG), and the bare 1-of-3 prevout is invisible
    // to legacy/P2SH -- so legacy must be exactly 1, accurate exactly 4.
    BOOST_CHECK_EQUAL(legacyCount, 1U);
    BOOST_CHECK_EQUAL(accurateCount, 4U);
}

// 5.4.3 (build-plan.md, F-242): closes a real gap found while genuinely
// mutation-testing the new ATMP CheckInputs-skip -- a mutant that skipped
// CheckInputs/CheckInputsFromMempoolAndCache unconditionally for EVERY
// transaction (not just the intended, narrow TRANSACTION_ATTESTED case)
// passed the entire 761-case suite silently, confirmed live before writing
// this test, not assumed. Every existing CheckInputs-adjacent test either
// calls CheckInputs directly (txvalidationcache_tests.cpp, bypassing ATMP
// entirely) or signs correctly (tx_mempool_block_doublespend and this
// file's own mempool_entry_uses_accurate_sigops_when_budget_active) -- none
// submit a deliberately-invalid signature through AcceptToMemoryPool's
// real, full entry point and check for rejection there.
BOOST_FIXTURE_TEST_CASE(tx_mempool_rejects_an_invalid_signature, TestChain100Setup) {
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    CMutableTransaction spendTx;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = scriptPubKey;

    // Signs the CORRECT sighash (matching the real prevout's P2PK
    // scriptPubKey exactly, tx_mempool_block_doublespend's own technique
    // above) with the WRONG key -- structurally a real, well-formed
    // signature (right shape, right SIGHASH_ALL byte), just over a key that
    // does not match coinbaseKey's pubkey embedded in scriptPubKey. This
    // fails at cryptographic verification specifically, not at parsing, so
    // it actually exercises CheckInputs' own script-check path rather than
    // an earlier structural rejection.
    CKey wrongKey;
    wrongKey.MakeNewKey(true);
    std::vector<unsigned char> vchSig;
    uint256 hash = SignatureHash(scriptPubKey, spendTx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    BOOST_REQUIRE(wrongKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char) SIGHASH_ALL);
    spendTx.vin[0].scriptSig = CScript() << vchSig;

    LOCK(cs_main);
    CValidationState state;
    BOOST_CHECK(!AcceptToMemoryPool(*m_node.mempool, state, MakeTransactionRef(spendTx),
                                    nullptr /* pfMissingInputs */, true /* bypass_limits */, 0 /* nAbsurdFee */));
    // The real reason string also carries a parenthetical ScriptErrorString
    // suffix (validation.cpp's own strprintf("mandatory-script-verify-flag-failed
    // (%s)", ...)) -- checking the fixed prefix proves the right rejection
    // category fired without depending on that detail's exact wording.
    BOOST_CHECK_EQUAL(state.GetRejectReason().rfind("mandatory-script-verify-flag-failed", 0), 0U);
}

// 5.4.3 (build-plan.md, F-242): the ConnectBlock-side twin of
// tx_mempool_rejects_an_invalid_signature above -- AcceptToMemoryPool never
// calls ConnectBlock, so that test alone cannot catch a mutant that skips
// ConnectBlock's own CheckInputs call unconditionally. Mirrors
// tx_mempool_block_doublespend's own "block containing an invalid
// transaction is not accepted as the new tip" technique (CreateAndProcessBlock
// embeds the given transactions directly, with no mempool pre-filtering, so
// the real ProcessNewBlock->...->ConnectBlock pipeline is what has to catch
// this, same as a genuinely-received network block would).
BOOST_FIXTURE_TEST_CASE(connectblock_rejects_an_invalid_signature, TestChain100Setup) {
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    CMutableTransaction spendTx;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = scriptPubKey;

    CKey wrongKey;
    wrongKey.MakeNewKey(true);
    std::vector<unsigned char> vchSig;
    uint256 hash = SignatureHash(scriptPubKey, spendTx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    BOOST_REQUIRE(wrongKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char) SIGHASH_ALL);
    spendTx.vin[0].scriptSig = CScript() << vchSig;

    CBlock block = CreateAndProcessBlock({spendTx}, scriptPubKey);
    LOCK(cs_main);
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash() != block.GetHash());
}

// Fable review (2026-10-01), CONFIRMED CRITICAL, closing the gap that
// finding's own executable PoC demonstrated live: a transaction with
// nVersion=4 (anything other than 3) and nType=TRANSACTION_ATTESTED, with a
// completely EMPTY scriptSig, was accepted into the mempool once
// EUpdate::ATTESTED_TX activated -- CheckSpecialTx (evo/specialtx.cpp) only
// ever dispatches to CheckAttestedTx when nVersion==3 exactly, so this
// transaction's attestation was never checked, and the 5.4.3 CheckInputs-skip
// (keyed, before the fix, on nType alone) meant its signature was never
// checked either. Fixed via IsAttestedTx (evo/attestedtx.h), which requires
// both. RequireStandardGuard matters here specifically: without it, the
// default test-binary fRequireStandard=true would let IsStandardTx's own,
// unrelated policy-level version check reject this first, masking whether
// the CONSENSUS-level fix actually works -- devnet's own real default
// (-acceptnonstdtxn effectively on) does not have that policy backstop.
BOOST_FIXTURE_TEST_CASE(tx_mempool_rejects_a_wrong_version_attested_typed_unsigned_spend, TestChain100Setup) {
    AttestedTxActiveGuard activation(1);
    RequireStandardGuard notStandard;

    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CMutableTransaction spendTx;
    spendTx.nVersion = 4;
    spendTx.nType = TRANSACTION_ATTESTED;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = scriptPubKey;
    // scriptSig left completely empty -- no signature of any kind.

    LOCK(cs_main);
    CValidationState state;
    BOOST_CHECK(!AcceptToMemoryPool(*m_node.mempool, state, MakeTransactionRef(spendTx),
                                    nullptr /* pfMissingInputs */, true /* bypass_limits */, 0 /* nAbsurdFee */));
}

// The ConnectBlock-side twin -- AcceptToMemoryPool never calls ConnectBlock,
// so the test above alone cannot catch a version-bypass that only manifests
// in ConnectBlock's own per-tx loop. No RequireStandardGuard needed:
// ConnectBlock has no IsStandardTx-equivalent policy check to mask anything.
BOOST_FIXTURE_TEST_CASE(connectblock_rejects_a_wrong_version_attested_typed_unsigned_spend, TestChain100Setup) {
    AttestedTxActiveGuard activation(1);

    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CMutableTransaction spendTx;
    spendTx.nVersion = 4;
    spendTx.nType = TRANSACTION_ATTESTED;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = scriptPubKey;

    CBlock block = CreateAndProcessBlock({spendTx}, scriptPubKey);
    LOCK(cs_main);
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash() != block.GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
