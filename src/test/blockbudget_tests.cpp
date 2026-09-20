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

// F6/F-98 (Fable review of the tests, 2026-09-19): a thrown BOOST_REQUIRE
// between setting g_commitmentBudgetActive = true and resetting it would
// leave the global on for every later test in the process. Mirrors the guard
// txvalidation_tests.cpp already introduced for exactly this.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

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
// NOTE (test review, 2026-09-19): MaxBlockInputs(false)/MaxBlockInputs() are
// NOT reachable through either enforcement site today -- both CheckBlock and
// ContextualCheckBlock short-circuit on `g_commitmentBudgetActive &&` before
// ever calling MaxBlockInputs, specifically to avoid an O(vtx)
// GetBlockInputCount scan on every block ever validated while the budget is
// off (unlike MaxBlockSigOps, whose off-branch IS live at every real call
// site, since nSigOps is already accumulated for the pre-existing legacy
// check regardless of the budget). This test still pins the function's own
// contract; it isn't proof of end-to-end off-state behavior at the
// enforcement sites -- see checkblock_byte_cap_is_gated_by_the_budget_flag_when_off
// for that on the byte-cap side. No input-cap equivalent is possible at
// comparable scale: any block over the input cap is inherently far over the
// byte cap too, at K-3's ~41 B/input floor -- see
// checkblock_rejects_aggregate_input_count_over_the_budget's own size check.
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

static CMutableTransaction MakeCoinbase() {
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 1000;
    coinbase.vout[0].scriptPubKey << OP_TRUE;
    return coinbase;
}

// The coinbase's own single dummy input touches no UTXO and costs none of
// the per-input validation work this budget bounds (F-93/F-94/F-97's
// mechanism is about real spends) -- it must not count towards the cap.
BOOST_AUTO_TEST_CASE(get_block_input_count_excludes_coinbase_and_sums_the_rest) {
    CBlock block;
    CMutableTransaction coinbase = MakeCoinbase();
    block.vtx.push_back(MakeTransactionRef(coinbase));
    BOOST_REQUIRE(CTransaction(coinbase).IsCoinBase());

    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(3)));
    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(5)));
    block.vtx.push_back(MakeTransactionRef(MakeTxWithInputs(0)));   // degenerate, has no inputs to sum

    BOOST_CHECK_EQUAL(GetBlockInputCount(block), 8U);   // 3 + 5 + 0, NOT +1 for the coinbase
}

static CBlock MakeBudgetTestBlockHeader() {
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = uint256S("0xbeef");
    block.nTime = 1700000000;
    block.nBits = 0x207fffff;
    block.nNonce = 7;
    block.vtx.push_back(MakeTransactionRef(MakeCoinbase()));
    return block;
}

// A transaction with 1 real input and a padded scriptPubKey, sized so many
// of these trip the BYTE cap (COMMITMENT_BUDGET_BODY_BYTES) while their
// aggregate input count stays negligible against COMMITMENT_BUDGET_MAX_INPUTS
// -- the two caps must be independently enforceable, not just independently
// selectable in isolation (max_block_size_threshold_selection only proves the
// latter).
static CMutableTransaction MakeBigLowInputTx(unsigned int txIndex) {
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", txIndex + 1)), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1000;
    std::vector<unsigned char> padding(999800, 0);
    tx.vout[0].scriptPubKey = CScript() << padding << OP_DROP << OP_TRUE;
    return tx;
}

