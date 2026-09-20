// Copyright (c) 2012-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <key.h>
#include <script/script.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <uint256.h>
#include <validation.h>
#include <test/test_raptoreum.h>

#include <vector>

#include <boost/test/unit_test.hpp>

// Helpers:
static std::vector<unsigned char>
Serialize(const CScript &s) {
    std::vector<unsigned char> sSerialized(s.begin(), s.end());
    return sSerialized;
}

BOOST_FIXTURE_TEST_SUITE(sigopcount_tests, BasicTestingSetup
)

BOOST_AUTO_TEST_CASE(GetSigOpCount)
        {
                // Test CScript::GetSigOpCount()
                CScript s1;
        BOOST_CHECK_EQUAL(s1.GetSigOpCount(false), 0U);
        BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 0U);

        uint160 dummy;
        s1 << OP_1 << ToByteVector(dummy) << ToByteVector(dummy) << OP_2 << OP_CHECKMULTISIG;
        BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 2U);
        s1 << OP_IF << OP_CHECKSIG << OP_ENDIF;
        BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 3U);
        BOOST_CHECK_EQUAL(s1.GetSigOpCount(false), 21U);

        CScript p2sh = GetScriptForDestination(CScriptID(s1));
        CScript scriptSig;
        scriptSig << OP_0 << Serialize(s1);
        BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig), 3U);

        std::vector<CPubKey> keys;
        for (int i = 0; i < 3; i++)
        {
            CKey k;
            k.MakeNewKey(true);
            keys.push_back(k.GetPubKey());
        }
        CScript s2 = GetScriptForMultisig(1, keys);
        BOOST_CHECK_EQUAL(s2.GetSigOpCount(true), 3U);
        BOOST_CHECK_EQUAL(s2.GetSigOpCount(false), 20U);

        p2sh = GetScriptForDestination(CScriptID(s2));
        BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(true), 0U);
        BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(false), 0U);
        CScript scriptSig2;
        scriptSig2 << OP_1 << ToByteVector(dummy) << ToByteVector(dummy) << Serialize(s2);
        BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig2), 3U);
        }

// 1.2 (D-17, F-86, F-87): GetAccurateSigOpCount, closing the counting gaps
// GetLegacySigOpCount + GetP2SHSigOpCount leave open before the block resource
// budget can be re-based. A new path -- these tests also pin that the existing
// counters are UNCHANGED, since pre-fork validation still depends on them.

static std::vector<CPubKey> MakeKeys(int n) {
    std::vector<CPubKey> keys;
    for (int i = 0; i < n; i++) {
        CKey k;
        k.MakeNewKey(true);
        keys.push_back(k.GetPubKey());
    }
    return keys;
}

// F-13/F-90: a spend of a BARE (non-P2SH) multisig output is invisible to
// GetLegacySigOpCount (which never reads the spent script) and to
// GetP2SHSigOpCount (which only reads P2SH prevouts). The real CHECKMULTISIG
// lives in the prevout's own scriptPubKey, not in this transaction's scriptSig
// or its own outputs.
BOOST_AUTO_TEST_CASE(accurate_count_sees_a_bare_multisig_spend) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CPubKey> keys = MakeKeys(15);
    CScript bareMultisig = GetScriptForMultisig(1, keys);   // 1-of-15, NOT P2SH-wrapped

    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = bareMultisig;
    txFrom.vout[0].nValue = 1000;
    AddCoins(coins, CTransaction(txFrom), 0);

    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].scriptSig << OP_0 << std::vector<unsigned char>(72, 0);   // just a signature push
    txTo.vout.resize(1);
    txTo.vout[0].scriptPubKey << OP_TRUE;

    const CTransaction tx(txTo);
    BOOST_CHECK_EQUAL(GetLegacySigOpCount(tx) + GetP2SHSigOpCount(tx, coins), 0U);   // the hole
    BOOST_CHECK_EQUAL(GetAccurateSigOpCount(tx, coins), 15U);                        // closed
}

// F-86, corrected: the attack does not need a pre-existing bare multisig
// output. Embedding the opcode directly in the spender's OWN scriptSig,
// against a trivial prevout (e.g. OP_TRUE) the attacker controls, must still
// be counted -- via the retained scriptSig term, not the spent-script term.
BOOST_AUTO_TEST_CASE(accurate_count_sees_a_scriptsig_embedded_multisig) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);

    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey << OP_TRUE;   // trivial: zero sigops of its own
    txFrom.vout[0].nValue = 1000;
    AddCoins(coins, CTransaction(txFrom), 0);

    std::vector<CPubKey> keys = MakeKeys(3);
    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].scriptSig << std::vector<unsigned char>(72, 0)
                          << OP_1 << ToByteVector(keys[0]) << ToByteVector(keys[1]) << ToByteVector(keys[2])
                          << OP_3 << OP_CHECKMULTISIG;
    txTo.vout.resize(1);
    txTo.vout[0].scriptPubKey << OP_TRUE;

    const CTransaction tx(txTo);
    BOOST_CHECK_EQUAL(GetAccurateSigOpCount(tx, coins), 3U);
}

