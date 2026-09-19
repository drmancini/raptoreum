// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>
#include <txmempool.h>
#include <amount.h>
#include <consensus/validation.h>
#include <key.h>
#include <keystore.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>


// F6 (1.2 review, 2026-09-19): a thrown BOOST_REQUIRE between setting
// g_commitmentBudgetActive = true and resetting it would leave the global on
// for every later test in the process. RAII guarantees the reset regardless.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

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
// budget from the MEMPOOL ENTRY's stored count (miner.cpp:315), set once at
// ATMP time -- so wiring has to happen exactly here, or the accurate counter
// exists but never reaches the path that matters. g_commitmentBudgetActive
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

BOOST_AUTO_TEST_SUITE_END()
