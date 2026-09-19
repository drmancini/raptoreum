// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <limits>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <tinyformat.h>
#include <uint256.h>
#include <validation.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

// 1.2 (Mike, 2026-09-19): the body-byte / input-count cap. Two limits, not
// one -- a byte cap alone can't tell realistic multi-input/output traffic
// apart from an attacker packing minimal-size inputs (F-91, F-93, F-97), so
// COMMITMENT_BUDGET_MAX_INPUTS is what actually bounds worst-case time,
// independent of how generous COMMITMENT_BUDGET_BODY_BYTES is for storage.

BOOST_FIXTURE_TEST_SUITE(blockbudget_tests, BasicTestingSetup)

// Every existing call site passes no second argument (or, for MaxBlockSize,
// at most one), relying on fCommitmentBudgetActive's false default -- this
// must reproduce the exact pre-1.2 byte-indexed values unchanged.
BOOST_AUTO_TEST_CASE(max_block_size_threshold_selection) {
    BOOST_CHECK_EQUAL(MaxBlockSize(), MAX_DIP0001_BLOCK_SIZE);
    BOOST_CHECK_EQUAL(MaxBlockSize(true), MAX_DIP0001_BLOCK_SIZE);
    BOOST_CHECK_EQUAL(MaxBlockSize(false), MAX_LEGACY_BLOCK_SIZE);
    BOOST_CHECK_EQUAL(MaxBlockSize(true, false), MAX_DIP0001_BLOCK_SIZE);
    BOOST_CHECK_EQUAL(MaxBlockSize(false, false), MAX_LEGACY_BLOCK_SIZE);

    // Active: the body-byte budget wins regardless of fDIP0001Active, exactly
    // like MaxBlockSigOps's existing fCommitmentBudgetActive precedent.
    BOOST_CHECK_EQUAL(MaxBlockSize(true, true), COMMITMENT_BUDGET_BODY_BYTES);
    BOOST_CHECK_EQUAL(MaxBlockSize(false, true), COMMITMENT_BUDGET_BODY_BYTES);
}

// No pre-1.2 concept exists for this, so "off" must not silently forbid
// anything -- effectively unlimited, not zero or some arbitrary small value.
BOOST_AUTO_TEST_CASE(max_block_inputs_threshold_selection) {
    BOOST_CHECK_EQUAL(MaxBlockInputs(), std::numeric_limits<unsigned int>::max());
    BOOST_CHECK_EQUAL(MaxBlockInputs(false), std::numeric_limits<unsigned int>::max());
    BOOST_CHECK_EQUAL(MaxBlockInputs(true), COMMITMENT_BUDGET_MAX_INPUTS);
}

static CMutableTransaction MakeTxWithInputs(size_t nIn) {
    CMutableTransaction tx;
    tx.vin.resize(nIn);
    for (size_t i = 0; i < nIn; i++) {
        tx.vin[i].prevout = COutPoint(uint256S(strprintf("%064x", i + 1)), 0);
    }
    tx.vout.resize(1);
    tx.vout[0].nValue = 1000;
    tx.vout[0].scriptPubKey << OP_TRUE;
    return tx;
}

// The coinbase's own single dummy input touches no UTXO and costs none of
// the per-input validation work this budget bounds (F-93/F-94/F-97's
// mechanism is about real spends) -- it must not count towards the cap.
BOOST_AUTO_TEST_CASE(get_block_input_count_excludes_coinbase_and_sums_the_rest) {
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 1000;
    coinbase.vout[0].scriptPubKey << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    BOOST_REQUIRE(CTransaction(coinbase).IsCoinBase());

    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(3)));
    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(5)));
    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(0)));   // degenerate, must not crash

    BOOST_CHECK_EQUAL(GetBlockInputCount(block), 8U);   // 3 + 5 + 0, NOT +1 for the coinbase
}

// F1's own lesson applied here without waiting for a second review to find
// it: prove CheckBlock actually enforces the aggregate input-count cap, not
// just that GetBlockInputCount itself counts correctly in isolation.
// Reproduces at a reduced scale (7,001 transactions x 100 inputs = 700,100,
// just over COMMITMENT_BUDGET_MAX_INPUTS) rather than the real ~1,500 tx/s
// planning shape, to keep the test fast; K-3's ~100 kB per-tx ceiling is
// nowhere near touched at 100 inputs/tx (~4.1 kB each).
BOOST_AUTO_TEST_CASE(checkblock_rejects_aggregate_input_count_over_the_budget) {
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = uint256S("0xbeef");
    block.nTime = 1700000000;
    block.nBits = 0x207fffff;
    block.nNonce = 7;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 1000;
    coinbase.vout[0].scriptPubKey << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(coinbase));

    const unsigned int kTxCount = 7001;      // x 100 inputs = 700,100 > 700,000
    const unsigned int kInputsPerTx = 100;
    for (unsigned int t = 0; t < kTxCount; t++) {
        CMutableTransaction tx;
        tx.vin.resize(kInputsPerTx);
        for (unsigned int i = 0; i < kInputsPerTx; i++) {
            tx.vin[i].prevout = COutPoint(uint256S(strprintf("%08x%056x", t, i + 1)), 0);
        }
        tx.vout.resize(1);
        tx.vout[0].nValue = 1000;
        tx.vout[0].scriptPubKey << OP_TRUE;
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    BOOST_REQUIRE_GT(GetBlockInputCount(block), COMMITMENT_BUDGET_MAX_INPUTS);

    g_commitmentBudgetActive = true;
    CValidationState state;
    bool accepted = CheckBlock(block, state, Params().GetConsensus(), /*nHeight=*/1,
                               /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);
    g_commitmentBudgetActive = false;

    BOOST_CHECK(!accepted);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-inputs");
}

BOOST_AUTO_TEST_SUITE_END()