// For an input whose prevout genuinely is P2SH, extending coverage past
// P2SH-only must not change the answer the existing path already got right.
BOOST_AUTO_TEST_CASE(accurate_count_agrees_with_the_p2sh_path_for_p2sh_inputs) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CPubKey> keys = MakeKeys(15);
    CScript redeem = GetScriptForMultisig(1, keys);
    CScript p2sh = GetScriptForDestination(CScriptID(redeem));

    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = p2sh;
    txFrom.vout[0].nValue = 1000;
    AddCoins(coins, CTransaction(txFrom), 0);

    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].scriptSig << OP_0 << std::vector<unsigned char>(redeem.begin(), redeem.end());
    txTo.vout.resize(1);
    txTo.vout[0].scriptPubKey << OP_TRUE;

    const CTransaction tx(txTo);
    BOOST_CHECK_EQUAL(GetP2SHSigOpCount(tx, coins), 15U);
    BOOST_CHECK_EQUAL(GetAccurateSigOpCount(tx, coins), 15U);
}

// A coinbase has no spend side at all -- must not look up its null prevout,
// and must count only its own outputs, mirroring GetTransactionSigOpCount's
// own coinbase early-return.
//
// F2 (1.2 review, 2026-09-19): the original version of this test passed even
// with the IsCoinBase() guard deleted, because AccessCoin on a genuinely
// missing entry returns an empty Coin whose scriptPubKey counts 0 -- the
// guard's absence was invisible. Planting a real, expensive coin at the null
// outpoint means a deleted guard now has something to find.
BOOST_AUTO_TEST_CASE(accurate_count_coinbase_has_no_spend_side) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CPubKey> keys = MakeKeys(15);
    coins.AddCoin(COutPoint(), Coin(CTxOut(1000, GetScriptForMultisig(1, keys)), 0, false, 0, {}), true);

    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vin[0].scriptSig << OP_1;
    cb.vout.resize(1);
    cb.vout[0].scriptPubKey << OP_CHECKSIG;

    const CTransaction tx(cb);
    BOOST_REQUIRE(tx.IsCoinBase());
    BOOST_CHECK_EQUAL(GetAccurateSigOpCount(tx, coins), 1U);
}

// F-87: OP_CHECKDATASIG(VERIFY) is counted only when explicitly asked for, and
// existing call sites (which never pass fCountDataSig) must see NO change --
// GetSigOpCount is shared with pre-fork validation and must not be altered
// retroactively for history already on chain.
BOOST_AUTO_TEST_CASE(checkdatasig_is_opt_in_only) {
    CScript sig;
    sig << OP_CHECKDATASIG;
    BOOST_CHECK_EQUAL(sig.GetSigOpCount(true), 0U);
    BOOST_CHECK_EQUAL(sig.GetSigOpCount(false), 0U);
    BOOST_CHECK_EQUAL(sig.GetSigOpCount(/*fAccurate=*/true, /*fCountDataSig=*/true), 1U);

    CScript verify;
    verify << OP_CHECKDATASIGVERIFY;
    BOOST_CHECK_EQUAL(verify.GetSigOpCount(true), 0U);
    BOOST_CHECK_EQUAL(verify.GetSigOpCount(true, true), 1U);
}

// F-87, one level up (1.2 test review, 2026-09-19): the test above only proves
// CScript::GetSigOpCount itself counts OP_CHECKDATASIG(VERIFY) when asked. It
// says nothing about whether the three fCountDataSig=true call sites inside
// GetAccurateOwnSigOpCount/GetAccurateSigOpCount are actually there -- drop
// any one of them and every other sigop test still passes, since none of them
// puts a CHECKDATASIG(VERIFY) in a scriptSig, a created output AND a spent
// script simultaneously. Distinct weights per component (1, 2, 4) make a
// dropped site identifiable from the failing value alone.
BOOST_AUTO_TEST_CASE(checkdatasig_is_counted_by_the_accurate_counters) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);

    CScript prevoutScript;
    for (int i = 0; i < 4; i++) prevoutScript << OP_CHECKDATASIGVERIFY;   // weight 4, spent side

    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = prevoutScript;
    txFrom.vout[0].nValue = 1000;
    AddCoins(coins, CTransaction(txFrom), 0);

    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].scriptSig << OP_CHECKDATASIG;                            // weight 1, own scriptSig
    txTo.vout.resize(1);
    txTo.vout[0].scriptPubKey << OP_CHECKDATASIG << OP_CHECKDATASIG;     // weight 2, own created output

    const CTransaction ctx(txTo);
    BOOST_CHECK_EQUAL(GetLegacySigOpCount(ctx), 0U);              // legacy never sees CHECKDATASIG at all
    BOOST_CHECK_EQUAL(GetAccurateOwnSigOpCount(ctx), 3U);         // scriptSig(1) + created output(2)
    BOOST_CHECK_EQUAL(GetAccurateSigOpCount(ctx, coins), 7U);     // + spent script(4)
}

