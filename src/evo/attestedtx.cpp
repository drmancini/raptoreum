// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/attestedtx.h>

#include <coins.h>
#include <consensus/validation.h>
#include <script/standard.h>

/** build-plan.md's own 5.4 row, interim decision (a): restricted to
 *  single-signature (P2PK/P2PKH) inputs only until non-standard-script
 *  handling is actually designed -- rejecting anything else at the type
 *  level rather than silently under-checking it. `Solver` is the existing,
 *  already-tested script classifier (script/standard.h/.cpp) reused here,
 *  not reimplemented. */
static bool IsRestrictedSingleSigScript(const CScript &scriptPubKey) {
    std::vector <std::vector<unsigned char>> solutions;
    txnouttype type = Solver(scriptPubKey, solutions);
    return type == TX_PUBKEY || type == TX_PUBKEYHASH;
}

bool CheckAttestedTx(const CTransaction &tx, const CBlockIndex *pindexPrev, CValidationState &state,
                     const CCoinsViewCache &view, bool check_sigs) {
    if (tx.nType != TRANSACTION_ATTESTED) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-type");
    }

    if (tx.IsCoinBase()) {
        // Consensus::CheckTxInputs' own value-conservation/double-spend
        // checks (this file's own doc comment explains why they are not
        // duplicated here) have nothing to apply to a coinbase, and the
        // script-shape restriction below is meaningless for an input with
        // no real prevout -- CheckSpecialTx never reaches nType-specific
        // dispatch for a coinbase transaction today (TRANSACTION_COINBASE
        // is its own, separate type), so this is unreachable in practice;
        // refused explicitly rather than assumed unreachable forever.
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-coinbase");
    }

    if (tx.vin.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-no-inputs");
    }

    for (const CTxIn &txin: tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            // Consensus::CheckTxInputs (validation.cpp, called before
            // CheckSpecialTx's own dispatch in both ATMP and ConnectBlock)
            // already refuses a transaction whose inputs are missing or
            // spent, for every nType -- reaching this branch would mean
            // that guarantee no longer holds, not that this function found
            // a new problem. Fails closed rather than reading a spent/
            // null Coin's own default-constructed fields as if they were
            // real.
            return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-missing-input");
        }
        if (!IsRestrictedSingleSigScript(coin.out.scriptPubKey)) {
            return state.DoS(10, false, REJECT_NONSTANDARD, "bad-attested-tx-nonstandard-input", false,
                             "TRANSACTION_ATTESTED is restricted to single-signature inputs (5.4.1)");
        }
    }

    // 5.4.2 (not yet built, build-plan.md): the quorum attestation
    // signature itself -- needs a payload format and an llmqType decision,
    // neither of which exist yet. check_sigs is accepted as a parameter
    // now, matching every sibling CheckXxxTx function's own convention
    // (CheckProRegTx et al.), specifically so 5.4.2's own attestation
    // check has an established place to land without changing this
    // function's signature again.
    (void) check_sigs;
    (void) pindexPrev;

    return true;
}
