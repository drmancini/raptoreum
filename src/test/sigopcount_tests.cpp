// Copyright (c) 2012-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/tx_verify.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <key.h>
#include <script/script.h>
#include <script/standard.h>
#include <uint256.h>
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
BOOST_AUTO_TEST_CASE(accurate_count_coinbase_has_no_spend_side) {
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);

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

BOOST_AUTO_TEST_SUITE_END()