// F1 (Fable review of R-33, 2026-09-19): the view-less CheckBlock/ContextualCheckBlock
// pre-checks kept using GetLegacySigOpCount under the commitment budget, on the
// B6 (F-121, full-arc adversarial review): a standalone N=3 case used to sit
// here (legacy_drastically_overcounts_a_small_created_multisig_output) -- the
// exact bare-multisig-output sub-case the matrix test below already sweeps at
// n=3, among every other N from 1 to 16. Removed as redundant; the matrix
// below is strictly more thorough, not merely equivalent.
//
// Characterisation matrix (2026-09-19): a systematic sweep across multisig
// size, not a handful of hand-picked points. F-95 (legacy overcounting a small
// created multisig output) existed at EVERY N from 1 to 16 and none of the
// tests above -- each written for one specific scenario -- happened to sweep
// N far enough to notice. Every expected value below is derived by hand from
// the opcode semantics (OP_N encoding, MAX_PUBKEYS_PER_MULTISIG), never by
// calling the function under test to produce its own "expected" value --
// that would just restate the implementation and always pass.
//
// CScript::EncodeOP_N asserts n <= 16 (script.h), so N stops at 16: a bare
// 17-to-20-key multisig cannot even be built via the normal OP_N encoding
// GetScriptForMultisig uses, independent of anything this file tests.
BOOST_AUTO_TEST_CASE(sigop_counting_matrix_across_multisig_sizes) {
    for (int n = 1; n <= 16; n++) {
        BOOST_TEST_CONTEXT("n=" << n)   // names the failing N in a red run instead of just a line number
        {
        std::vector<CPubKey> keys = MakeKeys(n);
        CScript bareRedeem = GetScriptForMultisig(1, keys);
        CScript p2sh = GetScriptForDestination(CScriptID(bareRedeem));

        // --- creating a BARE multisig output ---
        // Legacy's own-script scan uses fAccurate=false, which charges
        // MAX_PUBKEYS_PER_MULTISIG (20) for every CHECKMULTISIG regardless of
        // the real key count -- constant across all N, not the real n.
        {
            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].scriptSig << OP_TRUE;
            tx.vout.resize(1);
            tx.vout[0].scriptPubKey = bareRedeem;
            const CTransaction ctx(tx);
            BOOST_CHECK_EQUAL(GetLegacySigOpCount(ctx), 20U);
            BOOST_CHECK_EQUAL(GetAccurateOwnSigOpCount(ctx), (unsigned int) n);
        }

        // --- creating a P2SH-wrapped multisig output ---
        // The redeem script is not yet revealed; P2SH's own scriptPubKey
        // (OP_HASH160 <hash> OP_EQUAL) has no CHECKSIG/CHECKMULTISIG of its
        // own, so both counters see 0 regardless of N -- the redeem script's
        // real cost only exists once it is spent (below).
        {
            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].scriptSig << OP_TRUE;
            tx.vout.resize(1);
            tx.vout[0].scriptPubKey = p2sh;
            const CTransaction ctx(tx);
            BOOST_CHECK_EQUAL(GetLegacySigOpCount(ctx), 0U);
            BOOST_CHECK_EQUAL(GetAccurateOwnSigOpCount(ctx), 0U);
        }

        // --- spending a BARE multisig output ---
        // F-13's hole: legacy's spend-side blindness means the tx-wide legacy
        // count is just the 1 CHECKSIG in the tx's own P2PKH output, constant
        // across N, never the real n it should include from the spent script.
        {
            CCoinsView coinsDummy;
            CCoinsViewCache coins(&coinsDummy);
            CMutableTransaction txFrom;
            txFrom.vout.resize(1);
            txFrom.vout[0].scriptPubKey = bareRedeem;
            txFrom.vout[0].nValue = 1000;
            AddCoins(coins, CTransaction(txFrom), 0);

            CMutableTransaction txTo;
            txTo.vin.resize(1);
            txTo.vin[0].prevout.hash = txFrom.GetHash();
            txTo.vin[0].prevout.n = 0;
            txTo.vin[0].scriptSig << OP_0 << std::vector<unsigned char>(72, 0);
            txTo.vout.resize(1);
            txTo.vout[0].scriptPubKey = GetScriptForDestination(keys[0].GetID());   // real P2PKH, matches the comment above
            const CTransaction ctx(txTo);

            BOOST_CHECK_EQUAL(GetLegacySigOpCount(ctx) + GetP2SHSigOpCount(ctx, coins), 1U);
            BOOST_CHECK_EQUAL(GetAccurateSigOpCount(ctx, coins), (unsigned int) (1 + n));
        }

        // --- spending a P2SH-wrapped multisig output ---
        // Already correct pre-1.2 (GetP2SHSigOpCount decodes the redeem
        // script accurately): the new counter must reproduce the SAME
        // real-n-dependent answer across the whole range, not just the one
        // N=15 point the earlier test above happens to check.
        {
            CCoinsView coinsDummy;
            CCoinsViewCache coins(&coinsDummy);
            CMutableTransaction txFrom;
            txFrom.vout.resize(1);
            txFrom.vout[0].scriptPubKey = p2sh;
            txFrom.vout[0].nValue = 1000;
            AddCoins(coins, CTransaction(txFrom), 0);

            CMutableTransaction txTo;
            txTo.vin.resize(1);
            txTo.vin[0].prevout.hash = txFrom.GetHash();
            txTo.vin[0].prevout.n = 0;
            txTo.vin[0].scriptSig << OP_0 << std::vector<unsigned char>(bareRedeem.begin(), bareRedeem.end());
            txTo.vout.resize(1);
            txTo.vout[0].scriptPubKey = GetScriptForDestination(keys[0].GetID());   // real P2PKH, matches the comment above
            const CTransaction ctx(txTo);

            unsigned int legacyPlusP2sh = GetLegacySigOpCount(ctx) + GetP2SHSigOpCount(ctx, coins);
            BOOST_CHECK_EQUAL(legacyPlusP2sh, (unsigned int) (1 + n));
            BOOST_CHECK_EQUAL(GetAccurateSigOpCount(ctx, coins), (unsigned int) (1 + n));
        }
        }   // BOOST_TEST_CONTEXT
    }
}

