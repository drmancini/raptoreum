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

/** Fable review (2026-09-30), CONFIRMED HIGH, fixed here: the attested
 *  message cannot be tx.GetHash() itself. CTransaction::Serialize writes
 *  vExtraPayload unconditionally whenever nType != TRANSACTION_NORMAL
 *  (primitives/transaction.h) with no SER_GETHASH exclusion at that layer,
 *  and vExtraPayload IS this type's own CAttestationPayload -- which
 *  carries the very signature being verified. tx.GetHash() therefore
 *  depends on payload.sig, and a signer cannot choose a signature whose
 *  value determines the message it is supposed to be a signature over.
 *  No valid attestation could ever have been constructed against the
 *  unfixed version; this is why CProRegTx/CProUpServTx (evo/providertx.h)
 *  exclude their own signature field under SER_GETHASH -- but that guard
 *  only matters when the PAYLOAD OBJECT is itself the thing being
 *  SerializeHash'd. Here the payload is pre-flattened into opaque
 *  vExtraPayload bytes before the OUTER transaction is hashed, so a guard
 *  inside CAttestationPayload's own SERIALIZE_METHODS would never be
 *  consulted -- the fix has to strip vExtraPayload from a COPY of the
 *  transaction before hashing, at the outer layer, not add a guard the
 *  outer layer never triggers. */
uint256 ComputeAttestedMessageHash(const CTransaction &tx) {
    CMutableTransaction stripped(tx);
    stripped.vExtraPayload.clear();
    return ::SerializeHash(CTransaction(stripped));
}

/** id/msgHash are both derived from the payload-stripped transaction hash
 *  above, never carried on the wire (CAttestationPayload's own doc
 *  comment explains why nSignHeight, the one field that IS carried, is
 *  carried) -- id additionally mixes in the domain-separation prefix
 *  above so a signature produced for this purpose cannot be replayed as
 *  one of a different kind sharing the same quorum. */
static uint256 BuildAttestationId(const uint256 &strippedTxHash) {
    return ::SerializeHash(std::make_pair(ATTESTATION_REQUESTID_PREFIX, strippedTxHash));
}

/** Fable review (2026-09-30), CONFIRMED MEDIUM, fixed here: bounding the
 *  explicitly-carried nSignHeight (CAttestationPayload's own doc comment)
 *  against the block actually being validated, rather than trusting it
 *  unconditionally. Deliberately simple for v1, not islock's own
 *  cycleHash/dkgInterval scheme (quorums_instantsend.cpp) -- a real
 *  attestation-cycle concept is 5.4.4's own design job, not invented
 *  here. This only rejects a height that is structurally impossible
 *  (negative, or after the block being validated) or implausibly stale;
 *  it does not attempt to reconstruct which quorum rotation was live. */
static const int32_t MAX_ATTESTATION_SIGN_HEIGHT_AGE = 576; // ~1 day at 2.5 min/block, an interim bound only

static bool IsPlausibleAttestationSignHeight(int32_t nSignHeight, const CBlockIndex *pindexPrev) {
    if (nSignHeight < 0 || pindexPrev == nullptr) {
        return false;
    }
    if (nSignHeight > pindexPrev->nHeight) {
        return false;
    }
    return pindexPrev->nHeight - nSignHeight <= MAX_ATTESTATION_SIGN_HEIGHT_AGE;
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

    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: this loop's own
    // Coin lookup is NOT redundant with Consensus::CheckTxInputs the way
    // the comment below used to claim -- ATMP passes CoinsTip() to
    // CheckSpecialTx (validation.cpp), deliberately mempool-blind,
    // while CheckTxInputs itself ran against the mempool-backed view; in
    // ConnectBlock, ProcessSpecialTxsInBlock runs BEFORE the per-tx loop's
    // own CheckTxInputs/UpdateCoins, not after. Spending an unconfirmed
    // parent -- an attested transaction chained off another one still in
    // the mempool -- genuinely reaches this branch; it is not a "something
    // else already broke" signal. Treated as a real, if narrow, consensus
    // restriction (attested inputs must already be confirmed) rather than
    // an impossible case, matching this file's own single-signature
    // restriction in kind: a deliberate v1 narrowing, named as one.
    for (const CTxIn &txin: tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-unconfirmed-input", false,
                             "TRANSACTION_ATTESTED inputs must already be confirmed (5.4.1)");
        }
        if (!IsRestrictedSingleSigScript(coin.out.scriptPubKey)) {
            // Fable review (2026-09-30), LOW, fixed: this rule is enforced
            // inside ConnectBlock (consensus), not mempool policy --
            // REJECT_NONSTANDARD is policy vocabulary (BIP61 messages, log
            // triage) and misleading here.
            return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-nonstandard-input", false,
                             "TRANSACTION_ATTESTED is restricted to single-signature inputs (5.4.1)");
        }
    }

    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: payload presence
    // and version are structural rules, not signature verification --
    // every sibling CheckXxxTx function (CheckProRegTx et al.) parses and
    // validates its own payload unconditionally and gates ONLY the
    // signature check on check_sigs. The previous version of this function
    // returned true here before ever calling GetTxPayload, so a
    // completely payload-less attested transaction could pass under
    // assumevalid/RollforwardBlock's check_sigs=false while a fully-
    // validating node would reject it -- moved below the structural
    // checks to match the real sibling convention this file's own
    // comment already claimed to follow.
    CAttestationPayload payload;
    if (!GetTxPayload(tx, payload)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload");
    }
    if (payload.nVersion == 0 || payload.nVersion > CAttestationPayload::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload-version");
    }
    if (!IsPlausibleAttestationSignHeight(payload.nSignHeight, pindexPrev)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-payload-height");
    }

    if (!check_sigs) {
        // Matches every sibling CheckXxxTx function's own convention
        // (CheckProRegTx et al.): callers that have already verified
        // signatures elsewhere (or deliberately do not need to, e.g. a
        // -reindex replay of already-connected history) may skip doing it
        // again here -- now correctly only skipping the signature check
        // itself, not the structural payload checks above.
        return true;
    }

    const uint256 msgHash = ComputeAttestedMessageHash(tx);
    const uint256 id = BuildAttestationId(msgHash);
    // v1's own interim choice (this file's own header doc comment):
    // ChainLocks' existing, already-live llmqType, not a new one stood up
    // for this purpose -- confirmed a real ::Consensus field by direct
    // read of chainparams.cpp before relying on it, matching every other
    // call site that already trusts it (quorums_chainlocks.cpp).
    const Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeChainLocks;
    if (!llmq::CSigningManager::VerifyRecoveredSig(llmqType, payload.nSignHeight, id, msgHash, payload.sig)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-attestation");
    }

    return true;
}