// B6 (F-121, full-arc adversarial review): a well-over-budget case used to sit
// here (checkblock_rejects_aggregate_input_count_over_the_budget, 7,001 x
// 100-input transactions), proving CheckBlock enforces the aggregate cap, not
// just that GetBlockInputCount counts correctly in isolation. Removed as
// redundant: the boundary test below proves the identical rejection at
// exactly +1 over the cap, which (a strict '>' comparison) implies rejection
// at any larger overshoot -- strictly more informative, not merely equivalent.
//
// The boundary itself: enforcement is a strict '>' (validation.cpp), so
// exactly COMMITMENT_BUDGET_MAX_INPUTS must be legal and one more must not.
// An off-by-one ('>=' instead of '>', or vice versa) on a consensus limit is
// exactly the bug class worth pinning directly rather than trusting the
// "well over the cap" test above to imply it.
BOOST_AUTO_TEST_CASE(checkblock_input_count_boundary_is_exact) {
    const unsigned int kInputsPerTx = 100;
    const unsigned int kTxCountAtCap = COMMITMENT_BUDGET_MAX_INPUTS / kInputsPerTx;   // 7,000 * 100 = 700,000
    BOOST_REQUIRE_EQUAL(kTxCountAtCap * kInputsPerTx, COMMITMENT_BUDGET_MAX_INPUTS);

    auto buildBlock = [&](unsigned int txCount, unsigned int extraOneInputTx) {
        CBlock block = MakeBudgetTestBlockHeader();
        for (unsigned int t = 0; t < txCount; t++) {
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
        for (unsigned int e = 0; e < extraOneInputTx; e++) {
            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].prevout = COutPoint(uint256S(strprintf("%08xff%054x", txCount, e + 1)), 0);
            tx.vout.resize(1);
            tx.vout[0].nValue = 1000;
            tx.vout[0].scriptPubKey << OP_TRUE;
            block.vtx.push_back(MakeTransactionRef(tx));
        }
        return block;
    };

    {
        CBlock atCap = buildBlock(kTxCountAtCap, 0);
        BOOST_REQUIRE_EQUAL(GetBlockInputCount(atCap), COMMITMENT_BUDGET_MAX_INPUTS);

        CommitmentBudgetGuard guard;
        g_commitmentBudgetActive = true;
        CValidationState state;
        bool accepted = CheckBlock(atCap, state, Params().GetConsensus(), /*nHeight=*/1,
                                   /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);
        BOOST_CHECK(accepted);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "");
    }
    {
        CBlock overCap = buildBlock(kTxCountAtCap, 1);
        BOOST_REQUIRE_EQUAL(GetBlockInputCount(overCap), COMMITMENT_BUDGET_MAX_INPUTS + 1);

        CommitmentBudgetGuard guard;
        g_commitmentBudgetActive = true;
        CValidationState state;
        bool accepted = CheckBlock(overCap, state, Params().GetConsensus(), /*nHeight=*/1,
                                   /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);
        BOOST_CHECK(!accepted);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-inputs");
    }
}

// The body-byte cap is a SEPARATE ceiling from the input-count cap -- a block
// can be low-input (well under COMMITMENT_BUDGET_MAX_INPUTS) and still be
// rejected purely on bytes. Without this, nothing proves
// COMMITMENT_BUDGET_BODY_BYTES is actually enforced end to end (only that
// MaxBlockSize(true,true) returns the right constant in isolation).
BOOST_AUTO_TEST_CASE(checkblock_enforces_the_body_byte_budget_when_active) {
    CBlock block = MakeBudgetTestBlockHeader();

    const unsigned int kBigTxCount = 115;   // ~999,850 B/tx => ~115 MB > 110 MB budget
    for (unsigned int t = 0; t < kBigTxCount; t++)
        block.vtx.push_back(MakeTransactionRef(MakeBigLowInputTx(t)));

    BOOST_REQUIRE_LT(GetBlockInputCount(block), COMMITMENT_BUDGET_MAX_INPUTS / 1000);
    uint64_t serializedSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    BOOST_REQUIRE_GT(serializedSize, COMMITMENT_BUDGET_BODY_BYTES);

    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;
    CValidationState state;
    bool accepted = CheckBlock(block, state, Params().GetConsensus(), /*nHeight=*/1,
                               /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);

    BOOST_CHECK(!accepted);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
}

// Catches the byte-cap gate being "stuck open" -- e.g. reverting
// MaxBlockSize(true, g_commitmentBudgetActive) back to an ungated call at
// either CheckBlock or ContextualCheckBlock's size-limit site. With the
// budget explicitly OFF, a block between the old 8 MB DIP0001 cap and the
// 110 MB budget cap must still be rejected under the OLD, smaller cap; a
// "stuck open" mutant would instead accept it.
BOOST_AUTO_TEST_CASE(checkblock_byte_cap_is_gated_by_the_budget_flag_when_off) {
    CBlock block = MakeBudgetTestBlockHeader();

    const unsigned int kMidTxCount = 51;   // ~999,850 B/tx => ~51 MB: over 8 MB, under 110 MB
    for (unsigned int t = 0; t < kMidTxCount; t++)
        block.vtx.push_back(MakeTransactionRef(MakeBigLowInputTx(t)));

    uint64_t serializedSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    BOOST_REQUIRE_GT(serializedSize, MAX_DIP0001_BLOCK_SIZE);
    BOOST_REQUIRE_LT(serializedSize, COMMITMENT_BUDGET_BODY_BYTES);

    BOOST_REQUIRE(!g_commitmentBudgetActive);   // explicit: this is the off state
    CValidationState state;
    bool accepted = CheckBlock(block, state, Params().GetConsensus(), /*nHeight=*/1,
                               /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);

    BOOST_CHECK(!accepted);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
}

BOOST_AUTO_TEST_SUITE_END()