// F1's fix, end to end (1.2 test review, 2026-09-19): every test above proves
// GetAccurateOwnSigOpCount itself is correct in isolation. None of them prove
// CheckBlock actually CALLS it under the commitment budget instead of
// GetLegacySigOpCount -- revert either ternary in validation.cpp back to
// GetLegacySigOpCount and the whole suite stays green, since nothing else
// exercises that call site. This reproduces F1 at the real 250,000 budget:
// 12,501 minimal transactions, each creating one bare 1-of-1 multisig output.
// Legacy charges its constant 20/tx = 250,020 (over budget); the real, accurate
// cost is 1/tx = 12,501 (nowhere close). CheckTransaction never touches the
// UTXO set, so the spam transactions' prevouts can be arbitrary and need not
// exist.
BOOST_AUTO_TEST_CASE(checkblock_uses_accurate_own_count_under_the_budget) {
    std::vector<CPubKey> keys = MakeKeys(1);
    CScript oneOfOne = GetScriptForMultisig(1, keys);

    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = uint256S("0xbeef");
    block.nTime = 1700000000;
    block.nBits = 0x207fffff;
    block.nNonce = 7;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_1;   // CheckTransaction's minCbSize == 2
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 1000;
    coinbase.vout[0].scriptPubKey << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(coinbase));

    const unsigned int kSpamCount = 12501;   // 20/tx legacy => 250,020, just over the 250,000 budget
    for (unsigned int i = 0; i < kSpamCount; i++) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", i + 1)), 0);
        tx.vout.resize(1);
        tx.vout[0].nValue = 1000;
        tx.vout[0].scriptPubKey = oneOfOne;
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    // Confirm the fixture actually reproduces the bug's precondition, so a
    // pass below isn't hiding a miscounted setup rather than a real fix.
    unsigned int legacyTotal = 0;
    for (const auto &tx: block.vtx) legacyTotal += GetLegacySigOpCount(*tx);
    BOOST_REQUIRE_GT(legacyTotal, 250000U);

    g_commitmentBudgetActive = true;
    CValidationState state;
    bool accepted = CheckBlock(block, state, Params().GetConsensus(), /*nHeight=*/1,
                               /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);
    g_commitmentBudgetActive = false;

    BOOST_CHECK(accepted);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "");
}

BOOST_AUTO_TEST_SUITE_END()
