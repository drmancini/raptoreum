// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 5.4.3 (F-242): proves the real per-height gate ContextualCheckTransaction
// (validation.cpp) now applies to TRANSACTION_ATTESTED -- rejected before
// EUpdate::ATTESTED_TX activates, allowed through once it does, exactly at
// the boundary height and nowhere else. Mirrors
// commitmentmode_activation_tests.cpp's own ActivationHeightGuard technique
// (Update's heightActivated constructor parameter bypasses the whole voting
// simulation, confirmed by reading UpdateManager::State's own
// `if (update->HeightActivated() >= 0)` early return, update/update.cpp,
// before relying on it) rather than mining a real multi-round voting chain.
//
// A bare CBlockIndex with only nHeight set is sufficient here (unlike a test
// of the ordinary, non-forced voting path, which walks GetAncestor()): the
// HeightActivated() >= 0 branch returns directly from blockIndex->nHeight,
// never touching GetAncestor -- confirmed by the same read above. This
// function is also called directly, never through ConnectBlock, so no real
// mined chain or coins view is needed at all.

#include <chainparams.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/test_raptoreum.h>
#include <update/update.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

namespace attestedtx_activation_test_helpers {

// A huge, never-reached height -- mirrors
// commitmentmode_activation_tests.cpp's own COMMITMENT_MODE_TEST_NEVER_HEIGHT
// constant, duplicated locally per this codebase's own per-file convention.
static const int64_t ATTESTED_TX_TEST_NEVER_HEIGHT = 2000000000;

static Update MakeAttestedTxUpdate(int64_t heightActivated) {
    return Update(EUpdate::ATTESTED_TX, "Attested Transaction (test)", 4, 1, 0, 1, 1, 0, false,
                 VoteThreshold(0, 0, 1), VoteThreshold(0, 0, 1), false, heightActivated);
}

struct AttestedTxActivationGuard {
    explicit AttestedTxActivationGuard(int64_t heightActivated) {
        Updates().Add(MakeAttestedTxUpdate(heightActivated));
    }
    ~AttestedTxActivationGuard() {
        Updates().Add(MakeAttestedTxUpdate(ATTESTED_TX_TEST_NEVER_HEIGHT));
    }
};

CBlockIndex MakeIndexAtHeight(int nHeight) {
    CBlockIndex idx;
    idx.nHeight = nHeight;
    return idx;
}

CMutableTransaction MakeMinimalAttestedTx() {
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ATTESTED;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    return tx;
}

}  // namespace attestedtx_activation_test_helpers

using namespace attestedtx_activation_test_helpers;

BOOST_FIXTURE_TEST_SUITE(attestedtx_activation_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(contextual_check_transaction_rejects_attested_tx_before_activation) {
    // Regtest's own real registration (chainparams.cpp) is already present
    // (BasicTestingSetup selects regtest params), with its normal
    // structurally-unreachable startHeight -- no guard needed to prove
    // "not active yet" at a realistic height.
    CMutableTransaction tx = MakeMinimalAttestedTx();
    CBlockIndex pindexPrev = MakeIndexAtHeight(500);
    CValidationState state;
    BOOST_CHECK(!ContextualCheckTransaction(CTransaction(tx), state, Params().GetConsensus(), &pindexPrev));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type");
}

BOOST_AUTO_TEST_CASE(contextual_check_transaction_allows_attested_tx_at_and_after_forced_activation) {
    AttestedTxActivationGuard guard(1000);
    CMutableTransaction tx = MakeMinimalAttestedTx();

    CBlockIndex pindexAt = MakeIndexAtHeight(1000);
    CValidationState stateAt;
    BOOST_CHECK(ContextualCheckTransaction(CTransaction(tx), stateAt, Params().GetConsensus(), &pindexAt));

    CBlockIndex pindexAfter = MakeIndexAtHeight(5000);
    CValidationState stateAfter;
    BOOST_CHECK(ContextualCheckTransaction(CTransaction(tx), stateAfter, Params().GetConsensus(), &pindexAfter));
}

BOOST_AUTO_TEST_CASE(contextual_check_transaction_still_rejects_attested_tx_one_block_before_forced_activation) {
    // The exact boundary: HeightActivated()=1000 means height 999 is still
    // Defined (update/update.cpp's own strict `>=` comparison), not Active.
    AttestedTxActivationGuard guard(1000);
    CMutableTransaction tx = MakeMinimalAttestedTx();

    CBlockIndex pindexBefore = MakeIndexAtHeight(999);
    CValidationState state;
    BOOST_CHECK(!ContextualCheckTransaction(CTransaction(tx), state, Params().GetConsensus(), &pindexBefore));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type");
}

BOOST_AUTO_TEST_CASE(contextual_check_transaction_still_rejects_every_other_type_when_attested_tx_is_active) {
    // The new fAttestedTxAllowed clause must not accidentally widen the
    // whitelist for anything else -- an ordinary version-3 transaction
    // carrying an unrecognized nType is still rejected the same as before,
    // activation or not.
    AttestedTxActivationGuard guard(1000);
    CMutableTransaction tx = MakeMinimalAttestedTx();
    tx.nType = 250; // not a real type, matches no whitelist entry

    CBlockIndex pindexAt = MakeIndexAtHeight(5000);
    CValidationState state;
    BOOST_CHECK(!ContextualCheckTransaction(CTransaction(tx), state, Params().GetConsensus(), &pindexAt));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type");
}

BOOST_AUTO_TEST_SUITE_END()
