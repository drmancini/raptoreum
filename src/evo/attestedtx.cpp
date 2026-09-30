// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/attestedtx.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/quorums_signing.h>
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

/** 5.4.2: mirrors CLSIG_REQUESTID_PREFIX's own established shape exactly
 *  (llmq/quorums_chainlocks.cpp, "clsig", domain-separating a chainlock's
 *  own signing session from anything else that might share a quorum type)
 *  -- a different prefix here means an attestation signature can never be
 *  replayed as, or confused with, a chainlock or islock signature over the
 *  coincidentally-identical hash of some other object, even though this
 *  type currently reuses ChainLocks' own llmqType (see this file's own
 *  header doc comment for why that reuse, not a new quorum type, is v1's
 *  own interim choice). */
static const std::string ATTESTATION_REQUESTID_PREFIX = "atx";

/** id/msgHash are both derived from the transaction's own hash, never
 *  carried on the wire (CAttestationPayload's own doc comment explains
 *  why) -- id additionally mixes in the domain-separation prefix above so
 *  a signature produced for this purpose cannot be replayed as one of a
 *  different kind sharing the same quorum. */
static uint256 BuildAttestationId(const uint256 &txHash) {
    return ::SerializeHash(std::make_pair(ATTESTATION_REQUESTID_PREFIX, txHash));
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

    if (!check_sigs) {
        // Matches every sibling CheckXxxTx function's own convention
        // (CheckProRegTx et al.): callers that have already verified
        // signatures elsewhere (or deliberately do not need to, e.g. a
        // -reindex replay of already-connected history) may skip doing it
        // again here.
        return true;
    }

    CAttestationPayload payload;
    if (!GetTxPayload(tx, payload)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload");
    }
    if (payload.nVersion == 0 || payload.nVersion > CAttestationPayload::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload-version");
    }

    const uint256 txHash = tx.GetHash();
    const uint256 id = BuildAttestationId(txHash);
    const int signedAtHeight = pindexPrev ? pindexPrev->nHeight : 0;
    // v1's own interim choice (this file's own header doc comment):
    // ChainLocks' existing, already-live llmqType, not a new one stood up
    // for this purpose -- confirmed a real ::Consensus field by direct
    // read of chainparams.cpp before relying on it, matching every other
    // call site that already trusts it (quorums_chainlocks.cpp).
    const Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeChainLocks;
    if (!llmq::CSigningManager::VerifyRecoveredSig(llmqType, signedAtHeight, id, txHash, payload.sig)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-attestation");
    }

    return true;
}
